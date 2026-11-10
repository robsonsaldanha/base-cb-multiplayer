//! Mod do cliente: assinatura minisign (par gerado aqui), zip seguro, hashes, reinstalação e nunca downgrade, contra
//! um Hub falso (axum em 127.0.0.1).

use axum::extract::{Path as AxPath, State};
use axum::http::{StatusCode, Uri};
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::{Json, Router};
use base64::engine::general_purpose::STANDARD;
use base64::Engine as _;
use cbmp_launcher_lib::game::{GameInstall, Store};
use cbmp_launcher_lib::hub::HubClient;
use cbmp_launcher_lib::modpkg::{
    extract_zip, mod_dir, verify_minisign, ModInstaller, MAX_ENTRY_BYTES,
};
use sha2::{Digest, Sha256};
use std::io::{Cursor, Write};
use std::path::Path;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio_util::sync::CancellationToken;

// ----- chaves e assinatura (par só deste teste) -----

struct Keys {
    pk: minisign::PublicKey,
    sk: minisign::SecretKey,
}

fn keys() -> Keys {
    let kp = minisign::KeyPair::generate_unencrypted_keypair().unwrap();
    Keys {
        pk: kp.pk,
        sk: kp.sk,
    }
}

/// Chave pública no formato do Tauri (`keys/update.pub`): base64 do arquivo `.pub` do minisign.
fn tauri_pubkey(k: &Keys) -> String {
    STANDARD.encode(k.pk.to_box().unwrap().to_string())
}

/// Assinatura no formato do `tauri signer sign` (base64 do arquivo `.sig`).
fn tauri_sig(k: &Keys, data: &[u8]) -> String {
    STANDARD.encode(raw_sig(k, data))
}

/// Texto do `.sig` como o minisign grava (comentário confiável padrão: `timestamp:...`).
fn raw_sig(k: &Keys, data: &[u8]) -> String {
    raw_sig_with(k, data, None)
}

fn raw_sig_with(k: &Keys, data: &[u8], trusted: Option<&str>) -> String {
    minisign::sign(Some(&k.pk), &k.sk, Cursor::new(data), trusted, None)
        .unwrap()
        .to_string()
}

/// Comentário confiável que o workflow de release grava na assinatura do mod.
fn mod_comment(protocol: u32, version: &str, sha256: &str) -> String {
    format!("cbmp-mod protocol={protocol} version={version} sha256={sha256}")
}

/// Assinatura de mod como a do release (base64 do `.sig`, comentário `cbmp-mod ...`).
fn mod_sig(k: &Keys, data: &[u8], protocol: u32, version: &str) -> String {
    STANDARD.encode(raw_sig_with(
        k,
        data,
        Some(&mod_comment(protocol, version, &sha_hex(data))),
    ))
}

fn sha_hex(data: &[u8]) -> String {
    Sha256::digest(data)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

#[test]
fn minisign_valido_alterado_chave_errada_e_sem_assinatura() {
    let k = keys();
    let data = b"conteudo do mod".to_vec();
    let pk = tauri_pubkey(&k);
    let comment = verify_minisign(&data, &tauri_sig(&k, &data), &pk).unwrap();
    assert!(comment.starts_with("timestamp:"), "{comment}");
    // `.sig` e `.pub` em texto puro (como o minisign grava) também valem.
    verify_minisign(
        &data,
        &raw_sig(&k, &data),
        &k.pk.to_box().unwrap().to_string(),
    )
    .unwrap();

    let mut altered = data.clone();
    altered[0] ^= 1;
    let e = verify_minisign(&altered, &tauri_sig(&k, &data), &pk).unwrap_err();
    assert_eq!(e.code, "mod_signature_invalid");

    let other = keys();
    let e = verify_minisign(&data, &tauri_sig(&other, &data), &pk).unwrap_err();
    assert_eq!(e.code, "mod_signature_invalid");

    for sig in ["", "   ", "nao-e-base64!", &STANDARD.encode("lixo")] {
        let e = verify_minisign(&data, sig, &pk).unwrap_err();
        assert_eq!(e.code, "mod_signature_invalid", "{sig:?}");
    }
    // Chave pública inválida também recusa (nunca "passa" sem verificar).
    let e = verify_minisign(&data, &tauri_sig(&k, &data), "").unwrap_err();
    assert_eq!(e.code, "mod_signature_invalid");
}

// ----- zip -----

enum Entry<'a> {
    File(&'a str, &'a [u8]),
    Dir(&'a str),
    Symlink(&'a str, &'a str),
}

fn zip_bytes(entries: &[Entry]) -> Vec<u8> {
    let mut w = zip::ZipWriter::new(Cursor::new(Vec::new()));
    let opts = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    for e in entries {
        match e {
            Entry::File(name, data) => {
                w.start_file(*name, opts).unwrap();
                w.write_all(data).unwrap();
            }
            Entry::Dir(name) => w.add_directory(*name, opts).unwrap(),
            Entry::Symlink(name, target) => w.add_symlink(*name, *target, opts).unwrap(),
        }
    }
    w.finish().unwrap().into_inner()
}

/// Troca o tamanho descompactado declarado no diretório central da entrada `name` (zip "mentiroso").
fn patch_declared_size(zip: &mut [u8], name: &str, size: u32) {
    let sig = [0x50, 0x4b, 0x01, 0x02];
    let mut i = 0;
    while i + 46 <= zip.len() {
        if zip[i..i + 4] == sig {
            let n = u16::from_le_bytes([zip[i + 28], zip[i + 29]]) as usize;
            if &zip[i + 46..i + 46 + n] == name.as_bytes() {
                zip[i + 24..i + 28].copy_from_slice(&size.to_le_bytes());
                return;
            }
        }
        i += 1;
    }
    panic!("entrada {name} não encontrada");
}

fn write_zip(dir: &Path, bytes: &[u8]) -> std::path::PathBuf {
    let p = dir.join("mod.zip");
    std::fs::write(&p, bytes).unwrap();
    p
}

fn files_under(dir: &Path) -> Vec<String> {
    let mut out = Vec::new();
    fn walk(base: &Path, d: &Path, out: &mut Vec<String>) {
        let Ok(rd) = std::fs::read_dir(d) else { return };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                walk(base, &p, out);
            } else {
                out.push(
                    p.strip_prefix(base)
                        .unwrap()
                        .to_string_lossy()
                        .replace('\\', "/"),
                );
            }
        }
    }
    walk(dir, dir, &mut out);
    out.sort();
    out
}

#[test]
fn zip_valido_extrai_com_hash_de_cada_arquivo() {
    let tmp = tempfile::tempdir().unwrap();
    let z = write_zip(
        tmp.path(),
        &zip_bytes(&[
            Entry::Dir("bin/"),
            Entry::File("bin/cbmp.dll", b"dll"),
            Entry::File("cbmp.ini", b"[cbmp]"),
        ]),
    );
    let dest = tmp.path().join("cbmp");
    let files = extract_zip(&z, &dest).unwrap();
    assert_eq!(files_under(&dest), vec!["bin/cbmp.dll", "cbmp.ini"]);
    let dll = files.iter().find(|f| f.path == "bin/cbmp.dll").unwrap();
    assert_eq!(dll.sha256, sha_hex(b"dll"));
    assert_eq!(dll.size, 3);
    assert_eq!(files.len(), 2);
}

#[test]
fn zip_malicioso_e_recusado_sem_gravar_nada() {
    let tmp = tempfile::tempdir().unwrap();
    let ok: &[u8] = b"ok";
    let cases: Vec<(&str, Vec<u8>)> = vec![
        (
            "..",
            zip_bytes(&[Entry::File("a.dll", ok), Entry::File("../fora.dll", ok)]),
        ),
        (
            "../ no meio",
            zip_bytes(&[Entry::File("a/../../fora.dll", ok)]),
        ),
        ("absoluto", zip_bytes(&[Entry::File("/abs.dll", ok)])),
        ("unidade", zip_bytes(&[Entry::File("C:/abs.dll", ok)])),
        (
            "barra invertida",
            zip_bytes(&[Entry::File("a\\..\\..\\fora.dll", ok)]),
        ),
        (
            "symlink",
            zip_bytes(&[Entry::File("a.dll", ok), Entry::Symlink("link", "../../..")]),
        ),
        ("vazio", zip_bytes(&[Entry::File("a//b.dll", ok)])),
        (
            "duplicado",
            zip_bytes(&[Entry::File("a.dll", ok), Entry::File("A.DLL", ok)]),
        ),
    ];
    for (what, bytes) in cases {
        let dir = tmp.path().join(what);
        std::fs::create_dir_all(&dir).unwrap();
        let z = write_zip(&dir, &bytes);
        let dest = dir.join("cbmp");
        let e = extract_zip(&z, &dest).unwrap_err();
        assert_eq!(e.code, "mod_invalid", "{what}: {e}");
        // Recusado pela regra certa, não por o zip estar ilegível.
        assert!(!e.message.contains("corrompido"), "{what}: {e}");
        if what == "symlink" {
            assert!(e.message.contains("link"), "{e}");
        }
        assert!(files_under(&dest).is_empty(), "{what}: gravou algo");
        assert!(!dir.join("fora.dll").exists() && !tmp.path().join("fora.dll").exists());
    }
}

#[test]
fn zip_com_arquivo_declarado_acima_de_512_mib_e_recusado() {
    let tmp = tempfile::tempdir().unwrap();
    let mut bytes = zip_bytes(&[Entry::File("a.dll", b"ok"), Entry::File("grande.pak", b"x")]);
    let declared = u32::try_from(MAX_ENTRY_BYTES + 1).unwrap();
    patch_declared_size(&mut bytes, "grande.pak", declared);
    let z = write_zip(tmp.path(), &bytes);
    let dest = tmp.path().join("cbmp");
    let e = extract_zip(&z, &dest).unwrap_err();
    assert_eq!(e.code, "mod_invalid");
    assert!(e.message.contains("grande demais"), "{e}");
    assert!(files_under(&dest).is_empty());
}

#[test]
fn zip_que_mente_o_tamanho_para_baixo_e_recusado() {
    let tmp = tempfile::tempdir().unwrap();
    let mut bytes = zip_bytes(&[Entry::File("a.dll", &[7u8; 4096])]);
    patch_declared_size(&mut bytes, "a.dll", 10);
    let z = write_zip(tmp.path(), &bytes);
    let e = extract_zip(&z, &tmp.path().join("cbmp")).unwrap_err();
    assert_eq!(e.code, "mod_invalid");
}

// ----- Hub falso -----

#[derive(Clone)]
struct Published {
    version: String,
    zip: Vec<u8>,
    /// Assinatura anunciada (pode não bater, nos testes de recusa).
    signature: String,
    /// sha256 anunciado.
    sha256: String,
}

#[derive(Default)]
struct Fake {
    published: Option<Published>,
    /// Bytes do zip servidos em vez do publicado (download adulterado no caminho).
    serve_instead: Option<Vec<u8>>,
    /// Próximos N downloads são cortados no meio (como o Hub que derruba cliente lento).
    truncate_next: u32,
    downloads: u32,
    latest_calls: u32,
    protocols: Vec<String>,
    /// `http://127.0.0.1:porta` do próprio Hub falso (a `url` do mod aponta para ele, como o `publicUrl` do Hub).
    base: String,
}

type Shared = Arc<Mutex<Fake>>;

async fn latest(State(s): State<Shared>, uri: Uri) -> Response {
    let mut f = s.lock().unwrap();
    f.latest_calls += 1;
    let q = uri.query().unwrap_or("").to_string();
    f.protocols.push(q.clone());
    let protocol: u32 = q
        .strip_prefix("protocol=")
        .and_then(|p| p.parse().ok())
        .unwrap_or(999);
    let Some(p) = f.published.clone() else {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({ "error": "no_mod", "message": "x" })),
        )
            .into_response();
    };
    let name = format!("cbmp-mod-p{protocol}-v{}.zip", p.version);
    Json(serde_json::json!({
        "version": p.version,
        "protocol": protocol,
        "url": format!("{}/v1/dl/mod-p{protocol}-v{}/{name}", f.base, p.version),
        "sha256": p.sha256,
        "size": p.zip.len(),
        "signature": p.signature,
    }))
    .into_response()
}

async fn dl(State(s): State<Shared>, AxPath((_tag, _asset)): AxPath<(String, String)>) -> Response {
    let mut f = s.lock().unwrap();
    f.downloads += 1;
    let Some(p) = f.published.clone() else {
        return StatusCode::NOT_FOUND.into_response();
    };
    let body = f.serve_instead.clone().unwrap_or(p.zip);
    if f.truncate_next > 0 {
        f.truncate_next -= 1;
        // Content-Length completo, corpo pela metade: o cliente vê a conexão cair.
        let half = body[..body.len() / 2].to_vec();
        let stream = futures_util::stream::iter(vec![
            Ok::<_, std::io::Error>(half),
            Err(std::io::Error::other("cortado")),
        ]);
        return Response::builder()
            .header("content-length", body.len().to_string())
            .body(axum::body::Body::from_stream(stream))
            .unwrap();
    }
    body.into_response()
}

struct Env {
    _tmp: tempfile::TempDir,
    fake: Shared,
    hub: HubClient,
    installer: ModInstaller,
    game: GameInstall,
    keys: Keys,
    mods: std::path::PathBuf,
}

async fn env() -> Env {
    let tmp = tempfile::tempdir().unwrap();
    let fake: Shared = Arc::new(Mutex::new(Fake::default()));
    let app = Router::new()
        .route("/v1/mod/latest", get(latest))
        .route("/v1/dl/{tag}/{asset}", get(dl))
        .with_state(fake.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let base = format!("http://{addr}");
    fake.lock().unwrap().base = base.clone();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    let game_dir = tmp.path().join("Hogwarts Legacy");
    std::fs::create_dir_all(game_dir.join("Phoenix").join("Binaries").join("Win64")).unwrap();
    std::fs::write(game_dir.join("HogwartsLegacy.exe"), b"MZ").unwrap();
    let game = GameInstall {
        exe: game_dir.join("HogwartsLegacy.exe"),
        dir: game_dir,
        store: Store::Manual,
    };
    let k = keys();
    let mods = tmp.path().join("mods");
    let installer = ModInstaller::new(mods.clone(), tauri_pubkey(&k))
        .with_retry_delay(Duration::from_millis(5));
    Env {
        hub: HubClient::new(&base).unwrap(),
        _tmp: tmp,
        fake,
        installer,
        game,
        keys: k,
        mods,
    }
}

impl Env {
    fn publish(&self, version: &str, entries: &[Entry]) -> Vec<u8> {
        self.publish_p(3, version, entries)
    }

    fn publish_p(&self, protocol: u32, version: &str, entries: &[Entry]) -> Vec<u8> {
        self.publish_zip(
            zip_bytes(entries),
            mod_sig_for(&self.keys, protocol, version),
        )
        .1
    }

    /// Publica `zip` com a assinatura que `sign` produz para ele; devolve (versão anunciada, zip).
    fn publish_zip(&self, zip: Vec<u8>, sign: Signer) -> (String, Vec<u8>) {
        let (version, sign) = sign;
        let p = Published {
            version: version.clone(),
            signature: sign(&zip),
            sha256: sha_hex(&zip),
            zip: zip.clone(),
        };
        self.fake.lock().unwrap().published = Some(p);
        (version, zip)
    }

    fn package(&self) -> std::path::PathBuf {
        self.mods.join("package")
    }

    fn downloads(&self) -> u32 {
        self.fake.lock().unwrap().downloads
    }

    async fn ensure(
        &self,
        protocol: u32,
    ) -> Result<cbmp_launcher_lib::modpkg::ModState, cbmp_launcher_lib::error::AppError> {
        self.installer
            .ensure(&self.hub, protocol, &self.game, CancellationToken::new())
            .await
    }
}

/// Versão anunciada + assinador "do release" (protocolo/versão/sha256 do próprio zip).
fn mod_sig_for(k: &Keys, protocol: u32, version: &str) -> Signer {
    let (pk, sk) = (k.pk.clone(), k.sk.clone());
    let v = version.to_string();
    let keys = Keys { pk, sk };
    (
        version.to_string(),
        Box::new(move |zip: &[u8]| mod_sig(&keys, zip, protocol, &v)),
    )
}

/// Versão anunciada + função que assina um zip.
type Signer = (String, Box<dyn Fn(&[u8]) -> String>);

fn record(mods: &Path) -> serde_json::Value {
    serde_json::from_slice(&std::fs::read(mods.join("installed.json")).unwrap()).unwrap()
}

#[tokio::test]
async fn instala_registra_e_nao_baixa_de_novo() {
    let e = env().await;
    e.publish(
        "1.0.0",
        &[
            Entry::File("cbmp.dll", b"v1"),
            Entry::File("data/x.bin", b"xx"),
        ],
    );
    let st = e.ensure(3).await.unwrap();
    assert!(st.changed);
    assert_eq!(st.version, "1.0.0");
    assert_eq!(st.protocol, 3);
    let dir = mod_dir(&e.game);
    assert_eq!(
        dir,
        e.game
            .dir
            .join("Phoenix")
            .join("Binaries")
            .join("Win64")
            .join("cbmp")
    );
    assert_eq!(files_under(&dir), vec!["cbmp.dll", "data/x.bin"]);
    // Nada fora de `cbmp\` dentro do jogo.
    assert_eq!(
        files_under(&e.game.dir),
        vec![
            "HogwartsLegacy.exe",
            "Phoenix/Binaries/Win64/cbmp/cbmp.dll",
            "Phoenix/Binaries/Win64/cbmp/data/x.bin"
        ]
    );
    let r = record(&e.mods);
    assert_eq!(r["version"], "1.0.0");
    assert_eq!(r["protocol"], 3);
    assert_eq!(r["files"].as_array().unwrap().len(), 2);
    assert_eq!(r["files"][0]["path"], "cbmp.dll");
    assert_eq!(r["files"][0]["sha256"], sha_hex(b"v1"));
    assert!(e.installer.check_installed(&e.game).unwrap());
    assert_eq!(e.downloads(), 1);
    assert!(e
        .fake
        .lock()
        .unwrap()
        .protocols
        .iter()
        .all(|q| q == "protocol=3"));

    let st = e.ensure(3).await.unwrap();
    assert!(!st.changed);
    assert_eq!(e.downloads(), 1, "instalado e íntegro: não baixa");
    // A pasta de download temporária não fica para trás.
    assert!(!e.mods.join("download").exists() || files_under(&e.mods.join("download")).is_empty());
}

#[tokio::test]
async fn reinstala_quando_um_arquivo_instalado_muda_ou_some() {
    let e = env().await;
    e.publish(
        "1.0.0",
        &[Entry::File("cbmp.dll", b"v1"), Entry::File("b.bin", b"b")],
    );
    e.ensure(3).await.unwrap();
    let dll = mod_dir(&e.game).join("cbmp.dll");
    std::fs::write(&dll, b"v1-adulterado").unwrap();
    assert!(!e.installer.check_installed(&e.game).unwrap());
    // Reparo pelo pacote assinado guardado em `mods\package`: sem novo download.
    let st = e.ensure(3).await.unwrap();
    assert!(st.changed);
    assert_eq!(std::fs::read(&dll).unwrap(), b"v1");
    assert_eq!(e.downloads(), 1);
    assert!(e.package().join("cbmp-mod.zip").is_file());

    // Sem pacote local (ou pacote adulterado): baixa de novo do Hub.
    std::fs::remove_file(mod_dir(&e.game).join("b.bin")).unwrap();
    std::fs::write(e.package().join("cbmp-mod.zip"), b"lixo").unwrap();
    assert!(!e.installer.check_installed(&e.game).unwrap());
    e.ensure(3).await.unwrap();
    assert_eq!(e.downloads(), 2);
    assert!(e.installer.check_installed(&e.game).unwrap());
    std::fs::remove_dir_all(e.package()).unwrap();
    std::fs::remove_file(&dll).unwrap();
    e.ensure(3).await.unwrap();
    assert_eq!(e.downloads(), 3);
    assert!(e.installer.check_installed(&e.game).unwrap());
}

#[tokio::test]
async fn versao_nova_substitui_e_remove_arquivos_antigos() {
    let e = env().await;
    e.publish(
        "1.0.0",
        &[
            Entry::File("cbmp.dll", b"v1"),
            Entry::File("velho.bin", b"o"),
        ],
    );
    e.ensure(3).await.unwrap();
    e.publish("1.1.0", &[Entry::File("cbmp.dll", b"v2")]);
    let st = e.ensure(3).await.unwrap();
    assert!(st.changed);
    assert_eq!(st.version, "1.1.0");
    assert_eq!(files_under(&mod_dir(&e.game)), vec!["cbmp.dll"]);
    assert_eq!(record(&e.mods)["version"], "1.1.0");
}

#[tokio::test]
async fn hub_com_versao_menor_nunca_faz_downgrade() {
    let e = env().await;
    e.publish("2.0.0", &[Entry::File("cbmp.dll", b"v2")]);
    e.ensure(3).await.unwrap();
    assert_eq!(e.downloads(), 1);

    e.publish("1.9.9", &[Entry::File("cbmp.dll", b"v1")]);
    let st = e.ensure(3).await.unwrap();
    assert!(!st.changed);
    assert_eq!(st.version, "2.0.0");
    assert_eq!(e.downloads(), 1, "versão menor: nada é baixado");
    assert_eq!(
        std::fs::read(mod_dir(&e.game).join("cbmp.dll")).unwrap(),
        b"v2"
    );

    // Instalada alterada e o Hub só tem versão menor: repara pelo pacote local (mesma versão, não é downgrade).
    std::fs::write(mod_dir(&e.game).join("cbmp.dll"), b"x").unwrap();
    let st = e.ensure(3).await.unwrap();
    assert_eq!((st.version.as_str(), st.changed), ("2.0.0", true));
    assert_eq!(
        std::fs::read(mod_dir(&e.game).join("cbmp.dll")).unwrap(),
        b"v2"
    );
    assert_eq!(e.downloads(), 1);

    // Sem pacote local: recusa (não baixa a menor).
    std::fs::remove_dir_all(e.package()).unwrap();
    std::fs::write(mod_dir(&e.game).join("cbmp.dll"), b"x").unwrap();
    let err = e.ensure(3).await.unwrap_err();
    assert_eq!(err.code, "mod_altered");
    assert_eq!(e.downloads(), 1);
    // `2.0.0-beta` < `2.0.0` também é menor.
    e.publish("2.0.0-beta.1", &[Entry::File("cbmp.dll", b"b")]);
    assert_eq!(e.ensure(3).await.unwrap_err().code, "mod_altered");
    assert_eq!(e.downloads(), 1);
}

#[tokio::test]
async fn outro_protocolo_instala_a_versao_oferecida() {
    let e = env().await;
    e.publish("5.0.0", &[Entry::File("cbmp.dll", b"p3")]);
    e.ensure(3).await.unwrap();
    e.publish_p(4, "1.0.0", &[Entry::File("cbmp.dll", b"p4")]);
    let st = e.ensure(4).await.unwrap();
    assert!(st.changed);
    assert_eq!((st.version.as_str(), st.protocol), ("1.0.0", 4));
    assert_eq!(
        std::fs::read(mod_dir(&e.game).join("cbmp.dll")).unwrap(),
        b"p4"
    );
}

#[tokio::test]
async fn hash_ou_assinatura_errados_nao_instalam_nada() {
    let e = env().await;
    // Bytes trocados no caminho: sha256 anunciado não bate.
    e.publish("1.0.0", &[Entry::File("cbmp.dll", b"v1")]);
    e.fake.lock().unwrap().serve_instead = Some(zip_bytes(&[Entry::File("cbmp.dll", b"mal")]));
    let err = e.ensure(3).await.unwrap_err();
    assert_eq!(err.code, "mod_invalid");
    assert!(!mod_dir(&e.game).exists());
    assert!(!e.mods.join("installed.json").exists());

    // sha256 confere, assinatura de outra chave.
    e.fake.lock().unwrap().serve_instead = None;
    let other = keys();
    {
        let mut f = e.fake.lock().unwrap();
        let p = f.published.as_mut().unwrap();
        p.signature = mod_sig(&other, &p.zip, 3, "1.0.0");
    }
    let err = e.ensure(3).await.unwrap_err();
    assert_eq!(err.code, "mod_signature_invalid");
    assert!(!mod_dir(&e.game).exists());

    // Assinatura vazia.
    e.fake.lock().unwrap().published.as_mut().unwrap().signature = " ".into();
    assert_eq!(e.ensure(3).await.unwrap_err().code, "mod_signature_invalid");
    assert!(!mod_dir(&e.game).exists());
    assert!(
        files_under(&e.mods).is_empty(),
        "nenhuma sobra: {:?}",
        files_under(&e.mods)
    );
}

#[tokio::test]
async fn zip_assinado_mas_malicioso_nao_grava_nada() {
    let e = env().await;
    e.publish(
        "1.0.0",
        &[
            Entry::File("ok.dll", b"1"),
            Entry::File("../../../fora.dll", b"x"),
        ],
    );
    assert_eq!(e.ensure(3).await.unwrap_err().code, "mod_invalid");
    assert!(files_under(&e.game.dir) == vec!["HogwartsLegacy.exe"]);

    // Com uma versão boa instalada, uma nova maliciosa não mexe nela nem no registro.
    e.publish("1.0.1", &[Entry::File("cbmp.dll", b"bom")]);
    e.ensure(3).await.unwrap();
    e.publish(
        "1.0.2",
        &[
            Entry::File("cbmp.dll", b"x"),
            Entry::Symlink("l", "C:/Windows"),
        ],
    );
    assert_eq!(e.ensure(3).await.unwrap_err().code, "mod_invalid");
    assert!(e.installer.check_installed(&e.game).unwrap());
    assert_eq!(record(&e.mods)["version"], "1.0.1");
    assert_eq!(
        std::fs::read(mod_dir(&e.game).join("cbmp.dll")).unwrap(),
        b"bom"
    );
}

#[tokio::test]
async fn conexao_cortada_tenta_de_novo_e_avisa_lentidao() {
    let e = env().await;
    e.publish("1.0.0", &[Entry::File("cbmp.dll", &[9u8; 64 * 1024])]);
    e.fake.lock().unwrap().truncate_next = 2;
    e.ensure(3).await.unwrap();
    assert_eq!(e.downloads(), 3, "3 tentativas");

    std::fs::remove_dir_all(mod_dir(&e.game)).unwrap();
    std::fs::remove_dir_all(e.package()).unwrap();
    e.fake.lock().unwrap().truncate_next = 3;
    let err = e.ensure(3).await.unwrap_err();
    assert_eq!(err.code, "download_slow");
    assert!(err.message.contains("lenta"), "{}", err.message);
    assert_eq!(e.downloads(), 6);
}

#[tokio::test]
async fn sem_mod_publicado() {
    let e = env().await;
    let err = e.ensure(7).await.unwrap_err();
    assert_eq!(err.code, "mod_unavailable");
    assert_eq!(e.downloads(), 0);
}

#[tokio::test]
async fn cancelado_nao_instala() {
    let e = env().await;
    e.publish("1.0.0", &[Entry::File("cbmp.dll", b"v1")]);
    let c = CancellationToken::new();
    c.cancel();
    let err = e.installer.ensure(&e.hub, 3, &e.game, c).await.unwrap_err();
    assert_eq!(err.code, "cancelled");
    assert!(!mod_dir(&e.game).exists());
}

// ----- o que está assinado manda (I-1) -----

#[tokio::test]
async fn versao_ou_protocolo_anunciados_diferentes_do_assinado_sao_recusados() {
    let e = env().await;
    let zip = zip_bytes(&[Entry::File("cbmp.dll", b"v1")]);
    let k = Keys {
        pk: e.keys.pk.clone(),
        sk: e.keys.sk.clone(),
    };
    let cases: Vec<(&str, String)> = vec![
        // Zip assinado como 1.0.0 anunciado como 9.9.9 (rollback disfarçado de upgrade).
        ("9.9.9", mod_sig(&k, &zip, 3, "1.0.0")),
        // Assinado para o protocolo 2, servido como 3.
        ("1.0.0", mod_sig(&k, &zip, 2, "1.0.0")),
        // Sem o comentário `cbmp-mod` (assinatura comum do minisign/Tauri).
        ("1.0.0", tauri_sig(&k, &zip)),
        // sha256 assinado de outro zip.
        (
            "1.0.0",
            STANDARD.encode(raw_sig_with(
                &k,
                &zip,
                Some(&mod_comment(3, "1.0.0", &sha_hex(b"outro"))),
            )),
        ),
        // Campo a mais / formato diferente.
        (
            "1.0.0",
            STANDARD.encode(raw_sig_with(
                &k,
                &zip,
                Some(&format!(
                    "{} extra=1",
                    mod_comment(3, "1.0.0", &sha_hex(&zip))
                )),
            )),
        ),
    ];
    for (announced, sig) in cases {
        e.fake.lock().unwrap().published = Some(Published {
            version: announced.into(),
            zip: zip.clone(),
            signature: sig,
            sha256: sha_hex(&zip),
        });
        let err = e.ensure(3).await.unwrap_err();
        assert_eq!(err.code, "mod_signature_invalid", "{announced}");
        assert!(!mod_dir(&e.game).exists());
        assert!(!e.mods.join("installed.json").exists());
    }
}

#[tokio::test]
async fn versao_registrada_e_a_assinada() {
    let e = env().await;
    e.publish("1.2.0", &[Entry::File("cbmp.dll", b"v12")]);
    e.ensure(3).await.unwrap();
    assert_eq!(record(&e.mods)["version"], "1.2.0");
    // Um Hub comprometido anuncia um zip velho (assinado 1.0.0) como 99.0.0: recusado, 1.2.0 continua.
    let old = zip_bytes(&[Entry::File("cbmp.dll", b"v10")]);
    let k = Keys {
        pk: e.keys.pk.clone(),
        sk: e.keys.sk.clone(),
    };
    e.fake.lock().unwrap().published = Some(Published {
        version: "99.0.0".into(),
        signature: mod_sig(&k, &old, 3, "1.0.0"),
        sha256: sha_hex(&old),
        zip: old,
    });
    assert_eq!(e.ensure(3).await.unwrap_err().code, "mod_signature_invalid");
    assert_eq!(record(&e.mods)["version"], "1.2.0");
    assert_eq!(
        std::fs::read(mod_dir(&e.game).join("cbmp.dll")).unwrap(),
        b"v12"
    );
    // E a versão nova de verdade continua sendo aceita.
    e.publish("1.3.0", &[Entry::File("cbmp.dll", b"v13")]);
    assert_eq!(e.ensure(3).await.unwrap().version, "1.3.0");
}

// ----- instalação atômica (I-2) -----

/// Zip "stored" com o byte `marker` do conteúdo de `name` trocado: o CRC só falha na extração.
fn crc_broken_zip(entries: &[(&str, &[u8])], marker: &[u8]) -> Vec<u8> {
    let mut w = zip::ZipWriter::new(Cursor::new(Vec::new()));
    let opts =
        zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored);
    for (name, data) in entries {
        w.start_file(*name, opts).unwrap();
        w.write_all(data).unwrap();
    }
    let mut z = w.finish().unwrap().into_inner();
    let at = z
        .windows(marker.len())
        .position(|win| win == marker)
        .expect("marcador");
    z[at] ^= 0xFF;
    z
}

#[tokio::test]
async fn zip_que_falha_no_meio_da_extracao_preserva_o_mod_instalado() {
    let e = env().await;
    e.publish(
        "1.0.1",
        &[
            Entry::File("a.dll", b"bom-a"),
            Entry::File("b.dll", b"bom-b"),
        ],
    );
    e.ensure(3).await.unwrap();
    // 1.0.2 assinado (por um build comprometido, por exemplo) com o CRC do 2º arquivo quebrado.
    let bad = crc_broken_zip(
        &[("a.dll", b"novo-a"), ("b.dll", b"MARCADOR-CRC-XYZ")],
        b"MARCADOR-CRC-XYZ",
    );
    e.publish_zip(bad, mod_sig_for(&e.keys, 3, "1.0.2"));
    let err = e.ensure(3).await.unwrap_err();
    assert_eq!(err.code, "mod_invalid", "{err}");
    assert!(e.installer.check_installed(&e.game).unwrap());
    assert_eq!(record(&e.mods)["version"], "1.0.1");
    assert_eq!(
        std::fs::read(mod_dir(&e.game).join("a.dll")).unwrap(),
        b"bom-a"
    );
    let win64 = mod_dir(&e.game).parent().unwrap().to_path_buf();
    assert!(!win64.join("cbmp.staging").exists() && !win64.join("cbmp.old").exists());
}

#[test]
fn tamanho_mentiroso_nao_mexe_na_pasta_existente() {
    let tmp = tempfile::tempdir().unwrap();
    let dest = tmp.path().join("cbmp");
    std::fs::create_dir_all(&dest).unwrap();
    std::fs::write(dest.join("atual.dll"), b"funciona").unwrap();
    let mut bytes = zip_bytes(&[
        Entry::File("a.dll", b"1"),
        Entry::File("b.dll", &[7u8; 4096]),
    ]);
    patch_declared_size(&mut bytes, "b.dll", 10);
    let z = write_zip(tmp.path(), &bytes);
    assert_eq!(extract_zip(&z, &dest).unwrap_err().code, "mod_invalid");
    assert_eq!(files_under(&dest), vec!["atual.dll"]);
    assert!(!tmp.path().join("cbmp.staging").exists());
    // Zip bom troca a pasta inteira.
    let good = write_zip(tmp.path(), &zip_bytes(&[Entry::File("novo.dll", b"n")]));
    extract_zip(&good, &dest).unwrap();
    assert_eq!(files_under(&dest), vec!["novo.dll"]);
    assert!(!tmp.path().join("cbmp.old").exists());
}

#[test]
fn sobras_de_troca_interrompida_sao_resolvidas() {
    use cbmp_launcher_lib::modpkg::recover_mod_dir;
    let tmp = tempfile::tempdir().unwrap();
    let dest = tmp.path().join("cbmp");
    // Queda entre `cbmp` → `cbmp.old` e `cbmp.staging` → `cbmp`: a versão anterior volta.
    std::fs::create_dir_all(tmp.path().join("cbmp.old")).unwrap();
    std::fs::write(tmp.path().join("cbmp.old").join("v1.dll"), b"1").unwrap();
    std::fs::create_dir_all(tmp.path().join("cbmp.staging")).unwrap();
    std::fs::write(tmp.path().join("cbmp.staging").join("meio.dll"), b"?").unwrap();
    recover_mod_dir(&dest).unwrap();
    assert_eq!(files_under(&dest), vec!["v1.dll"]);
    assert!(!tmp.path().join("cbmp.old").exists() && !tmp.path().join("cbmp.staging").exists());
    // Queda depois da troca: `cbmp` novo existe e `cbmp.old` sobrou → `.old` sai.
    std::fs::create_dir_all(tmp.path().join("cbmp.old")).unwrap();
    recover_mod_dir(&dest).unwrap();
    assert_eq!(files_under(&dest), vec!["v1.dll"]);
    assert!(!tmp.path().join("cbmp.old").exists());
}
