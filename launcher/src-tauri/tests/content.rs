//! Conteúdo do servidor: manifest, download verificado, cache e montagem, contra um `cb-server` falso (axum em
//! 127.0.0.1) com falhas controláveis.

use axum::body::{Body, Bytes};
use axum::extract::{Path as AxPath, State};
use axum::http::{header, HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::Router;
use cbmp_launcher_lib::content::{
    fetch_manifest, server_base_url, validate_manifest, ContentStore, FileRef, Limits, Manifest,
    ManifestFile, Progress, ResourceEntry, MAX_FILES,
};
use cbmp_launcher_lib::error::AppError;
use sha2::{Digest, Sha256};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio_util::sync::CancellationToken;
use url::Url;

const SRV: &str = "srv_teste";
const PROTO: u32 = 3;

fn sha(data: &[u8]) -> String {
    hex(&Sha256::digest(data))
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn blob(seed: u8, len: usize) -> Vec<u8> {
    (0..len)
        .map(|i| (i as u8).wrapping_mul(31) ^ seed)
        .collect()
}

// ----- cb-server falso -----

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Mode {
    /// Serve o arquivo, com suporte a `Range: bytes=N-`.
    Ok,
    /// Bytes trocados, mesmo tamanho.
    Wrong,
    /// Corpo maior que o declarado, com `Content-Length`.
    Oversize,
    /// Corpo maior que o declarado, sem `Content-Length` (chunked).
    OversizeChunked,
    /// 1ª resposta manda metade (do arquivo, ou lixo se `garbage`) e cai; as seguintes seguem `then`.
    Drop { garbage: bool, then: Then },
    /// As N primeiras respostas são 503.
    Fail5xx(u32),
    /// 1 KiB a cada 20 ms, para testar cancelamento.
    Slow,
    /// Redireciona para outro endereço.
    Redirect,
}

/// Resposta depois da queda (pedido com `Range`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Then {
    /// 206 certo.
    Range,
    /// Ignora o Range: 200 com o arquivo inteiro.
    Full,
    /// 206 com `Content-Range` começando em outro lugar.
    BadStart,
    /// 416.
    Status416,
    /// 206 com total diferente do declarado.
    WrongTotal,
    /// Cai de novo, sempre.
    Again,
}

fn drop_half(data: &[u8], garbage: bool) -> Response {
    // Metade, uma pausa (para o hyper enviar o que tem) e a conexão cai.
    let mut half = data[..data.len() / 2].to_vec();
    if garbage {
        half.iter_mut().for_each(|b| *b ^= 0x5a);
    }
    let half = Bytes::from(half);
    let stream = futures_util::stream::unfold(0u8, move |step| {
        let half = half.clone();
        async move {
            match step {
                0 => Some((Ok(half), 1)),
                1 => {
                    tokio::time::sleep(Duration::from_millis(100)).await;
                    Some((Err(std::io::Error::other("queda")), 2))
                }
                _ => None,
            }
        }
    });
    (
        [(header::CONTENT_LENGTH, data.len().to_string())],
        Body::from_stream(stream),
    )
        .into_response()
}

#[derive(Default)]
struct Fake {
    files: HashMap<String, (Vec<u8>, Mode)>,
    hits: HashMap<String, u32>,
    ranges: Vec<(String, Option<String>)>,
    manifest: Option<String>,
    manifest_hits: u32,
}

type Shared = Arc<Mutex<Fake>>;

async fn file_handler(
    State(st): State<Shared>,
    AxPath(sha): AxPath<String>,
    headers: HeaderMap,
) -> Response {
    let range = headers
        .get(header::RANGE)
        .and_then(|v| v.to_str().ok())
        .map(str::to_string);
    let (data, mode, hit) = {
        let mut f = st.lock().unwrap();
        f.ranges.push((sha.clone(), range.clone()));
        let Some((data, mode)) = f.files.get(&sha).cloned() else {
            return StatusCode::NOT_FOUND.into_response();
        };
        let h = f.hits.entry(sha.clone()).or_default();
        *h += 1;
        (data, mode, *h)
    };
    let serve = |data: Vec<u8>| -> Response {
        let start = range
            .as_deref()
            .and_then(|r| r.strip_prefix("bytes="))
            .and_then(|r| r.strip_suffix('-'))
            .and_then(|n| n.parse::<usize>().ok());
        match start {
            Some(s) if s < data.len() => {
                let total = data.len();
                (
                    StatusCode::PARTIAL_CONTENT,
                    [(
                        header::CONTENT_RANGE,
                        format!("bytes {s}-{}/{total}", total - 1),
                    )],
                    data[s..].to_vec(),
                )
                    .into_response()
            }
            _ => data.into_response(),
        }
    };
    match mode {
        Mode::Ok => serve(data),
        Mode::Wrong => {
            let mut d = data;
            d[0] ^= 0xff;
            d.into_response()
        }
        Mode::Oversize => {
            let mut d = data;
            d.extend_from_slice(b"sobra");
            d.into_response()
        }
        Mode::OversizeChunked => {
            let mut d = data;
            d.extend_from_slice(&[7u8; 4096]);
            let chunks: Vec<Result<Bytes, std::io::Error>> = d
                .chunks(1024)
                .map(|c| Ok(Bytes::copy_from_slice(c)))
                .collect();
            Body::from_stream(futures_util::stream::iter(chunks)).into_response()
        }
        Mode::Drop { garbage, then } if hit == 1 || then == Then::Again => {
            drop_half(&data, garbage)
        }
        Mode::Drop { then, .. } => {
            let n = data.len();
            let start = range
                .as_deref()
                .and_then(|r| r.strip_prefix("bytes="))
                .and_then(|r| r.strip_suffix('-'))
                .and_then(|v| v.parse::<usize>().ok());
            match (then, start) {
                (Then::Full, _) => data.into_response(),
                (Then::BadStart, Some(_)) => (
                    StatusCode::PARTIAL_CONTENT,
                    [(header::CONTENT_RANGE, format!("bytes 0-{}/{n}", n - 1))],
                    data,
                )
                    .into_response(),
                (Then::Status416, Some(_)) => StatusCode::RANGE_NOT_SATISFIABLE.into_response(),
                (Then::WrongTotal, Some(s)) => (
                    StatusCode::PARTIAL_CONTENT,
                    [(
                        header::CONTENT_RANGE,
                        format!("bytes {s}-{}/{}", n - 1, n + 1),
                    )],
                    data[s..].to_vec(),
                )
                    .into_response(),
                _ => serve(data),
            }
        }
        Mode::Fail5xx(n) if hit <= n => StatusCode::SERVICE_UNAVAILABLE.into_response(),
        Mode::Fail5xx(_) => serve(data),
        Mode::Slow => {
            let stream = futures_util::stream::unfold(0usize, move |off| {
                let data = data.clone();
                async move {
                    if off >= data.len() {
                        return None;
                    }
                    tokio::time::sleep(Duration::from_millis(20)).await;
                    let end = (off + 1024).min(data.len());
                    Some((
                        Ok::<_, std::io::Error>(Bytes::copy_from_slice(&data[off..end])),
                        end,
                    ))
                }
            });
            Body::from_stream(stream).into_response()
        }
        Mode::Redirect => (
            StatusCode::FOUND,
            [(header::LOCATION, format!("/cb/files/{}", "0".repeat(64)))],
        )
            .into_response(),
    }
}

async fn manifest_handler(State(st): State<Shared>) -> Response {
    let mut f = st.lock().unwrap();
    f.manifest_hits += 1;
    match &f.manifest {
        Some(m) => ([(header::CONTENT_TYPE, "application/json")], m.clone()).into_response(),
        None => StatusCode::NOT_FOUND.into_response(),
    }
}

async fn start(fake: Fake) -> (Url, Shared) {
    let st: Shared = Arc::new(Mutex::new(fake));
    let app = Router::new()
        .route("/cb/files/{sha}", get(file_handler))
        .route("/cb/manifest", get(manifest_handler))
        .with_state(st.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = listener.local_addr().unwrap().port();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    (server_base_url("127.0.0.1", port).unwrap(), st)
}

// ----- manifest e loja -----

fn file(path: &str, data: &[u8]) -> ManifestFile {
    ManifestFile {
        path: path.into(),
        sha256: sha(data),
        size: data.len() as u64,
    }
}

fn manifest(resources: Vec<(&str, Vec<ManifestFile>)>) -> Manifest {
    let total = resources
        .iter()
        .flat_map(|(_, f)| f.iter())
        .map(|f| f.size)
        .sum();
    Manifest {
        server_id: SRV.into(),
        protocol: PROTO,
        resources: resources
            .into_iter()
            .map(|(name, files)| ResourceEntry {
                name: name.into(),
                version: "1.0.0".into(),
                files,
            })
            .collect(),
        total_size: total,
    }
}

struct Env {
    _tmp: tempfile::TempDir,
    cache: PathBuf,
    servers: PathBuf,
    store: ContentStore,
}

fn env() -> Env {
    let tmp = tempfile::tempdir().unwrap();
    let cache = tmp.path().join("cache");
    let servers = tmp.path().join("servers");
    let store = ContentStore::new(cache.clone(), servers.clone())
        .with_retry_delay(Duration::from_millis(5));
    Env {
        _tmp: tmp,
        cache,
        servers,
        store,
    }
}

/// Arquivos sob `cache/tmp` (em qualquer pasta de download).
fn parts(cache: &Path) -> Vec<PathBuf> {
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        let Ok(rd) = std::fs::read_dir(d) else {
            return;
        };
        for e in rd {
            let p = e.unwrap().path();
            if p.is_dir() {
                walk(&p, out);
            } else {
                out.push(p);
            }
        }
    }
    let mut out = vec![];
    walk(&cache.join("tmp"), &mut out);
    out
}

fn limits() -> Limits {
    Limits::new(2048)
}

#[derive(Clone, Default)]
struct Events(Arc<Mutex<Vec<(Instant, Progress)>>>);

impl Events {
    fn sink(&self) -> impl Fn(Progress) + Send + Sync + 'static {
        let ev = self.0.clone();
        move |p| ev.lock().unwrap().push((Instant::now(), p))
    }
    fn all(&self) -> Vec<(Instant, Progress)> {
        self.0.lock().unwrap().clone()
    }
}

async fn download(env: &Env, base: &Url, files: &[FileRef]) -> Result<(), AppError> {
    env.store
        .download(base, files, |_| {}, CancellationToken::new())
        .await
}

// ----- testes -----

#[tokio::test]
async fn baixa_verifica_e_deixa_somente_leitura() {
    let a = blob(1, 200_000);
    let b = blob(2, 3);
    let mut fake = Fake::default();
    fake.files.insert(sha(&a), (a.clone(), Mode::Ok));
    fake.files.insert(sha(&b), (b.clone(), Mode::Ok));
    let (base, st) = start(fake).await;
    let env = env();
    let m = manifest(vec![
        (
            "mapa",
            vec![file("files/a.png", &a), file("client/b.lua", &b)],
        ),
        ("outro", vec![file("ui/a.png", &a)]),
    ]);
    validate_manifest(&m, SRV, PROTO, &limits()).unwrap();

    let missing = env.store.missing(&m);
    assert_eq!(missing.len(), 2, "mesmo hash em dois lugares baixa uma vez");
    let ev = Events::default();
    env.store
        .download(&base, &missing, ev.sink(), CancellationToken::new())
        .await
        .unwrap();

    for (data, h) in [(&a, sha(&a)), (&b, sha(&b))] {
        let p = env.cache.join(&h);
        assert_eq!(&std::fs::read(&p).unwrap(), data);
        assert!(std::fs::metadata(&p).unwrap().permissions().readonly());
    }
    assert!(parts(&env.cache).is_empty());
    assert!(env.store.missing(&m).is_empty());
    let last = ev.all().last().unwrap().1.clone();
    assert_eq!(last.done_bytes, last.total_bytes);
    assert_eq!(last.total_bytes, (a.len() + b.len()) as u64);

    // Nada a baixar: o servidor não é chamado de novo.
    download(&env, &base, &env.store.missing(&m)).await.unwrap();
    assert_eq!(st.lock().unwrap().hits.values().sum::<u32>(), 2);
}

#[tokio::test]
async fn progresso_no_maximo_10_por_segundo() {
    let mut fake = Fake::default();
    let mut files = vec![];
    for i in 0..300u16 {
        let d = format!("arquivo {i}").into_bytes();
        files.push(file(&format!("files/{i}.json"), &d));
        fake.files.insert(sha(&d), (d, Mode::Ok));
    }
    let (base, _) = start(fake).await;
    let env = env();
    let m = manifest(vec![("muitos", files)]);
    let ev = Events::default();
    let t0 = Instant::now();
    env.store
        .download(
            &base,
            &env.store.missing(&m),
            ev.sink(),
            CancellationToken::new(),
        )
        .await
        .unwrap();
    let elapsed = t0.elapsed();
    let all = ev.all();
    let max = (elapsed.as_millis() / 100) as usize + 2;
    assert!(
        all.len() <= max,
        "{} eventos em {elapsed:?} (máx. {max})",
        all.len()
    );
    for w in all.windows(2).take(all.len().saturating_sub(2)) {
        assert!(w[1].0 - w[0].0 >= Duration::from_millis(95));
    }
    assert_eq!(all.last().unwrap().1.done_bytes, m.total_size);
}

#[tokio::test]
async fn hash_errado_nomeia_o_resource_e_apaga_o_part() {
    let a = blob(3, 50_000);
    let mut fake = Fake::default();
    fake.files.insert(sha(&a), (a.clone(), Mode::Wrong));
    let (base, st) = start(fake).await;
    let env = env();
    let m = manifest(vec![("varinhas", vec![file("files/v.png", &a)])]);
    let err = download(&env, &base, &env.store.missing(&m))
        .await
        .unwrap_err();
    assert_eq!(err.code, "hash_mismatch");
    assert!(err.message.contains("varinhas"), "{}", err.message);
    assert!(parts(&env.cache).is_empty());
    assert!(!env.cache.join(sha(&a)).exists());
    assert_eq!(
        st.lock().unwrap().hits[&sha(&a)],
        1,
        "hash errado não repete"
    );
}

#[tokio::test]
async fn corpo_maior_que_o_declarado_aborta() {
    for mode in [Mode::Oversize, Mode::OversizeChunked] {
        let a = blob(4, 10_000);
        let mut fake = Fake::default();
        fake.files.insert(sha(&a), (a.clone(), mode));
        let (base, _) = start(fake).await;
        let env = env();
        let m = manifest(vec![("grande", vec![file("files/g.png", &a)])]);
        let err = download(&env, &base, &env.store.missing(&m))
            .await
            .unwrap_err();
        assert_eq!(err.code, "size_mismatch", "{mode:?}");
        assert!(err.message.contains("grande"));
        assert!(parts(&env.cache).is_empty(), "{mode:?}");
        assert!(!env.cache.join(sha(&a)).exists());
    }
}

#[tokio::test]
async fn queda_no_meio_retoma_por_range() {
    let a = blob(5, 300_000);
    let mut fake = Fake::default();
    fake.files.insert(
        sha(&a),
        (
            a.clone(),
            Mode::Drop {
                garbage: false,
                then: Then::Range,
            },
        ),
    );
    let (base, st) = start(fake).await;
    let env = env();
    let m = manifest(vec![("r", vec![file("files/a.png", &a)])]);
    download(&env, &base, &env.store.missing(&m)).await.unwrap();
    assert_eq!(std::fs::read(env.cache.join(sha(&a))).unwrap(), a);
    let f = st.lock().unwrap();
    assert_eq!(f.ranges.len(), 2);
    assert_eq!(f.ranges[0].1, None);
    let r = f.ranges[1].1.clone().expect("2ª tentativa pede Range");
    let start: usize = r
        .strip_prefix("bytes=")
        .unwrap()
        .strip_suffix('-')
        .unwrap()
        .parse()
        .unwrap();
    assert!(start > 0 && start < a.len(), "{r}");
}

#[tokio::test]
async fn tres_tentativas_com_5xx() {
    let ok = blob(6, 1000);
    let bad = blob(7, 1000);
    let mut fake = Fake::default();
    fake.files.insert(sha(&ok), (ok.clone(), Mode::Fail5xx(2)));
    fake.files
        .insert(sha(&bad), (bad.clone(), Mode::Fail5xx(3)));
    let (base, st) = start(fake).await;

    let env = env();
    let m = manifest(vec![("r", vec![file("files/ok.png", &ok)])]);
    download(&env, &base, &env.store.missing(&m)).await.unwrap();
    assert_eq!(st.lock().unwrap().hits[&sha(&ok)], 3);

    let m = manifest(vec![("quebrado", vec![file("files/bad.png", &bad)])]);
    let err = download(&env, &base, &env.store.missing(&m))
        .await
        .unwrap_err();
    assert_eq!(err.code, "server_unavailable");
    assert!(err.message.contains("quebrado"));
    assert_eq!(st.lock().unwrap().hits[&sha(&bad)], 3, "nunca mais de 3");
}

#[tokio::test]
async fn redirecionamento_nao_e_seguido() {
    let a = blob(8, 100);
    let mut fake = Fake::default();
    fake.files.insert(sha(&a), (a.clone(), Mode::Redirect));
    let (base, st) = start(fake).await;
    let env = env();
    let m = manifest(vec![("r", vec![file("files/a.png", &a)])]);
    let err = download(&env, &base, &env.store.missing(&m))
        .await
        .unwrap_err();
    assert_eq!(err.code, "server_unavailable");
    let f = st.lock().unwrap();
    assert!(
        f.ranges.iter().all(|(s, _)| *s == sha(&a)),
        "não seguiu o Location"
    );
}

#[tokio::test]
async fn cancelamento_limpa_o_part() {
    let a = blob(9, 2_000_000);
    let mut fake = Fake::default();
    fake.files.insert(sha(&a), (a.clone(), Mode::Slow));
    let (base, _) = start(fake).await;
    let env = env();
    let m = manifest(vec![("lento", vec![file("files/a.webm", &a)])]);
    let cancel = CancellationToken::new();
    let ev = Events::default();
    let task = {
        let missing = env.store.missing(&m);
        let store = env.store.clone();
        let cancel = cancel.clone();
        let sink = ev.sink();
        tokio::spawn(async move { store.download(&base, &missing, sink, cancel).await })
    };
    let t0 = Instant::now();
    while !ev.all().iter().any(|(_, p)| p.done_bytes > 0) {
        assert!(t0.elapsed() < Duration::from_secs(10), "sem progresso");
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    assert!(!parts(&env.cache).is_empty(), "baixando em .part");
    cancel.cancel();
    let err = tokio::time::timeout(Duration::from_secs(5), task)
        .await
        .expect("cancelamento rápido")
        .unwrap()
        .unwrap_err();
    assert_eq!(err.code, "cancelled");
    assert!(parts(&env.cache).is_empty());
    assert!(!env.cache.join(sha(&a)).exists());
}

// ----- validação do manifest -----

fn one(path: &str) -> Manifest {
    manifest(vec![("r", vec![file(path, b"x")])])
}

#[test]
fn caminhos_e_nomes_maliciosos_sao_recusados() {
    for p in [
        "../x.lua",
        "client/../../x.lua",
        "/abs.lua",
        "C:\\x.lua",
        "C:/x.lua",
        "a//b.lua",
        "a\\b.lua",
        "",
        "./a.lua",
        "a/",
        "a/./b.lua",
        "a b.lua",
        "CON.lua",
        "files/nul",
        "files/x.",
        "files/ç.png",
    ] {
        let err = validate_manifest(&one(p), SRV, PROTO, &limits()).unwrap_err();
        assert_eq!(err.code, "invalid_manifest", "caminho {p:?} passou");
        assert!(err.message.contains("\"r\""), "{}", err.message);
    }
    let long = "a".repeat(65);
    for name in ["", "..", ".", "a b", "a/b", "a\\b", long.as_str(), "ç"] {
        let m = manifest(vec![(name, vec![file("client/a.lua", b"x")])]);
        let err = validate_manifest(&m, SRV, PROTO, &limits()).unwrap_err();
        assert_eq!(err.code, "invalid_manifest", "nome {name:?} passou");
    }
    // Duplicados (Windows não diferencia maiúsculas), arquivo que é prefixo de pasta, hash inválido.
    for files in [
        vec![file("client/a.lua", b"1"), file("client/A.lua", b"2")],
        vec![file("client/a", b"1"), file("client/a/b.lua", b"2")],
    ] {
        let err =
            validate_manifest(&manifest(vec![("r", files)]), SRV, PROTO, &limits()).unwrap_err();
        assert_eq!(err.code, "invalid_manifest");
    }
    let dup = manifest(vec![
        ("Mapa", vec![file("client/a.lua", b"1")]),
        ("mapa", vec![file("client/b.lua", b"2")]),
    ]);
    assert_eq!(
        validate_manifest(&dup, SRV, PROTO, &limits())
            .unwrap_err()
            .code,
        "invalid_manifest"
    );
    for bad_sha in ["A".repeat(64), "0".repeat(63), "g".repeat(64)] {
        let mut m = one("client/a.lua");
        m.resources[0].files[0].sha256 = bad_sha;
        assert_eq!(
            validate_manifest(&m, SRV, PROTO, &limits())
                .unwrap_err()
                .code,
            "invalid_manifest"
        );
    }
    // Mesmo hash com tamanhos diferentes; totalSize que não bate.
    let mut m = manifest(vec![("r", vec![file("a.lua", b"x"), file("b.lua", b"x")])]);
    m.resources[0].files[1].size = 2;
    m.total_size = 3;
    assert_eq!(
        validate_manifest(&m, SRV, PROTO, &limits())
            .unwrap_err()
            .code,
        "invalid_manifest"
    );
    let mut m = one("client/a.lua");
    m.total_size += 1;
    assert_eq!(
        validate_manifest(&m, SRV, PROTO, &limits())
            .unwrap_err()
            .code,
        "invalid_manifest"
    );
    // Comprimento: segmento até 100, caminho até 180.
    let long_seg = format!("files/{}.lua", "a".repeat(101));
    let long_path = format!("{}/x.lua", ["abcdefghij"; 17].join("/"));
    for p in [&long_seg, &long_path] {
        assert_eq!(
            validate_manifest(&one(p), SRV, PROTO, &limits())
                .unwrap_err()
                .code,
            "invalid_manifest",
            "{p}"
        );
    }
    validate_manifest(
        &one(&format!("files/{}.lua", "a".repeat(96))),
        SRV,
        PROTO,
        &limits(),
    )
    .unwrap();
    validate_manifest(&one("client/sub-dir/a_b.v2.lua"), SRV, PROTO, &limits()).unwrap();
}

#[test]
fn limites_de_quantidade_e_tamanho() {
    let files: Vec<_> = (0..=MAX_FILES)
        .map(|i| file(&format!("files/{i}.png"), format!("{i}").as_bytes()))
        .collect();
    let err = validate_manifest(&manifest(vec![("r", files)]), SRV, PROTO, &limits()).unwrap_err();
    assert_eq!(err.code, "content_too_large");

    let files: Vec<_> = (0..MAX_FILES)
        .map(|i| file(&format!("files/{i}.png"), format!("{i}").as_bytes()))
        .collect();
    validate_manifest(&manifest(vec![("r", files)]), SRV, PROTO, &limits()).unwrap();

    let mut m = one("files/big.webm");
    m.resources[0].files[0].size = 1024 * 1024 * 1024 + 1;
    m.total_size = m.resources[0].files[0].size;
    let err = validate_manifest(&m, SRV, PROTO, &Limits::new(1024)).unwrap_err();
    assert_eq!(err.code, "content_too_large");
    assert!(err.message.contains("1024"));
    validate_manifest(&m, SRV, PROTO, &Limits::new(2048)).unwrap();

    // Soma que estoura u64 também é recusada.
    let mut m = manifest(vec![("r", vec![file("a.lua", b"1"), file("b.lua", b"2")])]);
    m.resources[0].files[0].size = u64::MAX;
    assert_eq!(
        validate_manifest(&m, SRV, PROTO, &limits())
            .unwrap_err()
            .code,
        "content_too_large"
    );
}

#[test]
fn server_id_ou_protocol_diferente() {
    let m = one("client/a.lua");
    let err = validate_manifest(&m, "srv_outro", PROTO, &limits()).unwrap_err();
    assert_eq!(err.code, "manifest_mismatch");
    let err = validate_manifest(&m, SRV, PROTO + 1, &limits()).unwrap_err();
    assert_eq!(err.code, "manifest_mismatch");
}

#[tokio::test]
async fn manifest_malicioso_recusado_antes_de_qualquer_download() {
    let a = blob(10, 10);
    let mut fake = Fake::default();
    fake.files.insert(sha(&a), (a.clone(), Mode::Ok));
    fake.manifest = Some(
        serde_json::json!({
            "serverId": SRV, "protocol": PROTO, "totalSize": 10,
            "resources": [{ "name": "r", "version": "1", "files": [
                { "path": "../../evil.lua", "sha256": sha(&a), "size": 10 }
            ]}]
        })
        .to_string(),
    );
    let (base, st) = start(fake).await;
    let m = fetch_manifest(&base).await.unwrap();
    assert_eq!(m.resources[0].files[0].path, "../../evil.lua");
    assert_eq!(
        validate_manifest(&m, SRV, PROTO, &limits())
            .unwrap_err()
            .code,
        "invalid_manifest"
    );
    assert_eq!(st.lock().unwrap().manifest_hits, 1);
    assert!(st.lock().unwrap().hits.is_empty());
}

#[tokio::test]
async fn manifest_do_servidor_real_e_lido() {
    // Mesmo formato de `ResourceStore::manifestJson` (server/src/manifest.cpp).
    let mut fake = Fake::default();
    let body = format!(
        r#"{{"protocol":3,"resources":[{{"files":[{{"path":"client/main.lua","sha256":"{}","size":1}}],"name":"hello","version":"1.0.0"}}],"serverId":"srv_teste","totalSize":1}}"#,
        sha(b"x")
    );
    fake.manifest = Some(body);
    let (base, _) = start(fake).await;
    let m = fetch_manifest(&base).await.unwrap();
    validate_manifest(&m, SRV, PROTO, &limits()).unwrap();
    assert_eq!(m.resources[0].name, "hello");

    let fake = Fake {
        manifest: Some("não é json".into()),
        ..Fake::default()
    };
    let (base, _) = start(fake).await;
    assert_eq!(
        fetch_manifest(&base).await.unwrap_err().code,
        "invalid_manifest"
    );
    let (base, _) = start(Fake::default()).await;
    assert_eq!(
        fetch_manifest(&base).await.unwrap_err().code,
        "server_unavailable"
    );
}

#[test]
fn url_do_servidor_com_ipv6_entre_colchetes() {
    assert_eq!(
        server_base_url("::1", 30120).unwrap().as_str(),
        "http://[::1]:30120/"
    );
    assert_eq!(
        server_base_url("[2001:db8::1]", 8080).unwrap().as_str(),
        "http://[2001:db8::1]:8080/"
    );
    assert_eq!(
        server_base_url("203.0.113.5", 8080).unwrap().as_str(),
        "http://203.0.113.5:8080/"
    );
    assert_eq!(
        server_base_url("jogo.exemplo.com", 8080).unwrap().as_str(),
        "http://jogo.exemplo.com:8080/"
    );
    for bad in ["", "a/b", "x@y", "a b", "host:80", "[::1", "a?b", "a#b"] {
        assert_eq!(
            server_base_url(bad, 80).unwrap_err().code,
            "server_unavailable",
            "{bad:?}"
        );
    }
    assert!(server_base_url("127.0.0.1", 0).is_err());
}

// ----- montagem -----

async fn cached(env: &Env, files: &[(&[u8], &str, &str)]) -> Manifest {
    let mut fake = Fake::default();
    let mut by_res: Vec<(&str, Vec<ManifestFile>)> = vec![];
    for (data, res, path) in files {
        fake.files.insert(sha(data), (data.to_vec(), Mode::Ok));
        match by_res.iter_mut().find(|(r, _)| r == res) {
            Some((_, v)) => v.push(file(path, data)),
            None => by_res.push((res, vec![file(path, data)])),
        }
    }
    let (base, _) = start(fake).await;
    let m = manifest(by_res);
    validate_manifest(&m, SRV, PROTO, &limits()).unwrap();
    download(env, &base, &env.store.missing(&m)).await.unwrap();
    m
}

fn tree(dir: &Path) -> Vec<String> {
    fn walk(base: &Path, d: &Path, out: &mut Vec<String>) {
        for e in std::fs::read_dir(d).unwrap() {
            let p = e.unwrap().path();
            let rel = p
                .strip_prefix(base)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/");
            if p.is_dir() {
                out.push(format!("{rel}/"));
                walk(base, &p, out);
            } else {
                out.push(rel);
            }
        }
    }
    let mut out = vec![];
    walk(dir, dir, &mut out);
    out.sort();
    out
}

/// Hard link compartilha os atributos com o arquivo do cache; cópia não.
fn shares_attributes(cache_file: &Path, mounted: &Path) -> bool {
    let mut p = std::fs::metadata(mounted).unwrap().permissions();
    assert!(p.readonly());
    #[allow(clippy::permissions_set_readonly_false)]
    p.set_readonly(false);
    std::fs::set_permissions(mounted, p).unwrap();
    let shared = !std::fs::metadata(cache_file)
        .unwrap()
        .permissions()
        .readonly();
    let mut p = std::fs::metadata(mounted).unwrap().permissions();
    p.set_readonly(true);
    std::fs::set_permissions(mounted, p).unwrap();
    let mut p = std::fs::metadata(cache_file).unwrap().permissions();
    p.set_readonly(true);
    std::fs::set_permissions(cache_file, p).unwrap();
    shared
}

#[tokio::test]
async fn montagem_por_hard_link_e_remocao_de_sobras() {
    let env = env();
    let (a, b, c) = (blob(11, 500), blob(12, 600), blob(13, 700));
    let v1 = cached(
        &env,
        &[
            (&a, "mapa", "client/a.lua"),
            (&b, "mapa", "files/sub/b.png"),
            (&c, "velho", "ui/c.html"),
        ],
    )
    .await;
    let dir = env.store.mount(SRV, &v1).unwrap();
    assert_eq!(dir, env.servers.join(SRV));
    assert_eq!(
        tree(&dir),
        [
            "mapa/",
            "mapa/client/",
            "mapa/client/a.lua",
            "mapa/files/",
            "mapa/files/sub/",
            "mapa/files/sub/b.png",
            "velho/",
            "velho/ui/",
            "velho/ui/c.html"
        ]
    );
    assert_eq!(std::fs::read(dir.join("mapa/files/sub/b.png")).unwrap(), b);
    assert!(shares_attributes(
        &env.cache.join(sha(&a)),
        &dir.join("mapa/client/a.lua")
    ));

    // Nova versão: a muda de conteúdo, b some, recurso "velho" some, d chega; sobra solta também sai.
    std::fs::write(dir.join("mapa/solto.txt"), b"lixo").unwrap();
    let a2 = blob(14, 520);
    let d = blob(15, 10);
    let v2 = cached(
        &env,
        &[(&a2, "mapa", "client/a.lua"), (&d, "mapa", "files/d.png")],
    )
    .await;
    let dir = env.store.mount(SRV, &v2).unwrap();
    assert_eq!(
        tree(&dir),
        [
            "mapa/",
            "mapa/client/",
            "mapa/client/a.lua",
            "mapa/files/",
            "mapa/files/d.png"
        ]
    );
    assert_eq!(std::fs::read(dir.join("mapa/client/a.lua")).unwrap(), a2);
    // O cache continua íntegro e somente leitura depois de remover os links.
    assert_eq!(std::fs::read(env.cache.join(sha(&a))).unwrap(), a);
    assert!(std::fs::metadata(env.cache.join(sha(&a)))
        .unwrap()
        .permissions()
        .readonly());
    assert_eq!(
        env.store.mounted_hashes([SRV]),
        HashSet::from([sha(&a2), sha(&d)])
    );
}

#[tokio::test]
async fn montagem_cai_para_copia() {
    let tmp = tempfile::tempdir().unwrap();
    let base_env = env();
    let a = blob(16, 300);
    let m = cached(&base_env, &[(&a, "r", "client/a.lua")]).await;
    let store = ContentStore::new(base_env.cache.clone(), tmp.path().join("servers"))
        .with_hard_links(false);
    let dir = store.mount(SRV, &m).unwrap();
    let mounted = dir.join("r/client/a.lua");
    assert_eq!(std::fs::read(&mounted).unwrap(), a);
    assert!(!shares_attributes(&base_env.cache.join(sha(&a)), &mounted));
}

#[tokio::test]
async fn montagem_sem_arquivo_no_cache_falha() {
    let env = env();
    let m = manifest(vec![("r", vec![file("client/a.lua", b"nada")])]);
    let err = env.store.mount(SRV, &m).unwrap_err();
    assert_eq!(err.code, "content_missing");
    assert!(env.store.mount("../fora", &m).is_err());
    assert!(env.store.mount(".mounts", &m).is_err());
}

// ----- limite do cache -----

#[tokio::test]
async fn lru_remove_os_menos_usados_e_respeita_keep() {
    let env = env();
    let blobs: Vec<Vec<u8>> = (0..4).map(|i| blob(20 + i, 1000)).collect();
    // Baixados em ordem 0,1,2,3.
    for (i, d) in blobs.iter().enumerate() {
        cached(&env, &[(d, "r", &format!("files/{i}.png"))]).await;
    }
    let h: Vec<String> = blobs.iter().map(|d| sha(d)).collect();
    let m = |i: usize| {
        manifest(vec![(
            "r",
            vec![file(&format!("files/{i}.png"), &blobs[i])],
        )])
    };
    env.store.mount("srv_fav", &m(0)).unwrap();
    // Montar o 1 o torna o mais recente.
    env.store.mount("srv_x", &m(1)).unwrap();
    let keep = HashSet::from(["srv_fav".to_string()]);
    assert_eq!(
        env.store.disk_usage(),
        4000,
        "hard link não ocupa espaço próprio"
    );

    assert_eq!(
        env.store.enforce_limit(4000, &keep).unwrap(),
        0,
        "dentro do limite"
    );
    assert_eq!(env.store.enforce_limit(2500, &keep).unwrap(), 2000);
    assert!(!env.cache.join(&h[2]).exists() && !env.cache.join(&h[3]).exists());
    assert!(env.cache.join(&h[1]).exists(), "usado por último fica");
    assert!(env.servers.join("srv_x").exists());

    // Limite 0: o 1 sai junto com a montagem de srv_x (senão o hard link manteria os dados no disco); o do favorito
    // fica mesmo passando do limite.
    assert_eq!(env.store.enforce_limit(0, &keep).unwrap(), 1000);
    assert!(!env.cache.join(&h[1]).exists());
    assert!(!env.servers.join("srv_x").exists());
    assert!(env.store.mounted_hashes(["srv_x"]).is_empty());
    assert!(env.cache.join(&h[0]).exists());
    assert!(env.servers.join("srv_fav/r/files/0.png").exists());
    assert_eq!(env.store.disk_usage(), 1000);

    // Índice e registros em disco: outra instância, sem keep, apaga o resto.
    let again = ContentStore::new(env.cache.clone(), env.servers.clone());
    assert_eq!(again.enforce_limit(0, &HashSet::new()).unwrap(), 1000);
    assert_eq!(again.disk_usage(), 0);
    assert!(!env.servers.join("srv_fav").exists());
    assert!(env.cache.join("index.json").exists());
}

#[tokio::test]
async fn limite_conta_e_apaga_copias_montadas() {
    let base_env = env();
    let (a, b) = (blob(40, 1000), blob(41, 1000));
    let ma = cached(&base_env, &[(&a, "r", "files/a.png")]).await;
    let mb = cached(&base_env, &[(&b, "r", "files/b.png")]).await;
    let store =
        ContentStore::new(base_env.cache.clone(), base_env.servers.clone()).with_hard_links(false);
    store.mount("srv_fav", &mb).unwrap();
    store.mount("srv_x", &ma).unwrap();
    assert_eq!(store.disk_usage(), 4000, "cópias contam");
    let keep = HashSet::from(["srv_fav".to_string()]);
    assert_eq!(store.enforce_limit(2000, &keep).unwrap(), 2000);
    assert!(!base_env.servers.join("srv_x").exists());
    assert!(!base_env.cache.join(sha(&a)).exists());
    assert!(base_env.cache.join(sha(&b)).exists());
    assert!(base_env.servers.join("srv_fav/r/files/b.png").exists());
    assert_eq!(store.disk_usage(), 2000);
}

/// Sobe um servidor com um único arquivo neste modo e baixa `data` dele.
async fn try_mode(mode: Mode, data: &[u8]) -> (Env, Shared, Result<(), AppError>) {
    let mut fake = Fake::default();
    fake.files.insert(sha(data), (data.to_vec(), mode));
    let (base, st) = start(fake).await;
    let env = env();
    let m = manifest(vec![("r", vec![file("files/a.png", data)])]);
    let r = download(&env, &base, &env.store.missing(&m)).await;
    (env, st, r)
}

fn requests(st: &Shared) -> Vec<Option<String>> {
    st.lock()
        .unwrap()
        .ranges
        .iter()
        .map(|(_, r)| r.clone())
        .collect()
}

#[tokio::test]
async fn retomada_com_respostas_estranhas() {
    let a = blob(42, 200_000);
    let drop_then = |then| Mode::Drop {
        garbage: false,
        then,
    };

    // 200 ao pedido com Range: recomeça do zero na mesma resposta.
    let (env, st, r) = try_mode(drop_then(Then::Full), &a).await;
    r.unwrap();
    assert_eq!(std::fs::read(env.cache.join(sha(&a))).unwrap(), a);
    let req = requests(&st);
    assert_eq!(req.len(), 2);
    assert!(req[1].is_some());

    // 206 começando em outro lugar, ou 416: descarta o `.part` e a 3ª tentativa pede o arquivo inteiro.
    for then in [Then::BadStart, Then::Status416] {
        let (env, st, r) = try_mode(drop_then(then), &a).await;
        r.unwrap_or_else(|e| panic!("{then:?}: {e}"));
        assert_eq!(std::fs::read(env.cache.join(sha(&a))).unwrap(), a);
        let req = requests(&st);
        assert_eq!(req.len(), 3, "{then:?}");
        assert!(req[1].is_some() && req[2].is_none(), "{then:?}");
        assert!(parts(&env.cache).is_empty());
    }

    // Começo retomado com lixo: o hash final falha, mas a culpa pode ser do pedaço antigo → recomeça do zero.
    let (env, st, r) = try_mode(
        Mode::Drop {
            garbage: true,
            then: Then::Range,
        },
        &a,
    )
    .await;
    r.unwrap();
    assert_eq!(std::fs::read(env.cache.join(sha(&a))).unwrap(), a);
    let req = requests(&st);
    assert_eq!(req.len(), 3);
    assert!(req[1].is_some() && req[2].is_none());

    // Total do Content-Range diferente do declarado: aborta.
    let (env, _, r) = try_mode(drop_then(Then::WrongTotal), &a).await;
    assert_eq!(r.unwrap_err().code, "size_mismatch");
    assert!(parts(&env.cache).is_empty());
    assert!(!env.cache.join(sha(&a)).exists());
}

#[tokio::test]
async fn sobras_de_um_servidor_nao_contaminam_outro() {
    let a = blob(43, 100_000);
    // Servidor hostil: manda quase tudo do hash de outro servidor, com lixo, e derruba; sempre.
    let (env, _, r) = try_mode(
        Mode::Drop {
            garbage: true,
            then: Then::Again,
        },
        &a,
    )
    .await;
    assert!(r.is_err());
    assert!(parts(&env.cache).is_empty(), "nada fica em tmp");
    assert_eq!(
        std::fs::read_dir(env.cache.join("tmp")).unwrap().count(),
        0,
        "nem a pasta do download"
    );

    // O servidor honesto com o mesmo hash baixa do zero, sem Range, e dá certo.
    let mut fake = Fake::default();
    fake.files.insert(sha(&a), (a.clone(), Mode::Ok));
    let (good, st) = start(fake).await;
    let m = manifest(vec![("alvo", vec![file("files/a.png", &a)])]);
    download(&env, &good, &env.store.missing(&m)).await.unwrap();
    assert_eq!(requests(&st), vec![None]);
    assert_eq!(std::fs::read(env.cache.join(sha(&a))).unwrap(), a);
}

#[tokio::test]
async fn falha_apaga_os_part_do_download() {
    // Um arquivo esgota as tentativas no meio e outro ainda estava baixando: nada fica em tmp.
    let a = blob(44, 100_000);
    let slow = blob(45, 400_000);
    let mut fake = Fake::default();
    fake.files.insert(
        sha(&a),
        (
            a.clone(),
            Mode::Drop {
                garbage: false,
                then: Then::Again,
            },
        ),
    );
    fake.files.insert(sha(&slow), (slow.clone(), Mode::Slow));
    let (base, _) = start(fake).await;
    let env = env();
    let m = manifest(vec![(
        "r",
        vec![file("files/a.png", &a), file("files/s.webm", &slow)],
    )]);
    assert!(download(&env, &base, &env.store.missing(&m)).await.is_err());
    assert!(parts(&env.cache).is_empty());
    assert!(!env.cache.join(sha(&slow)).exists());
}

#[tokio::test]
async fn startup_apaga_sobras_e_arquivos_fora_do_indice() {
    let env = env();
    let a = blob(46, 100);
    let m = cached(&env, &[(&a, "r", "client/a.lua")]).await;
    // Sobra de um download antigo e arquivo do cache sem entrada no índice (queda antes de indexar).
    let stale = env.cache.join("tmp").join("antigo");
    std::fs::create_dir_all(&stale).unwrap();
    std::fs::write(stale.join("x.part"), b"meio").unwrap();
    let orphan = env.cache.join(sha(b"orfao"));
    std::fs::write(&orphan, b"orfao").unwrap();
    let m2 = manifest(vec![("r", vec![file("client/o.lua", b"orfao")])]);
    assert_eq!(
        env.store.missing(&m2).len(),
        1,
        "fora do índice não é confiável"
    );
    assert_eq!(
        env.store.mount(SRV, &m2).unwrap_err().code,
        "content_missing"
    );

    env.store.startup();
    assert!(!stale.exists());
    assert!(!orphan.exists());
    assert!(env.cache.join(sha(&a)).exists());
    assert!(env.store.missing(&m).is_empty());

    // A varredura também roda antes de cada download.
    std::fs::create_dir_all(&stale).unwrap();
    let (base, _) = start(Fake::default()).await;
    download(&env, &base, &[]).await.unwrap();
    assert!(!stale.exists());
}

#[tokio::test]
async fn montagem_interrompida_nao_deixa_registro_valendo() {
    let env = env();
    let (a, b) = (blob(47, 300), blob(48, 300));
    let ma = cached(&env, &[(&a, "r", "client/x.lua")]).await;
    let mb = cached(&env, &[(&b, "r", "client/x.lua")]).await;
    env.store.mount(SRV, &ma).unwrap();
    // Simula queda no meio de montar `mb`: o arquivo já virou b, mas o registro ficou como antes.
    let target = env.servers.join(SRV).join("r/client/x.lua");
    std::fs::remove_file(&target).unwrap();
    std::fs::copy(env.cache.join(sha(&b)), &target).unwrap();
    let rec = env.servers.join(".mounts").join(format!("{SRV}.json"));
    let body = std::fs::read_to_string(&rec).unwrap();
    assert!(body.contains("\"complete\":true"));
    // O registro é marcado incompleto antes de mexer na pasta: com ele assim, nada é mantido às cegas.
    std::fs::write(
        &rec,
        body.replace("\"complete\":true", "\"complete\":false"),
    )
    .unwrap();
    env.store.mount(SRV, &ma).unwrap();
    assert_eq!(std::fs::read(&target).unwrap(), a);
    let _ = mb;
}

#[tokio::test]
async fn limpar_cache() {
    let env = env();
    let a = blob(30, 100);
    let m = cached(&env, &[(&a, "r", "client/a.lua")]).await;
    env.store.mount(SRV, &m).unwrap();
    env.store.clear().unwrap();
    assert!(!env.cache.join(sha(&a)).exists());
    assert!(!env.servers.join(SRV).exists());
    assert_eq!(env.store.missing(&m).len(), 1);
}
