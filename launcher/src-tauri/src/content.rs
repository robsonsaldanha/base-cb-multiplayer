//! Conteúdo do servidor: manifest → validação → diff com o cache → download paralelo verificado → montagem.
//!
//! - Cache global endereçado por conteúdo: `cache\<sha256>` (somente leitura depois de verificado). Cada download
//!   usa a sua pasta `cache\tmp\<job>\` (`<sha256>.part`, hash calculado durante a escrita, `fsync` e `rename` no
//!   fim), apagada ao terminar de qualquer jeito: dados parciais de um servidor nunca são retomados para outro.
//! - `cache\index.json` (gravado de forma atômica) lista só arquivos verificados e já renomeados, com a marca de uso
//!   para o LRU. Arquivo do cache fora do índice não é confiável: conta como ausente e sai no
//!   [`ContentStore::startup`].
//! - Montagem: `servers\<serverId>\<resource>\<path>` por hard link (cópia se não der); o que está montado fica
//!   registrado em `servers\.mounts\<serverId>.json` (marcado incompleto enquanto a pasta muda).
//! - Limite do cache: conta cache + `tmp` + cópias montadas; ao despejar um hash, desmonta antes os servidores fora de
//!   favoritos/recentes que o usam, para o espaço sair de fato do disco.
//! - Nada aqui é executado nem aberto: só bytes conferidos por hash.
//!
//! O manifest vem de um servidor de jogo qualquer (não confiável): tudo é validado antes de qualquer download, e o
//! HTTP não segue redirecionamentos, tem prazos e nunca lê além do tamanho declarado.

use crate::error::{AppError, AppResult};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::Semaphore;
use tokio::task::JoinSet;
use tokio_util::sync::CancellationToken;
use url::Url;

/// Máximo de arquivos num manifest.
pub const MAX_FILES: usize = 10_000;
/// Downloads simultâneos.
pub const PARALLEL: usize = 4;
/// Tentativas por arquivo.
pub const ATTEMPTS: u32 = 3;
/// Intervalo mínimo entre eventos de progresso (no máx. 10/s).
pub const PROGRESS_INTERVAL: Duration = Duration::from_millis(100);
/// Teto do corpo de `/cb/manifest` (10 000 arquivos cabem com folga).
const MAX_MANIFEST_BYTES: usize = 16 * 1024 * 1024;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
/// Sem bytes por este tempo → falha (tentada de novo). Não há prazo total: arquivos podem ser grandes.
const IDLE_TIMEOUT: Duration = Duration::from_secs(30);
/// Prazo total do manifest (pequeno, JSON).
const MANIFEST_TIMEOUT: Duration = Duration::from_secs(20);
/// Espera antes da 2ª tentativa; dobra a cada falha.
const RETRY_DELAY: Duration = Duration::from_secs(1);
/// Abaixo desta média (depois da carência) a tentativa é abandonada e repetida: servidor que pinga 1 byte a cada
/// 29 s não prende a entrada para sempre.
const MIN_RATE_BYTES_PER_SEC: u64 = 16 * 1024;
const SLOW_GRACE: Duration = Duration::from_secs(60);
const MAX_NAME: usize = 64;
/// Segmento e caminho curtos o bastante para `servers\<id>\<resource>\<caminho>` ficar abaixo do MAX_PATH do jogo.
const MAX_SEGMENT: usize = 100;
const MAX_PATH_LEN: usize = 180;
const TMP_DIR: &str = "tmp";
const INDEX_FILE: &str = "index.json";
const MOUNTS_DIR: &str = ".mounts";

// ----- manifest -----

/// `GET /cb/manifest` (formato de `ResourceStore::manifestJson` no `cb-server`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Manifest {
    pub server_id: String,
    pub protocol: u32,
    pub resources: Vec<ResourceEntry>,
    pub total_size: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ResourceEntry {
    pub name: String,
    pub version: String,
    pub files: Vec<ManifestFile>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ManifestFile {
    pub path: String,
    pub sha256: String,
    pub size: u64,
}

/// Limites aplicados ao manifest antes de baixar qualquer coisa.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Limits {
    pub max_files: usize,
    pub max_total_bytes: u64,
    /// Só para a mensagem de erro.
    pub max_total_mb: u64,
}

impl Limits {
    /// `maxServerContentMB` das configurações.
    pub fn new(max_server_content_mb: u64) -> Self {
        Self {
            max_files: MAX_FILES,
            max_total_bytes: max_server_content_mb.saturating_mul(1024 * 1024),
            max_total_mb: max_server_content_mb,
        }
    }
}

/// Um arquivo a baixar (hash único), com o primeiro lugar onde aparece para mensagens e progresso.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileRef {
    pub sha256: String,
    pub size: u64,
    pub resource: String,
    pub path: String,
}

/// Carga de `content://progress`.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Progress {
    pub done_bytes: u64,
    pub total_bytes: u64,
    pub bytes_per_sec: u64,
    /// `resource/caminho` do último arquivo com bytes recebidos.
    pub current: String,
}

/// Texto vindo do servidor dentro de mensagem: curto, entre aspas e só ASCII visível (controle, bidi e o resto viram
/// `\u{…}`; a UI mostra sempre como texto).
fn quoted(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars().take(MAX_NAME + 16) {
        if c.is_ascii_graphic() || c == ' ' {
            out.push(c);
        } else {
            out.extend(c.escape_unicode());
        }
    }
    out.push('"');
    out
}

fn invalid(resource: &str, what: impl std::fmt::Display) -> AppError {
    AppError::new(
        "invalid_manifest",
        format!(
            "O servidor enviou uma lista de conteúdo inválida (resource {}): {what}.",
            quoted(resource)
        ),
    )
}

fn invalid_body() -> AppError {
    AppError::new(
        "invalid_manifest",
        "O servidor enviou uma lista de conteúdo inválida.",
    )
}

fn unavailable() -> AppError {
    AppError::new(
        "server_unavailable",
        "Servidor indisponível: não foi possível obter a lista de conteúdo.",
    )
}

fn unavailable_for(f: &FileRef) -> AppError {
    AppError::new(
        "server_unavailable",
        format!(
            "Servidor indisponível: não foi possível baixar o resource {}.",
            quoted(&f.resource)
        ),
    )
}

fn cancelled() -> AppError {
    AppError::new("cancelled", "Download cancelado.")
}

/// `[A-Za-z0-9._-]`, não vazio, diferente de `.`/`..` (mesma regra de `cbmp::safeSegment`). Além disso recusa o
/// que o Windows trataria de outro jeito: ponto no fim (o Windows o remove) e nomes de dispositivo (`CON`, `NUL`…).
fn safe_segment(s: &str) -> bool {
    if s.is_empty() || s.len() > MAX_SEGMENT || s == "." || s == ".." || s.ends_with('.') {
        return false;
    }
    if !s
        .bytes()
        .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'-'))
    {
        return false;
    }
    let stem = s.split('.').next().unwrap_or("").to_ascii_uppercase();
    let reserved = matches!(
        stem.as_str(),
        "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$"
    ) || ((stem.starts_with("COM") || stem.starts_with("LPT"))
        && stem.len() == 4
        && stem.as_bytes()[3].is_ascii_digit());
    !reserved
}

/// Nome de resource: 1 a 64 caracteres seguros.
pub fn valid_resource_name(s: &str) -> bool {
    s.len() <= MAX_NAME && safe_segment(s)
}

/// Caminho relativo `seg(/seg)*` com segmentos seguros: sem `..`, `\`, `:`, absoluto ou segmento vazio; no máximo
/// 180 caracteres (100 por segmento).
pub fn safe_rel_path(rel: &str) -> bool {
    !rel.is_empty() && rel.len() <= MAX_PATH_LEN && rel.split('/').all(safe_segment)
}

/// Id do servidor vira nome de pasta: segmento seguro que não começa com ponto (`.mounts` é nosso).
fn valid_server_id(id: &str) -> bool {
    valid_resource_name(id) && !id.starts_with('.')
}

fn valid_sha(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f'))
}

fn too_large(limits: &Limits) -> AppError {
    AppError::new(
        "content_too_large",
        format!(
            "O conteúdo do servidor passa do limite de {} MB (Configurações).",
            limits.max_total_mb
        ),
    )
}

/// Confere tudo antes de qualquer download: servidor/protocolo, nomes, caminhos, hashes, quantidade e tamanho.
pub fn validate_manifest(
    m: &Manifest,
    expected_server: &str,
    expected_protocol: u32,
    limits: &Limits,
) -> AppResult<()> {
    if m.server_id != expected_server || m.protocol != expected_protocol {
        return Err(AppError::new(
            "manifest_mismatch",
            "O conteúdo anunciado não corresponde a este servidor (id ou protocolo diferente do Hub).",
        ));
    }
    let count: usize = m.resources.iter().map(|r| r.files.len()).sum();
    if count > limits.max_files {
        return Err(AppError::new(
            "content_too_large",
            format!(
                "O servidor anuncia mais de {} arquivos de conteúdo.",
                limits.max_files
            ),
        ));
    }
    if m.total_size > limits.max_total_bytes {
        return Err(too_large(limits));
    }
    let mut names = HashSet::new();
    let mut sizes: HashMap<&str, u64> = HashMap::new();
    let mut total: u64 = 0;
    for r in &m.resources {
        if !valid_resource_name(&r.name) {
            return Err(invalid(&r.name, "nome de resource inválido"));
        }
        if !names.insert(r.name.to_ascii_lowercase()) {
            return Err(invalid(&r.name, "resource repetido"));
        }
        // Windows não diferencia maiúsculas: comparações de caminho em minúsculas.
        let mut files = HashSet::new();
        let mut dirs = HashSet::new();
        for f in &r.files {
            if !safe_rel_path(&f.path) {
                return Err(invalid(
                    &r.name,
                    format_args!("caminho não permitido {}", quoted(&f.path)),
                ));
            }
            if !valid_sha(&f.sha256) {
                return Err(invalid(
                    &r.name,
                    format_args!("hash inválido em {}", quoted(&f.path)),
                ));
            }
            if *sizes.entry(&f.sha256).or_insert(f.size) != f.size {
                return Err(invalid(
                    &r.name,
                    format_args!(
                        "tamanhos diferentes para o mesmo hash em {}",
                        quoted(&f.path)
                    ),
                ));
            }
            total = total.checked_add(f.size).ok_or_else(|| too_large(limits))?;
            let lower = f.path.to_ascii_lowercase();
            let mut acc = String::new();
            let segs: Vec<&str> = lower.split('/').collect();
            for seg in &segs[..segs.len() - 1] {
                if !acc.is_empty() {
                    acc.push('/');
                }
                acc.push_str(seg);
                dirs.insert(acc.clone());
            }
            if !files.insert(lower) {
                return Err(invalid(
                    &r.name,
                    format_args!("caminho repetido {}", quoted(&f.path)),
                ));
            }
        }
        if let Some(clash) = files.iter().find(|f| dirs.contains(*f)) {
            return Err(invalid(
                &r.name,
                format_args!("{} é arquivo e pasta ao mesmo tempo", quoted(clash)),
            ));
        }
    }
    if total > limits.max_total_bytes {
        return Err(too_large(limits));
    }
    if total != m.total_size {
        return Err(invalid_body());
    }
    Ok(())
}

// ----- HTTP -----

/// `http://host:porta/` do servidor; IPv6 entre colchetes. O Hub só anuncia `host` + `httpPort` e o `cb-server` serve
/// HTTP puro: o conteúdo é endereçado por hash e conferido aqui, então http basta.
pub fn server_base_url(host: &str, http_port: u16) -> AppResult<Url> {
    let bad = || {
        AppError::new(
            "server_unavailable",
            "Servidor indisponível: endereço anunciado inválido.",
        )
    };
    if http_port == 0 {
        return Err(bad());
    }
    let bare = host
        .strip_prefix('[')
        .and_then(|h| h.strip_suffix(']'))
        .unwrap_or(host);
    let parsed = if bare.contains(':') {
        let ip: std::net::Ipv6Addr = bare.parse().map_err(|_| bad())?;
        url::Host::<String>::Ipv6(ip)
    } else {
        if bare.is_empty()
            || !bare
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'-'))
        {
            return Err(bad());
        }
        url::Host::parse(bare).map_err(|_| bad())?
    };
    Url::parse(&format!("http://{parsed}:{http_port}/")).map_err(|_| bad())
}

/// Cliente do conteúdo: sem redirecionamentos (nem para o mesmo host), conexão 10 s, ociosidade 30 s.
fn content_client() -> reqwest::Client {
    if rustls::crypto::CryptoProvider::get_default().is_none() {
        let _ = rustls::crypto::ring::default_provider().install_default();
    }
    reqwest::Client::builder()
        .connect_timeout(CONNECT_TIMEOUT)
        .read_timeout(IDLE_TIMEOUT)
        .redirect(reqwest::redirect::Policy::none())
        .user_agent(concat!("CBMultiplayer/", env!("CARGO_PKG_VERSION")))
        .build()
        .expect("cliente HTTP")
}

fn check_base(base: &Url) -> AppResult<()> {
    let ok = matches!(base.scheme(), "http" | "https")
        && base.host().is_some()
        && base.username().is_empty()
        && base.password().is_none();
    if ok {
        Ok(())
    } else {
        Err(unavailable())
    }
}

/// `GET <base>/cb/manifest` (até 16 MiB). Só lê e interpreta: valide com [`validate_manifest`].
pub async fn fetch_manifest(base: &Url) -> AppResult<Manifest> {
    check_base(base)?;
    let url = base.join("cb/manifest").map_err(|_| unavailable())?;
    let mut resp = content_client()
        .get(url)
        .timeout(MANIFEST_TIMEOUT)
        .send()
        .await
        .map_err(|_| unavailable())?;
    if !resp.status().is_success() {
        return Err(unavailable());
    }
    if resp
        .content_length()
        .is_some_and(|n| n > MAX_MANIFEST_BYTES as u64)
    {
        return Err(invalid_body());
    }
    let mut body = Vec::new();
    while let Some(chunk) = resp.chunk().await.map_err(|_| unavailable())? {
        if body.len() + chunk.len() > MAX_MANIFEST_BYTES {
            return Err(invalid_body());
        }
        body.extend_from_slice(&chunk);
    }
    serde_json::from_slice(&body).map_err(|_| invalid_body())
}

// ----- índice de uso (LRU) -----

/// Só hashes verificados e já renomeados para o cache: ter entrada aqui é o que torna um arquivo do cache confiável
/// (depois disso basta conferir o tamanho).
#[derive(Default, Serialize, Deserialize)]
struct Index {
    /// sha256 → marca de uso (ms desde a época, estritamente crescente entre chamadas).
    used: HashMap<String, u64>,
}

struct IndexState {
    path: PathBuf,
    loaded: Option<Index>,
    last_tick: u64,
}

impl IndexState {
    fn new(root: &Path) -> Self {
        Self {
            path: root.join(INDEX_FILE),
            loaded: None,
            last_tick: 0,
        }
    }

    fn get(&mut self) -> &mut Index {
        let path = &self.path;
        self.loaded.get_or_insert_with(|| {
            std::fs::read(path)
                .ok()
                .and_then(|b| serde_json::from_slice(&b).ok())
                .unwrap_or_default()
        })
    }

    fn contains(&mut self, sha: &str) -> bool {
        self.get().used.contains_key(sha)
    }

    /// Marca uso (e inclui no índice). Falha ao gravar só vai para o log: não derruba a entrada no servidor.
    fn touch<'a>(&mut self, hashes: impl IntoIterator<Item = &'a str>) {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_millis() as u64)
            .unwrap_or(0);
        let max_seen = self.get().used.values().copied().max().unwrap_or(0);
        let tick = now.max(self.last_tick.max(max_seen) + 1);
        self.last_tick = tick;
        let idx = self.get();
        for h in hashes {
            idx.used.insert(h.to_string(), tick);
        }
        self.save_logged();
    }

    fn save_logged(&mut self) {
        let body = serde_json::to_vec(self.get()).expect("índice serializável");
        if let Err(e) = write_atomic(&self.path, &body) {
            log::warn!("índice do cache não gravado: {}", e.code);
        }
    }
}

/// Grava em `<arquivo>.tmp` com `fsync` e troca pelo `rename`.
fn write_atomic(path: &Path, body: &[u8]) -> AppResult<()> {
    use std::io::Write as _;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| AppError::io(dir, &e))?;
    }
    let tmp = path.with_extension("json.tmp");
    let write = || -> std::io::Result<()> {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(body)?;
        f.sync_all()
    };
    write().map_err(|e| AppError::io(&tmp, &e))?;
    std::fs::rename(&tmp, path).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        AppError::io(path, &e)
    })?;
    if let Some(dir) = path.parent() {
        sync_dir(dir);
    }
    Ok(())
}

/// `fsync` da pasta depois de um `rename`, onde o sistema permite. No Windows não há como abrir pasta pela std; lá o
/// NTFS registra o `rename` no journal e o que importa é o `sync_all` do arquivo antes dele.
fn sync_dir(dir: &Path) {
    #[cfg(unix)]
    if let Ok(f) = std::fs::File::open(dir) {
        let _ = f.sync_all();
    }
    #[cfg(not(unix))]
    let _ = dir;
}

fn set_readonly(path: &Path) -> std::io::Result<()> {
    let mut p = std::fs::metadata(path)?.permissions();
    p.set_readonly(true);
    std::fs::set_permissions(path, p)
}

/// Remove arquivo (mesmo somente leitura), pasta ou link; ausente não é erro.
fn remove_entry(path: &Path) -> std::io::Result<()> {
    let meta = match std::fs::symlink_metadata(path) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(e) => return Err(e),
    };
    if meta.is_dir() {
        // `remove_dir_all` não segue links/junções.
        std::fs::remove_dir_all(path)
    } else {
        // Link para pasta (junção) sai com `remove_dir`.
        std::fs::remove_file(path).or_else(|_| std::fs::remove_dir(path))
    }
}

/// Soma dos arquivos sob `dir` (sem seguir links).
fn dir_size(dir: &Path) -> u64 {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return 0;
    };
    rd.flatten()
        .map(|e| match e.file_type() {
            Ok(t) if t.is_dir() => dir_size(&e.path()),
            Ok(t) if t.is_file() => e.metadata().map(|m| m.len()).unwrap_or(0),
            _ => 0,
        })
        .sum()
}

// ----- progresso -----

struct Tracker {
    total: u64,
    done: u64,
    net: u64,
    start: Instant,
    last_emit: Option<Instant>,
    current: String,
    sink: Arc<dyn Fn(Progress) + Send + Sync>,
}

impl Tracker {
    /// `delta` pode ser negativo (resposta 200 a um pedido com Range, `.part` descartado).
    fn add(&mut self, delta: i64, network: u64, current: &str) {
        self.done = self.done.saturating_add_signed(delta);
        self.net += network;
        if self.current != current {
            self.current = current.to_string();
        }
        self.emit(false);
    }

    fn emit(&mut self, force: bool) {
        let now = Instant::now();
        if !force && self.last_emit.is_some_and(|t| now - t < PROGRESS_INTERVAL) {
            return;
        }
        self.last_emit = Some(now);
        let secs = (now - self.start).as_secs_f64();
        let bytes_per_sec = if secs > 0.0 {
            (self.net as f64 / secs) as u64
        } else {
            0
        };
        (self.sink)(Progress {
            done_bytes: self.done.min(self.total),
            total_bytes: self.total,
            bytes_per_sec,
            current: self.current.clone(),
        });
    }
}

// ----- montagens -----

/// `servers\.mounts\<id>.json`: o que está em `servers\<id>`. `complete = false` enquanto a pasta muda; registro
/// incompleto (queda no meio) não protege nada e a montagem é refeita/apagada.
#[derive(Default, Serialize, Deserialize)]
struct MountRecord {
    complete: bool,
    /// "resource/caminho" → arquivo montado.
    files: HashMap<String, Mounted>,
}

#[derive(Clone, Serialize, Deserialize)]
struct Mounted {
    sha256: String,
    size: u64,
    /// Cópia (não hard link): ocupa espaço próprio e conta no limite do cache.
    copied: bool,
}

impl MountRecord {
    fn copied_bytes(&self) -> u64 {
        self.files
            .values()
            .filter(|f| f.copied)
            .map(|f| f.size)
            .sum()
    }
}

// ----- loja -----

struct Inner {
    root: PathBuf,
    servers: PathBuf,
    http: reqwest::Client,
    retry_delay: Duration,
    hard_links: bool,
    index: Mutex<IndexState>,
    /// Downloads em andamento: só as pastas deles sobrevivem à varredura de `tmp`.
    jobs: Mutex<HashSet<String>>,
}

/// Cache de conteúdo + montagem por servidor. Barato de clonar (estado compartilhado): use uma instância só por
/// processo (o índice e as pastas de download são coordenados por ela). Métodos síncronos fazem E/S de disco
/// (até copiar 2 GB na montagem por cópia): chame-os por `spawn_blocking`.
#[derive(Clone)]
pub struct ContentStore {
    inner: Arc<Inner>,
}

/// Falha de uma tentativa: `Retry` tenta de novo (rede, 5xx, corpo curto, lento); `Fatal` encerra o download todo.
enum Failure {
    Retry(AppError),
    Fatal(AppError),
}

/// Pasta `cache\tmp\<job>` de um download: apagada (com os `.part`) quando o download termina de qualquer jeito,
/// inclusive se o future for abandonado.
struct Job {
    store: ContentStore,
    id: String,
    dir: PathBuf,
}

impl Drop for Job {
    fn drop(&mut self) {
        if let Err(e) = remove_entry(&self.dir) {
            log::warn!("pasta de download não apagada: {e}");
        }
        lock(&self.store.inner.jobs).remove(&self.id);
    }
}

impl ContentStore {
    /// `root` = `cache\`, `servers` = `servers\` (ver [`crate::paths`]).
    pub fn new(root: PathBuf, servers: PathBuf) -> Self {
        Self {
            inner: Arc::new(Inner {
                index: Mutex::new(IndexState::new(&root)),
                root,
                servers,
                http: content_client(),
                retry_delay: RETRY_DELAY,
                hard_links: true,
                jobs: Mutex::new(HashSet::new()),
            }),
        }
    }

    /// Nova loja com as mesmas pastas e cliente, e a configuração alterada (estado em memória novo).
    fn rebuild(&self, f: impl FnOnce(&mut Inner)) -> Self {
        let i = &self.inner;
        let mut inner = Inner {
            root: i.root.clone(),
            servers: i.servers.clone(),
            http: i.http.clone(),
            retry_delay: i.retry_delay,
            hard_links: i.hard_links,
            index: Mutex::new(IndexState::new(&i.root)),
            jobs: Mutex::new(HashSet::new()),
        };
        f(&mut inner);
        Self {
            inner: Arc::new(inner),
        }
    }

    /// Espera antes da 2ª tentativa (dobra a cada falha). Padrão 1 s; testes usam menos.
    pub fn with_retry_delay(self, delay: Duration) -> Self {
        self.rebuild(|i| i.retry_delay = delay)
    }

    /// `false` monta sempre por cópia (o caminho de fallback, para testes).
    pub fn with_hard_links(self, enabled: bool) -> Self {
        self.rebuild(|i| i.hard_links = enabled)
    }

    pub fn root(&self) -> &Path {
        &self.inner.root
    }

    fn cache_path(&self, sha: &str) -> PathBuf {
        self.inner.root.join(sha)
    }

    fn tmp_dir(&self) -> PathBuf {
        self.inner.root.join(TMP_DIR)
    }

    fn index(&self) -> MutexGuard<'_, IndexState> {
        lock(&self.inner.index)
    }

    /// Na abertura do launcher: apaga sobras de downloads e arquivos do cache sem entrada no índice (podem ter sido
    /// gravados pela metade antes de uma queda); tira do índice o que não existe mais.
    pub fn startup(&self) {
        self.sweep_tmp();
        let root = &self.inner.root;
        let mut idx = self.index();
        if let Ok(rd) = std::fs::read_dir(root) {
            for e in rd.flatten() {
                let name = e.file_name().to_string_lossy().into_owned();
                if valid_sha(&name) && !idx.contains(&name) {
                    if let Err(err) = remove_entry(&e.path()) {
                        log::warn!("arquivo do cache fora do índice não apagado: {err}");
                    }
                }
            }
        }
        idx.get().used.retain(|h, _| root.join(h).is_file());
        idx.save_logged();
    }

    /// Apaga de `cache\tmp` tudo que não pertence a um download em andamento.
    fn sweep_tmp(&self) {
        let jobs = lock(&self.inner.jobs);
        let Ok(rd) = std::fs::read_dir(self.tmp_dir()) else {
            return;
        };
        for e in rd.flatten() {
            if !jobs.contains(e.file_name().to_string_lossy().as_ref()) {
                if let Err(err) = remove_entry(&e.path()) {
                    log::warn!("sobra de download não apagada: {err}");
                }
            }
        }
    }

    fn begin_job(&self) -> AppResult<Job> {
        let mut raw = [0u8; 16];
        getrandom::fill(&mut raw)
            .map_err(|_| AppError::new("internal_error", "Falha interna no download."))?;
        let id = hex(&raw);
        let dir = self.tmp_dir().join(&id);
        // Registrado antes de criar a pasta: a varredura (com o mesmo lock) nunca apaga um download vivo.
        lock(&self.inner.jobs).insert(id.clone());
        let job = Job {
            store: self.clone(),
            id,
            dir,
        };
        std::fs::create_dir_all(&job.dir).map_err(|e| AppError::io(&job.dir, &e))?;
        Ok(job)
    }

    /// Hashes do manifest que não estão no cache (um por hash, na ordem do manifest). Só conta como presente o que
    /// está no índice (verificado) e com o tamanho certo.
    pub fn missing(&self, m: &Manifest) -> Vec<FileRef> {
        let mut idx = self.index();
        let mut seen = HashSet::new();
        let mut out = Vec::new();
        for r in &m.resources {
            for f in &r.files {
                if !seen.insert(f.sha256.as_str()) {
                    continue;
                }
                let present = valid_sha(&f.sha256)
                    && idx.contains(&f.sha256)
                    && std::fs::metadata(self.cache_path(&f.sha256))
                        .is_ok_and(|md| md.is_file() && md.len() == f.size);
                if !present {
                    out.push(FileRef {
                        sha256: f.sha256.clone(),
                        size: f.size,
                        resource: r.name.clone(),
                        path: f.path.clone(),
                    });
                }
            }
        }
        out
    }

    /// Baixa `files` de `<base>/cb/files/<sha256>`: 4 por vez, 3 tentativas cada com espera crescente, retomada por
    /// `Range` dentro deste download. Progresso no máx. 10/s (+ um evento final). O primeiro erro fatal interrompe os
    /// demais. A pasta de trabalho deste download é apagada ao terminar, com sucesso ou não.
    pub async fn download(
        &self,
        base: &Url,
        files: &[FileRef],
        progress: impl Fn(Progress) + Send + Sync + 'static,
        cancel: CancellationToken,
    ) -> AppResult<()> {
        check_base(base)?;
        if files.iter().any(|f| !valid_sha(&f.sha256)) {
            return Err(invalid_body());
        }
        self.sweep_tmp();
        let job = self.begin_job()?;
        let job_dir = Arc::new(job.dir.clone());
        let tracker = Arc::new(Mutex::new(Tracker {
            total: files.iter().map(|f| f.size).sum(),
            done: 0,
            net: 0,
            start: Instant::now(),
            last_emit: None,
            current: String::new(),
            sink: Arc::new(progress),
        }));
        let finished = Arc::new(Mutex::new(Vec::<String>::new()));
        let abort = cancel.child_token();
        let permits = Arc::new(Semaphore::new(PARALLEL));
        let mut tasks = JoinSet::new();
        for f in files {
            let f = f.clone();
            let (store, base, abort, permits, tracker, job_dir, finished) = (
                self.clone(),
                base.clone(),
                abort.clone(),
                permits.clone(),
                tracker.clone(),
                job_dir.clone(),
                finished.clone(),
            );
            tasks.spawn(async move {
                let _permit = tokio::select! {
                    p = permits.acquire_owned() => p.expect("semáforo aberto"),
                    _ = abort.cancelled() => return Err(cancelled()),
                };
                let part = job_dir.join(format!("{}.part", f.sha256));
                store
                    .fetch_with_retries(&base, &f, &part, &tracker, &abort)
                    .await?;
                lock(&finished).push(f.sha256.clone());
                Ok(())
            });
        }
        let mut first_err: Option<AppError> = None;
        while let Some(joined) = tasks.join_next().await {
            let r = joined.unwrap_or_else(|_| {
                Err(AppError::new(
                    "internal_error",
                    "Falha interna no download.",
                ))
            });
            if let Err(e) = r {
                abort.cancel();
                if first_err.is_none() && e.code != "cancelled" {
                    first_err = Some(e);
                }
            }
        }
        // Só agora (depois do `rename`) os arquivos verificados entram no índice, mesmo se outro falhou.
        let done = std::mem::take(&mut *lock(&finished));
        if !done.is_empty() {
            self.index().touch(done.iter().map(String::as_str));
        }
        drop(job);
        if cancel.is_cancelled() {
            return Err(cancelled());
        }
        if let Some(e) = first_err {
            return Err(e);
        }
        lock(&tracker).emit(true);
        Ok(())
    }

    async fn fetch_with_retries(
        &self,
        base: &Url,
        f: &FileRef,
        part: &Path,
        tracker: &Mutex<Tracker>,
        abort: &CancellationToken,
    ) -> AppResult<()> {
        let mut delay = self.inner.retry_delay;
        let mut counted: u64 = 0;
        for attempt in 1..=ATTEMPTS {
            match self
                .fetch_once(base, f, part, tracker, abort, &mut counted)
                .await
            {
                Ok(()) => return Ok(()),
                Err(Failure::Fatal(e)) => return Err(e),
                Err(Failure::Retry(e)) if attempt == ATTEMPTS => return Err(e),
                Err(Failure::Retry(_)) => {
                    tokio::select! {
                        _ = tokio::time::sleep(delay) => {}
                        _ = abort.cancelled() => return Err(cancelled()),
                    }
                    delay *= 2;
                }
            }
        }
        unreachable!("o laço sempre retorna")
    }

    /// Uma tentativa. `counted` = bytes deste arquivo já somados ao progresso (sobrevive entre tentativas). O `.part`
    /// só existe dentro deste download (tentativa anterior contra o mesmo servidor).
    async fn fetch_once(
        &self,
        base: &Url,
        f: &FileRef,
        part: &Path,
        tracker: &Mutex<Tracker>,
        abort: &CancellationToken,
        counted: &mut u64,
    ) -> Result<(), Failure> {
        let label = format!("{}/{}", f.resource, f.path);
        let io = |p: &Path, e: std::io::Error| Failure::Fatal(AppError::io(p, &e));
        let set_counted = |n: u64, net: u64, counted: &mut u64| {
            lock(tracker).add(n as i64 - *counted as i64, net, &label);
            *counted = n;
        };
        let size_error = |what: &str| {
            Failure::Fatal(AppError::new(
                "size_mismatch",
                format!(
                    "{what} no resource {} ({}).",
                    quoted(&f.resource),
                    quoted(&f.path)
                ),
            ))
        };
        let oversize = || size_error("Arquivo maior que o declarado");

        // Retomada: hash do que já está no `.part`. Sobra maior que o arquivo, ou completa mas errada, recomeça.
        let mut hasher = Sha256::new();
        let mut have: u64 = match std::fs::metadata(part) {
            Ok(md) if md.len() <= f.size => md.len(),
            Ok(_) => {
                std::fs::remove_file(part).map_err(|e| io(part, e))?;
                0
            }
            Err(_) => 0,
        };
        if have > 0 {
            hash_file_into(part, &mut hasher)
                .await
                .map_err(|e| io(part, e))?;
            if have == f.size {
                if hex(&hasher.clone().finalize()) == f.sha256 {
                    set_counted(have, 0, counted);
                    return self.finish(f, part, hasher, false);
                }
                std::fs::remove_file(part).map_err(|e| io(part, e))?;
                hasher = Sha256::new();
                have = 0;
            }
        }
        set_counted(have, 0, counted);

        let url = base
            .join(&format!("cb/files/{}", f.sha256))
            .map_err(|_| Failure::Fatal(unavailable_for(f)))?;
        let mut req = self.inner.http.get(url);
        if have > 0 {
            req = req.header(reqwest::header::RANGE, format!("bytes={have}-"));
        }
        let mut resp = tokio::select! {
            r = req.send() => r.map_err(|_| Failure::Retry(unavailable_for(f)))?,
            _ = abort.cancelled() => return Err(Failure::Fatal(cancelled())),
        };
        let status = resp.status();
        let append = if status == reqwest::StatusCode::PARTIAL_CONTENT && have > 0 {
            match content_range(resp.headers()) {
                Some((start, total)) if start == have && total.is_none_or(|t| t == f.size) => true,
                Some((_, Some(t))) if t != f.size => {
                    let _ = std::fs::remove_file(part);
                    return Err(size_error("Tamanho do arquivo diferente do declarado"));
                }
                _ => {
                    // Faixa diferente da pedida: recomeça do zero na próxima tentativa.
                    let _ = std::fs::remove_file(part);
                    set_counted(0, 0, counted);
                    return Err(Failure::Retry(unavailable_for(f)));
                }
            }
        } else if status == reqwest::StatusCode::OK {
            false
        } else if status == reqwest::StatusCode::RANGE_NOT_SATISFIABLE {
            let _ = std::fs::remove_file(part);
            set_counted(0, 0, counted);
            return Err(Failure::Retry(unavailable_for(f)));
        } else if status.is_server_error()
            || status == reqwest::StatusCode::TOO_MANY_REQUESTS
            || status == reqwest::StatusCode::REQUEST_TIMEOUT
        {
            return Err(Failure::Retry(unavailable_for(f)));
        } else {
            return Err(Failure::Fatal(unavailable_for(f)));
        };
        if !append {
            hasher = Sha256::new();
            have = 0;
            set_counted(0, 0, counted);
        }
        if resp.content_length().is_some_and(|n| n > f.size - have) {
            let _ = std::fs::remove_file(part);
            return Err(oversize());
        }

        let mut out = tokio::fs::OpenOptions::new()
            .create(true)
            .write(true)
            .append(append)
            .truncate(!append)
            .open(part)
            .await
            .map_err(|e| io(part, e))?;
        let started = Instant::now();
        let mut received: u64 = 0;
        loop {
            let chunk = tokio::select! {
                c = resp.chunk() => c,
                _ = abort.cancelled() => return Err(Failure::Fatal(cancelled())),
            };
            let chunk = match chunk {
                Ok(Some(c)) => c,
                Ok(None) => break,
                Err(_) => {
                    // Queda/ociosidade: o `.part` fica para a próxima tentativa deste download.
                    let _ = out.flush().await;
                    return Err(Failure::Retry(unavailable_for(f)));
                }
            };
            if have + chunk.len() as u64 > f.size {
                drop(out);
                let _ = std::fs::remove_file(part);
                return Err(oversize());
            }
            out.write_all(&chunk).await.map_err(|e| io(part, e))?;
            hasher.update(&chunk);
            have += chunk.len() as u64;
            received += chunk.len() as u64;
            set_counted(have, chunk.len() as u64, counted);
            let elapsed = started.elapsed();
            if have < f.size
                && elapsed > SLOW_GRACE
                && received < MIN_RATE_BYTES_PER_SEC.saturating_mul(elapsed.as_secs())
            {
                let _ = out.flush().await;
                return Err(Failure::Retry(unavailable_for(f)));
            }
        }
        if have < f.size {
            let _ = out.flush().await;
            return Err(Failure::Retry(unavailable_for(f)));
        }
        // Dados no disco antes do `rename`: depois de uma queda de energia, nada com tamanho certo e conteúdo errado.
        out.flush().await.map_err(|e| io(part, e))?;
        out.sync_all().await.map_err(|e| io(part, e))?;
        drop(out);
        self.finish(f, part, hasher, append)
    }

    /// Confere o hash; certo → somente leitura + `rename` para o cache; errado → apaga. Se o `.part` era retomado, o
    /// erro pode estar no começo já gravado: tenta de novo do zero; baixado inteiro de uma vez → `hash_mismatch`.
    fn finish(
        &self,
        f: &FileRef,
        part: &Path,
        hasher: Sha256,
        resumed: bool,
    ) -> Result<(), Failure> {
        let got = hex(&hasher.finalize());
        if got != f.sha256 {
            let _ = std::fs::remove_file(part);
            if resumed {
                return Err(Failure::Retry(unavailable_for(f)));
            }
            return Err(Failure::Fatal(AppError::new(
                "hash_mismatch",
                format!(
                    "Arquivo corrompido no resource {} ({}): o conteúdo não confere.",
                    quoted(&f.resource),
                    quoted(&f.path)
                ),
            )));
        }
        let dest = self.cache_path(&f.sha256);
        let io = |p: &Path, e: std::io::Error| Failure::Fatal(AppError::io(p, &e));
        set_readonly(part).map_err(|e| io(part, e))?;
        // `rename` não substitui arquivo somente leitura no Windows: remove antes (sobra fora do índice).
        remove_entry(&dest).map_err(|e| io(&dest, e))?;
        std::fs::rename(part, &dest).map_err(|e| io(&dest, e))?;
        sync_dir(&self.inner.root);
        Ok(())
    }

    /// Monta o manifest em `servers\<serverId>` (hard link do cache, ou cópia) e remove o que não faz mais parte.
    /// Todos os arquivos precisam estar no cache (verificados).
    pub fn mount(&self, server_id: &str, m: &Manifest) -> AppResult<PathBuf> {
        if !valid_server_id(server_id) {
            return Err(AppError::new(
                "invalid_server",
                "Identificador de servidor inválido.",
            ));
        }
        // Defesa extra (o chamador já passou por `validate_manifest`): nada de caminho fora da pasta.
        for r in &m.resources {
            if !valid_resource_name(&r.name) {
                return Err(invalid(&r.name, "nome de resource inválido"));
            }
            for f in &r.files {
                if !safe_rel_path(&f.path) || !valid_sha(&f.sha256) {
                    return Err(invalid(
                        &r.name,
                        format_args!("caminho não permitido {}", quoted(&f.path)),
                    ));
                }
            }
        }
        let dir = self.inner.servers.join(server_id);

        // "resource/caminho" → arquivo do manifest.
        let mut want: HashMap<String, &ManifestFile> = HashMap::new();
        for r in &m.resources {
            for f in &r.files {
                want.insert(format!("{}/{}", r.name, f.path), f);
            }
        }
        {
            let mut idx = self.index();
            for f in want.values() {
                let ok = idx.contains(&f.sha256)
                    && std::fs::metadata(self.cache_path(&f.sha256))
                        .is_ok_and(|md| md.is_file() && md.len() == f.size);
                if !ok {
                    return Err(AppError::new(
                        "content_missing",
                        "Conteúdo do servidor incompleto no cache. Tente entrar de novo.",
                    ));
                }
            }
        }

        // Incompleto enquanto a pasta muda: uma queda no meio não deixa o registro antigo valendo.
        let old = self.read_record(server_id).unwrap_or_default();
        self.write_record(server_id, &MountRecord::default())?;

        // A pasta do servidor não pode ser link: removê-lo não toca o destino.
        if std::fs::symlink_metadata(&dir).is_ok_and(|md| !md.is_dir()) {
            remove_entry(&dir).map_err(|e| AppError::io(&dir, &e))?;
        }
        std::fs::create_dir_all(&dir).map_err(|e| AppError::io(&dir, &e))?;

        // Sobras: tudo que não está no manifest novo, que mudou de hash ou de tamanho, ou sem registro completo.
        let keep = |rel: &str, len: u64| {
            want.get(rel).is_some_and(|f| {
                f.size == len
                    && old.complete
                    && old
                        .files
                        .get(rel)
                        .is_some_and(|o| o.sha256 == f.sha256 && o.size == len)
            })
        };
        prune(&dir, &dir, &keep).map_err(|e| AppError::io(&dir, &e))?;

        let mut record = MountRecord {
            complete: true,
            files: HashMap::new(),
        };
        for (rel, f) in &want {
            let target = rel.split('/').fold(dir.clone(), |p, s| p.join(s));
            let mounted = |copied| Mounted {
                sha256: f.sha256.clone(),
                size: f.size,
                copied,
            };
            if std::fs::symlink_metadata(&target).is_ok() {
                // Mantido pelo `prune`.
                let kept = old.files.get(rel).cloned().unwrap_or_else(|| mounted(true));
                record.files.insert(rel.clone(), kept);
                continue;
            }
            if let Some(parent) = target.parent() {
                std::fs::create_dir_all(parent).map_err(|e| AppError::io(parent, &e))?;
            }
            let src = self.cache_path(&f.sha256);
            let linked = self.inner.hard_links && std::fs::hard_link(&src, &target).is_ok();
            if !linked {
                std::fs::copy(&src, &target).map_err(|e| AppError::io(&target, &e))?;
                set_readonly(&target).map_err(|e| AppError::io(&target, &e))?;
            }
            record.files.insert(rel.clone(), mounted(!linked));
        }
        self.write_record(server_id, &record)?;
        self.index().touch(want.values().map(|f| f.sha256.as_str()));
        Ok(dir)
    }

    fn records_dir(&self) -> PathBuf {
        self.inner.servers.join(MOUNTS_DIR)
    }

    fn record_path(&self, server_id: &str) -> PathBuf {
        self.records_dir().join(format!("{server_id}.json"))
    }

    fn read_record(&self, server_id: &str) -> Option<MountRecord> {
        std::fs::read(self.record_path(server_id))
            .ok()
            .and_then(|b| serde_json::from_slice(&b).ok())
    }

    fn write_record(&self, server_id: &str, r: &MountRecord) -> AppResult<()> {
        let body = serde_json::to_vec(r).expect("registro serializável");
        write_atomic(&self.record_path(server_id), &body)
    }

    /// Ids com pasta montada ou registro.
    fn mounted_ids(&self) -> Vec<String> {
        let mut ids: HashSet<String> = HashSet::new();
        if let Ok(rd) = std::fs::read_dir(&self.inner.servers) {
            ids.extend(
                rd.flatten()
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .filter(|n| valid_server_id(n)),
            );
        }
        if let Ok(rd) = std::fs::read_dir(self.records_dir()) {
            ids.extend(rd.flatten().filter_map(|e| {
                let n = e.file_name().to_string_lossy().into_owned();
                n.strip_suffix(".json")
                    .filter(|id| valid_server_id(id))
                    .map(str::to_string)
            }));
        }
        let mut ids: Vec<String> = ids.into_iter().collect();
        ids.sort();
        ids
    }

    /// Apaga a montagem (pasta + registro). Devolve os bytes de cópias que ela ocupava.
    fn unmount(&self, server_id: &str) -> std::io::Result<u64> {
        let copied = self
            .read_record(server_id)
            .map(|r| r.copied_bytes())
            .unwrap_or(0);
        remove_entry(&self.inner.servers.join(server_id))?;
        remove_entry(&self.record_path(server_id))?;
        Ok(copied)
    }

    /// Hashes montados (registro completo) para estes servidores.
    pub fn mounted_hashes<'a>(
        &self,
        server_ids: impl IntoIterator<Item = &'a str>,
    ) -> HashSet<String> {
        server_ids
            .into_iter()
            .filter(|id| valid_server_id(id))
            .filter_map(|id| self.read_record(id))
            .filter(|r| r.complete)
            .flat_map(|r| r.files.into_values().map(|f| f.sha256))
            .collect()
    }

    /// Espaço em disco do conteúdo: arquivos do cache, downloads em andamento e cópias montadas (hard links não
    /// ocupam espaço próprio). Montagem sem registro completo conta inteira.
    pub fn disk_usage(&self) -> u64 {
        let mut total = dir_size(&self.tmp_dir());
        if let Ok(rd) = std::fs::read_dir(&self.inner.root) {
            for e in rd.flatten() {
                if valid_sha(&e.file_name().to_string_lossy()) {
                    total += e
                        .metadata()
                        .ok()
                        .filter(|m| m.is_file())
                        .map_or(0, |m| m.len());
                }
            }
        }
        for id in self.mounted_ids() {
            total += match self.read_record(&id) {
                Some(r) if r.complete => r.copied_bytes(),
                _ => dir_size(&self.inner.servers.join(&id)),
            };
        }
        total
    }

    /// Mantém o conteúdo em até `limit_bytes` ([`Self::disk_usage`]). Montagens quebradas saem sempre. Depois, dos
    /// hashes que nenhum servidor de `keep_servers` (favoritos/recentes) usa, apaga os usados há mais tempo, junto
    /// com as montagens de outros servidores que os usam (senão o hard link mantém os dados no disco). Se ainda
    /// passar, apaga as cópias montadas de servidores fora de `keep_servers`. Devolve os bytes de fato liberados.
    pub fn enforce_limit(
        &self,
        limit_bytes: u64,
        keep_servers: &HashSet<String>,
    ) -> AppResult<u64> {
        self.sweep_tmp();
        let mut live: Vec<(String, MountRecord)> = Vec::new();
        for id in self.mounted_ids() {
            match self.read_record(&id) {
                Some(r) if r.complete && self.inner.servers.join(&id).is_dir() => {
                    live.push((id, r))
                }
                _ => {
                    if let Err(e) = self.unmount(&id) {
                        log::warn!("montagem quebrada não apagada: {e}");
                    }
                }
            }
        }
        let mut usage = self.disk_usage();
        if usage <= limit_bytes {
            return Ok(0);
        }
        let kept: HashSet<&str> = live
            .iter()
            .filter(|(id, _)| keep_servers.contains(id))
            .flat_map(|(_, r)| r.files.values().map(|f| f.sha256.as_str()))
            .collect();
        let root = &self.inner.root;
        let mut cache_files = Vec::new();
        if let Ok(rd) = std::fs::read_dir(root) {
            for e in rd.flatten() {
                let name = e.file_name().to_string_lossy().into_owned();
                if valid_sha(&name) && !kept.contains(name.as_str()) {
                    if let Ok(md) = e.metadata().map(|m| m.len()) {
                        cache_files.push((name, md));
                    }
                }
            }
        }
        let mut index = self.index();
        let mut candidates: Vec<(u64, String, u64)> = cache_files
            .into_iter()
            .map(|(h, len)| {
                let used = index.get().used.get(&h).copied().unwrap_or(0);
                (used, h, len)
            })
            .collect();
        candidates.sort();

        let mut freed = 0u64;
        let mut removed: HashSet<String> = HashSet::new();
        let mut unmount = |id: &str, usage: &mut u64, freed: &mut u64| -> bool {
            if removed.contains(id) {
                return true;
            }
            match self.unmount(id) {
                Ok(copied) => {
                    *usage = usage.saturating_sub(copied);
                    *freed += copied;
                    removed.insert(id.to_string());
                    true
                }
                Err(e) => {
                    log::warn!("montagem não apagada (em uso?): {e}");
                    false
                }
            }
        };
        for (_, h, len) in candidates {
            if usage <= limit_bytes {
                break;
            }
            let users: Vec<&str> = live
                .iter()
                .filter(|(id, r)| {
                    !keep_servers.contains(id) && r.files.values().any(|f| f.sha256 == h)
                })
                .map(|(id, _)| id.as_str())
                .collect();
            let mut all_gone = true;
            for id in users {
                all_gone &= unmount(id, &mut usage, &mut freed);
            }
            if !all_gone {
                continue; // ainda há link para os dados: apagar o arquivo do cache não liberaria nada
            }
            match remove_entry(&root.join(&h)) {
                Ok(()) => {
                    index.get().used.remove(&h);
                    usage = usage.saturating_sub(len);
                    freed += len;
                }
                Err(e) => log::warn!("arquivo do cache não apagado (em uso?): {e}"),
            }
        }
        for (id, r) in &live {
            if usage <= limit_bytes {
                break;
            }
            if !keep_servers.contains(id) && r.copied_bytes() > 0 {
                unmount(id, &mut usage, &mut freed);
            }
        }
        index.save_logged();
        Ok(freed)
    }

    /// "Limpar cache": apaga o cache (arquivos, downloads, índice) e as montagens dos servidores.
    pub fn clear(&self) -> AppResult<()> {
        let mut index = self.index();
        for dir in [&self.inner.root, &self.inner.servers] {
            remove_entry(dir).map_err(|e| AppError::io(dir, &e))?;
        }
        index.loaded = None;
        Ok(())
    }
}

/// Apaga, sob `dir`, arquivos que `keep(rel, tamanho)` recusa, links e pastas que ficarem vazias.
fn prune(base: &Path, dir: &Path, keep: &dyn Fn(&str, u64) -> bool) -> std::io::Result<()> {
    for e in std::fs::read_dir(dir)? {
        let e = e?;
        let path = e.path();
        let ft = e.file_type()?;
        if ft.is_dir() {
            prune(base, &path, keep)?;
            if std::fs::read_dir(&path)?.next().is_none() {
                std::fs::remove_dir(&path)?;
            }
            continue;
        }
        let rel = path
            .strip_prefix(base)
            .ok()
            .and_then(|p| p.to_str())
            .map(|s| s.replace('\\', "/"));
        let len = e.metadata()?.len();
        let wanted = ft.is_file() && rel.is_some_and(|r| keep(&r, len));
        if !wanted {
            remove_entry(&path)?;
        }
    }
    Ok(())
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

async fn hash_file_into(path: &Path, hasher: &mut Sha256) -> std::io::Result<()> {
    let mut f = tokio::fs::File::open(path).await?;
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        let n = f.read(&mut buf).await?;
        if n == 0 {
            return Ok(());
        }
        hasher.update(&buf[..n]);
    }
}

/// `Content-Range: bytes <início>-<fim>/<total|*>` → (início, total).
fn content_range(h: &reqwest::header::HeaderMap) -> Option<(u64, Option<u64>)> {
    let v = h.get(reqwest::header::CONTENT_RANGE)?.to_str().ok()?;
    let (range, total) = v.strip_prefix("bytes ")?.split_once('/')?;
    let start = range.split_once('-')?.0.parse().ok()?;
    let total = if total == "*" {
        None
    } else {
        Some(total.parse().ok()?)
    };
    Some((start, total))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn texto_do_servidor_em_mensagem_e_escapado() {
        assert_eq!(quoted("mapa-1"), "\"mapa-1\"");
        assert_eq!(quoted("a\u{202e}b\n"), "\"a\\u{202e}b\\u{a}\"");
        assert_eq!(quoted(&"x".repeat(500)).len(), 80 + 2);
    }

    #[test]
    fn segmentos_seguros() {
        for ok in [
            "a",
            "a.lua",
            "a-b_c.v2",
            "COM10.lua",
            "console.lua",
            ".hidden",
        ] {
            assert!(safe_segment(ok), "{ok}");
        }
        for bad in [
            "", ".", "..", "a.", "con", "Nul.png", "com1", "LPT9.txt", "a:b", "a b", "ç",
        ] {
            assert!(!safe_segment(bad), "{bad}");
        }
        assert!(valid_server_id("srv_abc-1"));
        assert!(!valid_server_id(".mounts"));
    }

    #[test]
    fn content_range_le_inicio_e_total() {
        let mut h = reqwest::header::HeaderMap::new();
        h.insert(
            reqwest::header::CONTENT_RANGE,
            "bytes 10-99/100".parse().unwrap(),
        );
        assert_eq!(content_range(&h), Some((10, Some(100))));
        h.insert(
            reqwest::header::CONTENT_RANGE,
            "bytes 5-9/*".parse().unwrap(),
        );
        assert_eq!(content_range(&h), Some((5, None)));
        h.insert(reqwest::header::CONTENT_RANGE, "lixo".parse().unwrap());
        assert_eq!(content_range(&h), None);
    }

    #[test]
    fn progresso_serializa_em_camel_case() {
        let v = serde_json::to_value(Progress {
            done_bytes: 1,
            total_bytes: 2,
            bytes_per_sec: 3,
            current: "r/a.lua".into(),
        })
        .unwrap();
        assert_eq!(
            v,
            serde_json::json!({"doneBytes":1,"totalBytes":2,"bytesPerSec":3,"current":"r/a.lua"})
        );
    }
}
