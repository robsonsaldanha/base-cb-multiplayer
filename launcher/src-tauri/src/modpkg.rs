//! Mod do cliente, entregue pelo Hub por `protocol` (`GET /v1/mod/latest?protocol=N` → zip em `/v1/dl/...`).
//!
//! - Baixa com 3 tentativas e espera crescente (o Hub derruba quem fica abaixo de 32 KiB/s e não serve `Range`),
//!   confere tamanho e sha256 anunciados e a assinatura minisign com a chave embutida (`keys/update.pub`, a mesma do
//!   updater). Assinatura inválida → nada é instalado.
//! - Zip validado inteiro antes de gravar: só caminhos relativos seguros (sem `..`, absolutos, `\`, `:`, segmentos
//!   vazios), sem links, sem criptografia, só stored/deflate, cada arquivo até 512 MiB declarados e o total até 2 GiB.
//!   A extração confere que cada arquivo tem exatamente o tamanho declarado.
//! - Instala só em `<jogo>\Phoenix\Binaries\Win64\cbmp\` (pasta inteira do launcher, trocada a cada instalação); nada
//!   fora dela no jogo. `mods\installed.json` guarda versão, protocolo, pasta do jogo e sha256 de cada arquivo; é
//!   apagado antes de mexer na pasta e gravado por último, então instalação interrompida reinstala na próxima vez.
//! - Nunca faz downgrade: versão oferecida menor (semver) que a instalada no mesmo protocolo mantém a instalada.

use crate::content::{hex, remove_entry, safe_rel_path};
use crate::error::{AppError, AppResult};
use crate::game::{GameInstall, BINARIES};
use crate::hub::{self, HubClient, ModInfo};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;
use tokio::io::AsyncWriteExt;
use tokio_util::sync::CancellationToken;
use url::Url;

/// Tamanho máximo declarado de um arquivo dentro do zip.
pub const MAX_ENTRY_BYTES: u64 = 512 * 1024 * 1024;
/// Tamanho máximo do zip baixado.
pub const MAX_MOD_BYTES: u64 = 512 * 1024 * 1024;
/// Soma máxima dos tamanhos declarados no zip.
pub const MAX_TOTAL_BYTES: u64 = 2 * 1024 * 1024 * 1024;
/// Máximo de entradas no zip.
pub const MAX_ENTRIES: usize = 10_000;
/// Tentativas de download.
pub const ATTEMPTS: u32 = 3;
/// Pasta do mod dentro de `Phoenix\Binaries\Win64`.
pub const MOD_FOLDER: &str = "cbmp";
/// Espera antes da 2ª tentativa; dobra a cada falha.
const RETRY_DELAY: Duration = Duration::from_secs(3);
const INSTALLED_FILE: &str = "installed.json";
const DOWNLOAD_DIR: &str = "download";
/// Assinatura minisign antiga (sem pré-hash) exige o arquivo inteiro na memória: só até este tamanho.
const MAX_LEGACY_SIGNED: u64 = 64 * 1024 * 1024;

/// Um arquivo instalado (caminho relativo com `/`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct InstalledFile {
    pub path: String,
    pub sha256: String,
    pub size: u64,
}

/// `mods\installed.json`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Installed {
    pub version: String,
    pub protocol: u32,
    pub game_dir: PathBuf,
    /// sha256 do zip instalado.
    pub sha256: String,
    pub files: Vec<InstalledFile>,
}

/// Resultado de [`ModInstaller::ensure`]: versão em uso e se algo foi (re)instalado agora.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ModState {
    pub version: String,
    pub protocol: u32,
    pub changed: bool,
}

/// `<jogo>\Phoenix\Binaries\Win64\cbmp`.
pub fn mod_dir(game: &GameInstall) -> PathBuf {
    BINARIES
        .iter()
        .fold(game.dir.clone(), |p, s| p.join(s))
        .join(MOD_FOLDER)
}

fn signature_invalid() -> AppError {
    AppError::new(
        "signature_invalid",
        "A assinatura do mod não confere; nada foi instalado.",
    )
}

fn mod_invalid(detail: &str) -> AppError {
    AppError::new(
        "mod_invalid",
        format!("Pacote do mod recusado ({detail}); nada foi instalado."),
    )
}

fn cancelled() -> AppError {
    AppError::new("cancelled", "Download cancelado.")
}

fn bad_offer() -> AppError {
    AppError::new("hub_bad_response", "O Hub anunciou um mod inválido.")
}

// ----- assinatura -----

/// Texto de arquivo minisign (`untrusted comment: ...`), direto ou em base64 (formato do Tauri).
fn minisign_text(s: &str) -> Option<String> {
    use base64::Engine as _;
    let t = s.trim();
    if t.starts_with("untrusted comment:") {
        return Some(t.to_string());
    }
    let raw = base64::engine::general_purpose::STANDARD.decode(t).ok()?;
    let text = String::from_utf8(raw).ok()?;
    text.starts_with("untrusted comment:").then_some(text)
}

fn decode_pubkey(pubkey: &str) -> AppResult<minisign_verify::PublicKey> {
    let pk = match minisign_text(pubkey) {
        Some(text) => minisign_verify::PublicKey::decode(&text),
        // Só a linha da chave (`RW...`).
        None => minisign_verify::PublicKey::from_base64(pubkey.trim()),
    };
    pk.map_err(|_| {
        log::error!("chave pública do mod inválida");
        signature_invalid()
    })
}

fn decode_signature(sig: &str) -> AppResult<minisign_verify::Signature> {
    let text = minisign_text(sig).ok_or_else(signature_invalid)?;
    minisign_verify::Signature::decode(&text).map_err(|_| signature_invalid())
}

/// Confere a assinatura minisign de `data` (`sig`/`pubkey` em texto ou base64 do texto, como no Tauri). Aceita o
/// formato antigo sem pré-hash, como o plugin updater.
pub fn verify_minisign(data: &[u8], sig: &str, pubkey: &str) -> AppResult<()> {
    let pk = decode_pubkey(pubkey)?;
    let sig = decode_signature(sig)?;
    pk.verify(data, &sig, true).map_err(|_| {
        log::warn!("assinatura minisign recusada");
        signature_invalid()
    })
}

/// Como [`verify_minisign`], lendo o arquivo em blocos (assinatura com pré-hash, a padrão).
pub fn verify_minisign_file(path: &Path, sig: &str, pubkey: &str) -> AppResult<()> {
    let pk = decode_pubkey(pubkey)?;
    let signature = decode_signature(sig)?;
    let io = |e: std::io::Error| AppError::io(path, &e);
    match pk.verify_stream(&signature) {
        Ok(mut v) => {
            let mut f = std::fs::File::open(path).map_err(io)?;
            let mut buf = vec![0u8; 256 * 1024];
            loop {
                let n = f.read(&mut buf).map_err(io)?;
                if n == 0 {
                    break;
                }
                v.update(&buf[..n]);
            }
            v.finalize().map_err(|_| {
                log::warn!("assinatura minisign recusada");
                signature_invalid()
            })
        }
        Err(minisign_verify::Error::UnsupportedLegacyMode) => {
            let len = std::fs::metadata(path).map_err(io)?.len();
            if len > MAX_LEGACY_SIGNED {
                return Err(signature_invalid());
            }
            let data = std::fs::read(path).map_err(io)?;
            pk.verify(&data, &signature, true)
                .map_err(|_| signature_invalid())
        }
        Err(_) => {
            log::warn!("assinatura minisign de outra chave");
            Err(signature_invalid())
        }
    }
}

// ----- zip -----

struct Planned {
    index: usize,
    rel: String,
    dir: bool,
    size: u64,
}

/// Valida o zip inteiro (sem gravar nada) e devolve o que extrair.
fn plan<R: Read + std::io::Seek>(ar: &mut zip::ZipArchive<R>) -> AppResult<Vec<Planned>> {
    if ar.len() > MAX_ENTRIES {
        return Err(mod_invalid("arquivos demais"));
    }
    let mut out = Vec::with_capacity(ar.len());
    let mut names = HashSet::new();
    let mut total: u64 = 0;
    for index in 0..ar.len() {
        let e = ar
            .by_index_raw(index)
            .map_err(|_| mod_invalid("pacote corrompido"))?;
        let name = std::str::from_utf8(e.name_raw())
            .map_err(|_| mod_invalid("nome de arquivo inválido"))?;
        if e.encrypted() {
            return Err(mod_invalid("arquivo criptografado"));
        }
        if e.is_symlink() {
            return Err(mod_invalid("link no pacote"));
        }
        // Só arquivo comum ou pasta (quando o zip informa o tipo Unix).
        if let Some(mode) = e.unix_mode() {
            let kind = mode & 0o170000;
            if kind != 0 && kind != 0o100000 && kind != 0o040000 {
                return Err(mod_invalid("tipo de arquivo não permitido"));
            }
        }
        let dir = name.ends_with('/');
        let rel = if dir { &name[..name.len() - 1] } else { name };
        if !safe_rel_path(rel) {
            return Err(mod_invalid("caminho inválido"));
        }
        if !names.insert(rel.to_ascii_lowercase()) {
            return Err(mod_invalid("arquivo repetido"));
        }
        let size = if dir {
            0
        } else {
            if !matches!(
                e.compression(),
                zip::CompressionMethod::Stored | zip::CompressionMethod::Deflated
            ) {
                return Err(mod_invalid("compressão não suportada"));
            }
            let size = e.size();
            if size > MAX_ENTRY_BYTES {
                return Err(mod_invalid("arquivo grande demais"));
            }
            total = total
                .checked_add(size)
                .filter(|t| *t <= MAX_TOTAL_BYTES)
                .ok_or_else(|| mod_invalid("conteúdo grande demais"))?;
            size
        };
        out.push(Planned {
            index,
            rel: rel.to_string(),
            dir,
            size,
        });
    }
    Ok(out)
}

type Archive = zip::ZipArchive<std::fs::File>;

/// Abre e valida o zip inteiro, sem gravar nada.
fn open_planned(zip: &Path) -> AppResult<(Archive, Vec<Planned>)> {
    let f = std::fs::File::open(zip).map_err(|e| AppError::io(zip, &e))?;
    let mut ar = zip::ZipArchive::new(f).map_err(|_| mod_invalid("pacote corrompido"))?;
    let planned = plan(&mut ar)?;
    Ok((ar, planned))
}

fn join_rel(base: &Path, rel: &str) -> PathBuf {
    rel.split('/').fold(base.to_path_buf(), |p, s| p.join(s))
}

/// Extrai `zip` em `dest` com segurança: valida tudo antes (nada é gravado se algo for recusado), troca a pasta
/// `dest` inteira e devolve cada arquivo com sha256 (ordenado por caminho).
pub fn extract_zip(zip: &Path, dest: &Path) -> AppResult<Vec<InstalledFile>> {
    let (mut ar, planned) = open_planned(zip)?;

    remove_entry(dest).map_err(|e| AppError::io(dest, &e))?;
    std::fs::create_dir_all(dest).map_err(|e| AppError::io(dest, &e))?;
    let mut files = Vec::new();
    for p in planned {
        let target = join_rel(dest, &p.rel);
        if p.dir {
            std::fs::create_dir_all(&target).map_err(|e| AppError::io(&target, &e))?;
            continue;
        }
        if let Some(parent) = target.parent() {
            std::fs::create_dir_all(parent).map_err(|e| AppError::io(parent, &e))?;
        }
        let entry = ar
            .by_index(p.index)
            .map_err(|_| mod_invalid("pacote corrompido"))?;
        let mut out = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&target)
            .map_err(|e| AppError::io(&target, &e))?;
        let mut reader = entry.take(p.size + 1);
        let mut hasher = Sha256::new();
        let mut buf = vec![0u8; 256 * 1024];
        let mut n_total: u64 = 0;
        loop {
            // Erro de leitura aqui é zip corrompido (CRC, deflate); de escrita é disco.
            let n = reader
                .read(&mut buf)
                .map_err(|_| mod_invalid("pacote corrompido"))?;
            if n == 0 {
                break;
            }
            n_total += n as u64;
            if n_total > p.size {
                return Err(mod_invalid("tamanho diferente do declarado"));
            }
            hasher.update(&buf[..n]);
            out.write_all(&buf[..n])
                .map_err(|e| AppError::io(&target, &e))?;
        }
        if n_total != p.size {
            return Err(mod_invalid("tamanho diferente do declarado"));
        }
        out.sync_all().map_err(|e| AppError::io(&target, &e))?;
        files.push(InstalledFile {
            path: p.rel,
            sha256: hex(&hasher.finalize()),
            size: p.size,
        });
    }
    files.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(files)
}

fn hash_file(path: &Path) -> std::io::Result<(String, u64)> {
    let mut f = std::fs::File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buf = vec![0u8; 256 * 1024];
    let mut len = 0u64;
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        len += n as u64;
        hasher.update(&buf[..n]);
    }
    Ok((hex(&hasher.finalize()), len))
}

// ----- instalador -----

/// Falha de uma tentativa de download: `Retry` tenta de novo; `Fatal` encerra.
enum Failure {
    Retry(AppError),
    Fatal(AppError),
}

/// Baixa, verifica e instala o mod. `mods_dir` = `%LOCALAPPDATA%\CBMultiplayer\mods`.
pub struct ModInstaller {
    mods_dir: PathBuf,
    pubkey: String,
    retry_delay: Duration,
    http: reqwest::Client,
}

impl ModInstaller {
    pub fn new(mods_dir: PathBuf, pubkey: String) -> Self {
        Self {
            mods_dir,
            pubkey,
            retry_delay: RETRY_DELAY,
            http: hub::download_client(),
        }
    }

    /// Pastas e chave reais do launcher.
    pub fn system() -> Self {
        Self::new(
            crate::paths::mods_dir(),
            crate::update::pubkey().to_string(),
        )
    }

    /// Espera antes da 2ª tentativa (dobra a cada falha). Padrão 3 s; testes usam menos.
    pub fn with_retry_delay(mut self, delay: Duration) -> Self {
        self.retry_delay = delay;
        self
    }

    fn record_path(&self) -> PathBuf {
        self.mods_dir.join(INSTALLED_FILE)
    }

    /// `mods\installed.json`, se existir e for legível.
    pub fn installed(&self) -> Option<Installed> {
        let bytes = std::fs::read(self.record_path()).ok()?;
        serde_json::from_slice(&bytes).ok()
    }

    /// O mod registrado está nesta pasta do jogo e cada arquivo tem o tamanho e o sha256 registrados.
    pub fn check_installed(&self, game: &GameInstall) -> AppResult<bool> {
        Ok(self
            .installed()
            .is_some_and(|r| r.game_dir == game.dir && files_intact(&r, game)))
    }

    /// Garante o mod mais novo do Hub para `protocol` em `game` (ver o topo do módulo).
    pub async fn ensure(
        &self,
        hub: &HubClient,
        protocol: u32,
        game: &GameInstall,
        cancel: CancellationToken,
    ) -> AppResult<ModState> {
        if cancel.is_cancelled() {
            return Err(cancelled());
        }
        let info = hub.mod_latest(protocol).await?.ok_or_else(|| {
            AppError::new(
                "mod_unavailable",
                format!("Ainda não há mod publicado para o protocolo {protocol} deste servidor."),
            )
        })?;
        let url = validate_offer(&info, protocol, hub.base())?;
        let offered = semver::Version::parse(&info.version).map_err(|_| bad_offer())?;

        let current = self
            .installed()
            .filter(|r| r.protocol == protocol && r.game_dir == game.dir);
        if let Some(r) = current {
            let intact = {
                let (r, g) = (r.clone(), game.clone());
                tokio::task::spawn_blocking(move || files_intact(&r, &g))
                    .await
                    .unwrap_or(false)
            };
            let unchanged = |r: &Installed| ModState {
                version: r.version.clone(),
                protocol,
                changed: false,
            };
            if let Ok(cur) = semver::Version::parse(&r.version) {
                if offered < cur {
                    if intact {
                        log::info!(
                            "Hub oferece mod {} menor que o instalado {}; mantido",
                            info.version,
                            r.version
                        );
                        return Ok(unchanged(&r));
                    }
                    return Err(AppError::new(
                        "mod_altered",
                        format!(
                            "Os arquivos do mod {} foram alterados e o Hub só oferece uma versão anterior. \
                             Não fazemos downgrade: aguarde uma versão nova do mod.",
                            r.version
                        ),
                    ));
                }
                if offered == cur && intact {
                    return Ok(unchanged(&r));
                }
            }
            if !intact {
                log::warn!("arquivos do mod alterados; reinstalando");
            }
        }
        self.install(&info, &url, game, cancel).await?;
        Ok(ModState {
            version: info.version,
            protocol,
            changed: true,
        })
    }

    async fn install(
        &self,
        info: &ModInfo,
        url: &Url,
        game: &GameInstall,
        cancel: CancellationToken,
    ) -> AppResult<()> {
        let dl_dir = self.mods_dir.join(DOWNLOAD_DIR);
        // Uma instalação por vez (o `play` é serializado): sobras de downloads antigos saem.
        remove_entry(&dl_dir).map_err(|e| AppError::io(&dl_dir, &e))?;
        std::fs::create_dir_all(&dl_dir).map_err(|e| AppError::io(&dl_dir, &e))?;
        let _cleanup = RemoveOnDrop(dl_dir.clone());
        let mut raw = [0u8; 8];
        getrandom::fill(&mut raw)
            .map_err(|_| AppError::new("internal_error", "Falha interna no download."))?;
        let zip_path = dl_dir.join(format!("{}.zip", hex(&raw)));

        self.download(url, info, &zip_path, &cancel).await?;

        let (zp, sig, pk) = (
            zip_path.clone(),
            info.signature.clone(),
            self.pubkey.clone(),
        );
        tokio::task::spawn_blocking(move || verify_minisign_file(&zp, &sig, &pk))
            .await
            .map_err(|_| AppError::new("internal_error", "Falha interna ao verificar o mod."))??;
        if cancel.is_cancelled() {
            return Err(cancelled());
        }

        let record_path = self.record_path();
        let dest = mod_dir(game);
        let record_base = Installed {
            version: info.version.clone(),
            protocol: info.protocol,
            game_dir: game.dir.clone(),
            sha256: info.sha256.clone(),
            files: Vec::new(),
        };
        tokio::task::spawn_blocking(move || -> AppResult<()> {
            // Zip recusado não mexe em nada (nem no registro da instalação atual).
            open_planned(&zip_path)?;
            // Registro sai antes de mexer na pasta: queda no meio vira "não instalado".
            remove_entry(&record_path).map_err(|e| AppError::io(&record_path, &e))?;
            let files = extract_zip(&zip_path, &dest)?;
            write_record(
                &record_path,
                &Installed {
                    files,
                    ..record_base
                },
            )
        })
        .await
        .map_err(|_| AppError::new("internal_error", "Falha interna ao instalar o mod."))??;
        log::info!(
            "mod {} (protocolo {}) instalado",
            info.version,
            info.protocol
        );
        Ok(())
    }

    /// Até [`ATTEMPTS`] tentativas com espera crescente; queda/lentidão no fim vira `download_slow`.
    async fn download(
        &self,
        url: &Url,
        info: &ModInfo,
        dest: &Path,
        cancel: &CancellationToken,
    ) -> AppResult<()> {
        let mut last = hub::download_slow("o mod");
        for attempt in 0..ATTEMPTS {
            if attempt > 0 {
                let wait = self.retry_delay * 2u32.pow(attempt - 1);
                tokio::select! {
                    _ = cancel.cancelled() => return Err(cancelled()),
                    _ = tokio::time::sleep(wait) => {}
                }
            }
            match self.try_download(url, info, dest, cancel).await {
                Ok(()) => return Ok(()),
                Err(Failure::Fatal(e)) => return Err(e),
                Err(Failure::Retry(e)) => {
                    log::warn!(
                        "download do mod falhou (tentativa {}): {}",
                        attempt + 1,
                        e.code
                    );
                    last = e;
                }
            }
        }
        Err(last)
    }

    async fn try_download(
        &self,
        url: &Url,
        info: &ModInfo,
        dest: &Path,
        cancel: &CancellationToken,
    ) -> Result<(), Failure> {
        let slow = || Failure::Retry(hub::download_slow("o mod"));
        let resp = tokio::select! {
            _ = cancel.cancelled() => return Err(Failure::Fatal(cancelled())),
            r = self.http.get(url.clone()).send() => r,
        };
        let mut resp = match resp {
            Ok(r) => r,
            Err(e) if e.is_connect() => {
                return Err(Failure::Retry(AppError::new(
                    "hub_unreachable",
                    "Não foi possível falar com o Hub. Verifique sua conexão.",
                )))
            }
            Err(_) => return Err(slow()),
        };
        let status = resp.status();
        if !status.is_success() {
            let e = hub::map_hub_error(status.as_u16(), None);
            return Err(if status.is_server_error() || status.as_u16() == 429 {
                Failure::Retry(e)
            } else {
                Failure::Fatal(e)
            });
        }
        if resp.content_length().is_some_and(|n| n != info.size) {
            return Err(Failure::Fatal(mod_invalid(
                "tamanho diferente do anunciado",
            )));
        }
        let io = |e: std::io::Error| Failure::Fatal(AppError::io(dest, &e));
        let mut file = tokio::fs::File::create(dest).await.map_err(io)?;
        let mut hasher = Sha256::new();
        let mut len: u64 = 0;
        loop {
            let chunk = tokio::select! {
                _ = cancel.cancelled() => return Err(Failure::Fatal(cancelled())),
                c = resp.chunk() => c,
            };
            let Some(chunk) = chunk.map_err(|_| slow())? else {
                break;
            };
            len += chunk.len() as u64;
            if len > info.size {
                return Err(Failure::Fatal(mod_invalid("maior que o anunciado")));
            }
            hasher.update(&chunk);
            file.write_all(&chunk).await.map_err(io)?;
        }
        if len < info.size {
            // Corpo curto: o Hub cortou a conexão (lento) ou ela caiu.
            return Err(slow());
        }
        file.flush().await.map_err(io)?;
        file.sync_all().await.map_err(io)?;
        if hex(&hasher.finalize()) != info.sha256 {
            log::warn!("sha256 do mod baixado não confere com o anunciado");
            return Err(Failure::Fatal(mod_invalid("hash diferente do anunciado")));
        }
        Ok(())
    }
}

/// Apaga a pasta ao sair do escopo (sucesso, erro ou future abandonado).
struct RemoveOnDrop(PathBuf);

impl Drop for RemoveOnDrop {
    fn drop(&mut self) {
        if let Err(e) = remove_entry(&self.0) {
            log::warn!("pasta de download do mod não apagada: {e}");
        }
    }
}

/// Oferta do Hub coerente: protocolo pedido, sha256 em hex, tamanho dentro do limite e URL na mesma origem do Hub.
fn validate_offer(info: &ModInfo, protocol: u32, hub_base: &Url) -> AppResult<Url> {
    let sha_ok = info.sha256.len() == 64
        && info
            .sha256
            .bytes()
            .all(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f'));
    if info.protocol != protocol || !sha_ok || info.size == 0 {
        return Err(bad_offer());
    }
    if info.size > MAX_MOD_BYTES {
        return Err(mod_invalid("grande demais"));
    }
    let url = Url::parse(&info.url).map_err(|_| bad_offer())?;
    let same_origin = url.scheme() == hub_base.scheme()
        && url.host() == hub_base.host()
        && url.port_or_known_default() == hub_base.port_or_known_default()
        && url.username().is_empty()
        && url.password().is_none();
    if !same_origin || !hub::allowed_url(&url, cfg!(debug_assertions)) {
        return Err(bad_offer());
    }
    Ok(url)
}

fn files_intact(r: &Installed, game: &GameInstall) -> bool {
    let dir = mod_dir(game);
    !r.files.is_empty()
        && r.files.iter().all(|f| {
            safe_rel_path(&f.path)
                && hash_file(&join_rel(&dir, &f.path))
                    .is_ok_and(|(sha, len)| len == f.size && sha == f.sha256)
        })
}

fn write_record(path: &Path, r: &Installed) -> AppResult<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| AppError::io(parent, &e))?;
    }
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".tmp");
    let tmp = PathBuf::from(tmp);
    let json = serde_json::to_vec_pretty(r).expect("Installed sempre serializa");
    let written = (|| {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(&json)?;
        f.sync_all()?;
        std::fs::rename(&tmp, path)
    })();
    written.map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        AppError::io(path, &e)
    })
}

/// [`ModInstaller::ensure`] com as pastas e a chave reais.
pub async fn ensure_mod(hub: &HubClient, protocol: u32, game: &GameInstall) -> AppResult<ModState> {
    ModInstaller::system()
        .ensure(hub, protocol, game, CancellationToken::new())
        .await
}

/// [`ModInstaller::check_installed`] com as pastas reais.
pub fn check_installed(game: &GameInstall) -> AppResult<bool> {
    ModInstaller::system().check_installed(game)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn offer(url: &str) -> ModInfo {
        ModInfo {
            version: "1.0.0".into(),
            protocol: 3,
            url: url.into(),
            sha256: "a".repeat(64),
            size: 10,
            signature: "x".into(),
        }
    }

    #[test]
    fn oferta_so_da_mesma_origem_do_hub() {
        let hub = Url::parse("https://hub.example.com/").unwrap();
        validate_offer(&offer("https://hub.example.com/v1/dl/t/a.zip"), 3, &hub).unwrap();
        for bad in [
            "https://evil.example.com/v1/dl/t/a.zip",
            "http://hub.example.com/v1/dl/t/a.zip",
            "https://hub.example.com:444/x",
            "https://u:p@hub.example.com/x",
            "nada",
        ] {
            assert_eq!(
                validate_offer(&offer(bad), 3, &hub).unwrap_err().code,
                "hub_bad_response",
                "{bad}"
            );
        }
        let ok = "https://hub.example.com/x";
        assert!(
            validate_offer(&offer(ok), 4, &hub).is_err(),
            "protocolo trocado"
        );
        let mut big = offer(ok);
        big.size = MAX_MOD_BYTES + 1;
        assert_eq!(
            validate_offer(&big, 3, &hub).unwrap_err().code,
            "mod_invalid"
        );
        let mut upper = offer(ok);
        upper.sha256 = "A".repeat(64);
        assert!(validate_offer(&upper, 3, &hub).is_err());
    }

    #[test]
    fn mod_dir_fica_em_win64() {
        let g = GameInstall {
            dir: PathBuf::from("C:/HL"),
            exe: PathBuf::from("C:/HL/HogwartsLegacy.exe"),
            store: crate::game::Store::Manual,
        };
        assert_eq!(
            mod_dir(&g),
            PathBuf::from("C:/HL")
                .join("Phoenix")
                .join("Binaries")
                .join("Win64")
                .join("cbmp")
        );
    }
}
