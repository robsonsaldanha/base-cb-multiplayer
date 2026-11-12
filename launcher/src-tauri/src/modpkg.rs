//! Mod do cliente, entregue pelo Hub por `protocol` (`GET /v1/mod/latest?protocol=N` → zip em `/v1/dl/...`).
//!
//! - Baixa com 3 tentativas e espera crescente (o Hub derruba quem fica abaixo de 32 KiB/s e não serve `Range`),
//!   confere tamanho e sha256 anunciados e a assinatura minisign com a chave embutida (`keys/update.pub`, a mesma do
//!   updater).
//! - **O que está assinado manda, não o Hub.** O comentário confiável da assinatura (coberto por ela) tem de ser
//!   exatamente `cbmp-mod protocol=<N> version=<X.Y.Z> sha256=<hex>` e bater com o protocolo pedido, a versão
//!   anunciada e o sha256 do zip. A versão registrada e usada na regra de downgrade é a assinada. Qualquer
//!   divergência → `mod_signature_invalid` e nada é instalado.
//! - Zip validado inteiro antes de gravar: só caminhos relativos seguros (sem `..`, absolutos, `\`, `:`, segmentos
//!   vazios), sem links, sem criptografia, só stored/deflate, cada arquivo até 512 MiB declarados e o total até 2 GiB.
//!   A extração confere que cada arquivo tem exatamente o tamanho declarado.
//! - **Layout 2**: o zip traz `cbmp-mod.json` (`layout` 2, `version` e `protocol` iguais aos assinados, `minLauncher`
//!   ≤ versão deste launcher), `loader/dwmapi.dll`, `ue4ss/UE4SS.dll` e `ue4ss/Mods/cb-mp/dlls/main.dll`; senão
//!   `mod_invalid` (ou `mod_incompatible` pelo `minLauncher`). Conferido antes de extrair, também no reparo.
//! - **Instalação atômica** em `<jogo>\Phoenix\Binaries\Win64\cbmp\atual\`: o contêiner `cbmp\` é criado se faltar e
//!   nunca renomeado (`cbmp-run.json`, `logs\`, `crash\` e `cb-mp.json` ficam); extrai em `atual.staging`, relê cada
//!   arquivo e confere o hash, e só então troca (`atual` → `atual.old`, `atual.staging` → `atual`, apaga `atual.old`).
//!   Qualquer falha antes da troca deixa o mod que funciona intacto; sobras de uma queda são resolvidas na próxima vez
//!   ([`recover_mod_dir`]). Permissão negada no jogo → `mod_needs_admin`. Ativar o carregador é do [`crate::modact`].
//! - `mods\installed.json` guarda versão (assinada), protocolo, pasta do jogo, sha256 de cada arquivo, `layout`, o
//!   carregador (`loader`, com os anteriores em `loaderHistory`), os arquivos do jogador movidos para
//!   `cbmp-backup\` e `removed`; o zip assinado e o `.sig` ficam em `mods\package\` para reparo local (arquivos
//!   alterados ou mod removido → reinstala do pacote, com a assinatura conferida de novo, sem depender do Hub).
//! - Migração do layout 1 (arquivos direto em `cbmp\`): antes da primeira instalação em `atual\` saem só os arquivos
//!   do registro antigo (e as pastas que ficarem vazias).
//! - Nunca faz downgrade: versão menor (semver) que a instalada no mesmo protocolo mantém a instalada (também depois
//!   de remover o mod: a versão registrada continua sendo o piso).

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
/// Contêiner do mod dentro de `Phoenix\Binaries\Win64` (nunca renomeado).
pub const MOD_FOLDER: &str = "cbmp";
/// Pacote em uso dentro do contêiner.
pub const CURRENT_FOLDER: &str = "atual";
/// Layout do pacote que este launcher instala.
pub const LAYOUT: u32 = 2;
/// Metadados do pacote, na raiz do zip.
pub const MOD_META_FILE: &str = "cbmp-mod.json";
/// Carregador (proxy do UE4SS) dentro do zip; a ativação o copia para `Win64\`.
pub const LOADER_IN_ZIP: &str = "loader/dwmapi.dll";
/// Hashes de carregadores anteriores guardados no registro.
pub const MAX_LOADER_HISTORY: usize = 8;
/// O que todo pacote do layout 2 tem de trazer.
const REQUIRED_IN_ZIP: [&str; 3] = [
    LOADER_IN_ZIP,
    "ue4ss/UE4SS.dll",
    "ue4ss/Mods/cb-mp/dlls/main.dll",
];
/// Tamanho máximo do `cbmp-mod.json`.
const MAX_META_BYTES: u64 = 64 * 1024;
/// Espera antes da 2ª tentativa; dobra a cada falha.
const RETRY_DELAY: Duration = Duration::from_secs(3);
const INSTALLED_FILE: &str = "installed.json";
const DOWNLOAD_DIR: &str = "download";
/// Último pacote instalado (zip assinado + `.sig`), para reparo local.
const PACKAGE_DIR: &str = "package";
const PACKAGE_ZIP: &str = "cbmp-mod.zip";
const PACKAGE_SIG: &str = "cbmp-mod.zip.sig";
/// Começo do comentário confiável da assinatura do mod.
pub const MOD_COMMENT_PREFIX: &str = "cbmp-mod";
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
    /// Layout do pacote instalado (ausente = 1, arquivos direto em `cbmp\`).
    #[serde(default)]
    pub layout: u32,
    /// `atual\loader\dwmapi.dll` instalado.
    #[serde(default)]
    pub loader: Option<LoaderRef>,
    /// sha256 de carregadores anteriores (ainda reconhecidos como nossos em `Win64\`), o mais velho primeiro.
    #[serde(default)]
    pub loader_history: Vec<String>,
    /// Arquivos do jogador movidos de `Win64\` para `Win64\cbmp-backup\`.
    #[serde(default)]
    pub backups: Vec<BackupEntry>,
    /// Mod removido do jogo (Configurações); a versão continua valendo como piso do anti-downgrade.
    #[serde(default)]
    pub removed: bool,
    /// Pastas do jogo anteriores onde ainda há algo nosso (carregador, `override.txt`, `cbmp\atual`) ou backups do
    /// jogador: a troca de pasta não as abandona. Registro antigo (uma pasta só) carrega com a lista vazia.
    #[serde(default)]
    pub other_folders: Vec<FolderRecord>,
}

impl Installed {
    /// Ainda há algo nosso ou do jogador a devolver na pasta deste registro.
    pub fn has_traces(&self) -> bool {
        !self.removed || !self.backups.is_empty()
    }
}

/// Pasta do jogo anterior com algo nosso ou backups do jogador (ver [`Installed::other_folders`]).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FolderRecord {
    pub game_dir: PathBuf,
    pub version: String,
    /// Carregador que foi ativado nessa pasta (reconhecido como nosso ao remover).
    #[serde(default)]
    pub loader: Option<LoaderRef>,
    #[serde(default)]
    pub backups: Vec<BackupEntry>,
    /// O que era nosso já saiu; só restam backups que não puderam voltar (destino ocupado).
    #[serde(default)]
    pub removed: bool,
}

impl FolderRecord {
    pub fn of(r: &Installed) -> Self {
        Self {
            game_dir: r.game_dir.clone(),
            version: r.version.clone(),
            loader: r.loader.clone(),
            backups: r.backups.clone(),
            removed: r.removed,
        }
    }
}

/// Carregador instalado.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LoaderRef {
    pub sha256: String,
    pub size: u64,
}

/// Arquivo do jogador em `cbmp-backup\`: `name` em `Win64\` → `stored` em `cbmp-backup\`.
///
/// Gravado no registro **antes** de mover (`Pending`) e marcado `Done` depois: uma queda no meio deixa uma entrada que
/// a próxima conferência resolve (arquivo em `cbmp-backup\` → `Done`; ainda em `Win64\` → entrada sai). A mesma lista
/// fica no manifesto dentro da pasta do jogo (`cbmp-backup\cbmp-backup.json`, ver [`crate::modact`]).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackupEntry {
    pub name: String,
    pub stored: String,
    /// sha256 do arquivo do jogador (ausente: era uma pasta, ou registro anterior ao manifesto).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sha256: Option<String>,
    /// Ausente (registro antigo) = `Done`.
    #[serde(default)]
    pub state: BackupState,
}

/// Situação de um backup.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum BackupState {
    /// Movido para `cbmp-backup\`.
    #[default]
    Done,
    /// Registrado; o arquivo talvez ainda não tenha saído de `Win64\`.
    Pending,
}

/// `cbmp-mod.json` do pacote.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModMeta {
    pub layout: u32,
    pub version: String,
    pub protocol: u32,
    pub ue4ss: String,
    pub min_launcher: String,
}

/// Junta `hash` ao histórico de carregadores: sem repetir, no máximo [`MAX_LOADER_HISTORY`], o mais velho sai.
pub fn push_loader_history(history: &mut Vec<String>, hash: &str) {
    history.retain(|h| h != hash);
    history.push(hash.to_string());
    let excess = history.len().saturating_sub(MAX_LOADER_HISTORY);
    history.drain(..excess);
}

/// Resultado de [`ModInstaller::ensure`]: versão em uso e se algo foi (re)instalado agora.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ModState {
    pub version: String,
    pub protocol: u32,
    pub changed: bool,
}

/// `<jogo>\Phoenix\Binaries\Win64`.
pub fn win64_dir(game: &GameInstall) -> PathBuf {
    BINARIES.iter().fold(game.dir.clone(), |p, s| p.join(s))
}

/// `<jogo>\Phoenix\Binaries\Win64\cbmp` (contêiner).
pub fn container_dir(game: &GameInstall) -> PathBuf {
    win64_dir(game).join(MOD_FOLDER)
}

/// `<jogo>\Phoenix\Binaries\Win64\cbmp\atual` (pacote em uso).
pub fn mod_dir(game: &GameInstall) -> PathBuf {
    container_dir(game).join(CURRENT_FOLDER)
}

/// Sem permissão para gravar na pasta do jogo.
pub fn mod_needs_admin() -> AppError {
    AppError::new(
        "mod_needs_admin",
        "O launcher não tem permissão para instalar o mod na pasta do jogo.",
    )
}

/// `permission_denied` (de [`AppError::io`]) na pasta do jogo vira `mod_needs_admin`; o resto passa.
pub fn admin_if_denied(e: AppError) -> AppError {
    if e.code == "permission_denied" {
        log::warn!("sem permissão na pasta do jogo: {}", e.message);
        mod_needs_admin()
    } else {
        e
    }
}

fn mod_incompatible() -> AppError {
    AppError::new(
        "mod_incompatible",
        "Este mod precisa de uma versão mais nova do launcher. Atualize e tente de novo.",
    )
}

fn signature_invalid() -> AppError {
    AppError::new(
        "mod_signature_invalid",
        "A assinatura do mod não confere; nada foi instalado.",
    )
}

pub(crate) fn mod_invalid(detail: &str) -> AppError {
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

/// Confere a assinatura minisign de `data` (`sig`/`pubkey` em texto ou base64 do texto, como no Tauri) e devolve o
/// comentário confiável (coberto pela assinatura). Aceita o formato antigo sem pré-hash, como o plugin updater.
pub fn verify_minisign(data: &[u8], sig: &str, pubkey: &str) -> AppResult<String> {
    let pk = decode_pubkey(pubkey)?;
    let sig = decode_signature(sig)?;
    pk.verify(data, &sig, true).map_err(|_| {
        log::warn!("assinatura minisign recusada");
        signature_invalid()
    })?;
    Ok(sig.trusted_comment().to_string())
}

/// Como [`verify_minisign`], lendo o arquivo em blocos. Devolve o comentário confiável e o sha256 do arquivo.
pub fn verify_minisign_file(path: &Path, sig: &str, pubkey: &str) -> AppResult<(String, String)> {
    let pk = decode_pubkey(pubkey)?;
    let signature = decode_signature(sig)?;
    let io = |e: std::io::Error| AppError::io(path, &e);
    let comment = signature.trusted_comment().to_string();
    match pk.verify_stream(&signature) {
        Ok(mut v) => {
            let mut f = std::fs::File::open(path).map_err(io)?;
            let mut hasher = Sha256::new();
            let mut buf = vec![0u8; 256 * 1024];
            loop {
                let n = f.read(&mut buf).map_err(io)?;
                if n == 0 {
                    break;
                }
                v.update(&buf[..n]);
                hasher.update(&buf[..n]);
            }
            v.finalize().map_err(|_| {
                log::warn!("assinatura minisign recusada");
                signature_invalid()
            })?;
            Ok((comment, hex(&hasher.finalize())))
        }
        Err(minisign_verify::Error::UnsupportedLegacyMode) => {
            let len = std::fs::metadata(path).map_err(io)?.len();
            if len > MAX_LEGACY_SIGNED {
                return Err(signature_invalid());
            }
            let data = std::fs::read(path).map_err(io)?;
            pk.verify(&data, &signature, true)
                .map_err(|_| signature_invalid())?;
            Ok((comment, hex(&Sha256::digest(&data))))
        }
        Err(_) => {
            log::warn!("assinatura minisign de outra chave");
            Err(signature_invalid())
        }
    }
}

/// O que a assinatura do mod garante (comentário confiável).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SignedMod {
    pub protocol: u32,
    pub version: semver::Version,
    pub sha256: String,
}

/// Lê `cbmp-mod protocol=<N> version=<X.Y.Z> sha256=<hex>` (exatamente esses campos, nessa ordem).
pub fn parse_mod_comment(comment: &str) -> Option<SignedMod> {
    let mut it = comment.trim().split(' ');
    if it.next()? != MOD_COMMENT_PREFIX {
        return None;
    }
    let protocol = it.next()?.strip_prefix("protocol=")?;
    let version = it.next()?.strip_prefix("version=")?;
    let sha256 = it.next()?.strip_prefix("sha256=")?;
    if it.next().is_some()
        || protocol.is_empty()
        || !protocol.bytes().all(|b| b.is_ascii_digit())
        || sha256.len() != 64
        || !sha256
            .bytes()
            .all(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f'))
    {
        return None;
    }
    Some(SignedMod {
        protocol: protocol.parse().ok()?,
        version: semver::Version::parse(version).ok()?,
        sha256: sha256.to_string(),
    })
}

/// Confere a assinatura do zip em `path` e devolve o que ela garante. O sha256 assinado tem de ser o do arquivo.
pub fn verify_mod_file(path: &Path, sig: &str, pubkey: &str) -> AppResult<SignedMod> {
    let (comment, sha) = verify_minisign_file(path, sig, pubkey)?;
    let signed = parse_mod_comment(&comment).ok_or_else(|| {
        log::warn!("assinatura do mod sem o comentário confiável cbmp-mod");
        signature_invalid()
    })?;
    if signed.sha256 != sha {
        log::warn!("sha256 assinado não é o do zip");
        return Err(signature_invalid());
    }
    Ok(signed)
}

/// Confere o `cbmp-mod.json` contra o que a assinatura garante e a versão deste launcher.
pub fn check_mod_meta(
    bytes: &[u8],
    signed: &SignedMod,
    launcher: &semver::Version,
) -> AppResult<ModMeta> {
    let meta: ModMeta =
        serde_json::from_slice(bytes).map_err(|_| mod_invalid("cbmp-mod.json inválido"))?;
    if meta.layout != LAYOUT {
        return Err(mod_invalid("layout do pacote não suportado"));
    }
    let version = semver::Version::parse(&meta.version).ok();
    if version.as_ref() != Some(&signed.version) || meta.protocol != signed.protocol {
        log::warn!(
            "cbmp-mod.json (p{} v{}) diferente do assinado (p{} v{})",
            meta.protocol,
            meta.version,
            signed.protocol,
            signed.version
        );
        return Err(mod_invalid("cbmp-mod.json diferente do assinado"));
    }
    let min = semver::Version::parse(&meta.min_launcher).map_err(|_| mod_incompatible())?;
    if min > *launcher {
        log::warn!("mod pede o launcher {min}; este é {launcher}");
        return Err(mod_incompatible());
    }
    Ok(meta)
}

/// Valida o zip inteiro (sem gravar nada): arquivos obrigatórios do layout 2 e `cbmp-mod.json`.
fn check_package(zip: &Path, signed: &SignedMod, launcher: &semver::Version) -> AppResult<ModMeta> {
    let (mut ar, planned) = open_planned(zip)?;
    for name in REQUIRED_IN_ZIP {
        if !planned.iter().any(|p| !p.dir && p.rel == name) {
            return Err(mod_invalid(&format!("sem {name}")));
        }
    }
    let meta = planned
        .iter()
        .find(|p| !p.dir && p.rel == MOD_META_FILE)
        .ok_or_else(|| mod_invalid("sem cbmp-mod.json"))?;
    if meta.size > MAX_META_BYTES {
        return Err(mod_invalid("cbmp-mod.json grande demais"));
    }
    let entry = ar
        .by_index(meta.index)
        .map_err(|_| mod_invalid("pacote corrompido"))?;
    let mut bytes = Vec::new();
    entry
        .take(MAX_META_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| mod_invalid("pacote corrompido"))?;
    check_mod_meta(&bytes, signed, launcher)
}

/// Instala `zip` (assinatura já conferida: `signed`) em `cbmp\atual` e devolve o registro novo. `prev` é o registro
/// anterior (qualquer protocolo): dá a migração do layout 1, o histórico de carregadores e os backups.
fn install_package(
    zip: &Path,
    signed: &SignedMod,
    launcher: &semver::Version,
    game: &GameInstall,
    prev: Option<&Installed>,
) -> AppResult<Installed> {
    check_package(zip, signed, launcher)?;
    let container = container_dir(game);
    std::fs::create_dir_all(&container)
        .map_err(|e| admin_if_denied(AppError::io(&container, &e)))?;
    let same_game = prev.filter(|p| p.game_dir == game.dir);
    if let Some(old) = same_game.filter(|p| p.layout < LAYOUT) {
        migrate_legacy(&container, old).map_err(admin_if_denied)?;
    }
    let files = extract_zip(zip, &mod_dir(game)).map_err(admin_if_denied)?;
    let loader = files
        .iter()
        .find(|f| f.path == LOADER_IN_ZIP)
        .map(|f| LoaderRef {
            sha256: f.sha256.clone(),
            size: f.size,
        })
        .ok_or_else(|| mod_invalid("sem o carregador"))?;
    let mut loader_history = prev.map(|p| p.loader_history.clone()).unwrap_or_default();
    if let Some(old) = prev.and_then(|p| p.loader.as_ref()) {
        if old.sha256 != loader.sha256 {
            push_loader_history(&mut loader_history, &old.sha256);
        }
    }
    loader_history.retain(|h| *h != loader.sha256);
    let mut backups = same_game.map(|p| p.backups.clone()).unwrap_or_default();
    // Pastas anteriores com algo nosso continuam registradas; a pasta de antes entra na lista se ainda tiver algo.
    let mut other_folders = prev.map(|p| p.other_folders.clone()).unwrap_or_default();
    if let Some(p) = prev.filter(|p| p.game_dir != game.dir && p.has_traces()) {
        other_folders.retain(|f| f.game_dir != p.game_dir);
        other_folders.push(FolderRecord::of(p));
    }
    // De volta a uma pasta anterior: os backups dela voltam ao registro atual.
    if let Some(i) = other_folders.iter().position(|f| f.game_dir == game.dir) {
        let back = other_folders.remove(i);
        backups.extend(back.backups);
        if let Some(l) = back.loader.filter(|l| l.sha256 != loader.sha256) {
            push_loader_history(&mut loader_history, &l.sha256);
        }
    }
    Ok(Installed {
        version: signed.version.to_string(),
        protocol: signed.protocol,
        game_dir: game.dir.clone(),
        sha256: signed.sha256.clone(),
        files,
        layout: LAYOUT,
        loader: Some(loader),
        loader_history,
        backups,
        removed: false,
        other_folders,
    })
}

/// Layout 1 → 2: apaga de `cbmp\` só os arquivos do registro antigo e as pastas que ficarem vazias.
fn migrate_legacy(container: &Path, old: &Installed) -> AppResult<()> {
    for f in &old.files {
        if !safe_rel_path(&f.path) {
            continue;
        }
        let path = join_rel(container, &f.path);
        match std::fs::symlink_metadata(&path) {
            Ok(m) if !m.is_dir() => {
                std::fs::remove_file(&path).map_err(|e| AppError::io(&path, &e))?
            }
            _ => continue,
        }
        let mut dir = path.parent();
        while let Some(d) = dir.filter(|d| *d != container && d.starts_with(container)) {
            if std::fs::remove_dir(d).is_err() {
                break;
            }
            dir = d.parent();
        }
    }
    log::info!("mod do layout antigo ({}) removido de cbmp\\", old.version);
    Ok(())
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

fn sibling(dest: &Path, suffix: &str) -> PathBuf {
    let mut name = dest.file_name().unwrap_or_default().to_os_string();
    name.push(suffix);
    dest.with_file_name(name)
}

/// Resolve sobras de uma troca interrompida em `dest`: `.staging` sai; `.old` volta a ser `dest` se `dest` sumiu
/// (queda entre as duas renomeações), senão sai.
pub fn recover_mod_dir(dest: &Path) -> AppResult<()> {
    let staging = sibling(dest, ".staging");
    let old = sibling(dest, ".old");
    remove_entry(&staging).map_err(|e| AppError::io(&staging, &e))?;
    if std::fs::symlink_metadata(&old).is_ok() {
        if std::fs::symlink_metadata(dest).is_ok() {
            remove_entry(&old).map_err(|e| AppError::io(&old, &e))?;
        } else {
            log::warn!("troca do mod interrompida; restaurando a versão anterior");
            std::fs::rename(&old, dest).map_err(|e| AppError::io(dest, &e))?;
        }
    }
    Ok(())
}

/// Extrai os arquivos planejados em `dir` (pasta nova) e devolve cada um com sha256.
fn extract_into(
    ar: &mut Archive,
    planned: Vec<Planned>,
    dir: &Path,
) -> AppResult<Vec<InstalledFile>> {
    let mut files = Vec::new();
    for p in planned {
        let target = join_rel(dir, &p.rel);
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
    // Relê o que foi gravado: o que vai para o jogo é exatamente o que foi extraído.
    for f in &files {
        let path = join_rel(dir, &f.path);
        let (sha, len) = hash_file(&path).map_err(|e| AppError::io(&path, &e))?;
        if sha != f.sha256 || len != f.size {
            return Err(mod_invalid("arquivo gravado difere do pacote"));
        }
    }
    files.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(files)
}

/// Instala `zip` em `dest` de forma atômica: valida tudo (nada é gravado se algo for recusado), extrai em
/// `<dest>.staging`, confere os hashes e só então troca as pastas. Em qualquer falha `dest` fica como estava.
/// Devolve cada arquivo com sha256 (ordenado por caminho).
pub fn extract_zip(zip: &Path, dest: &Path) -> AppResult<Vec<InstalledFile>> {
    let (mut ar, planned) = open_planned(zip)?;
    recover_mod_dir(dest)?;
    let staging = sibling(dest, ".staging");
    let old = sibling(dest, ".old");
    std::fs::create_dir_all(&staging).map_err(|e| AppError::io(&staging, &e))?;
    let files = match extract_into(&mut ar, planned, &staging) {
        Ok(f) => f,
        Err(e) => {
            let _ = remove_entry(&staging);
            return Err(e);
        }
    };
    let had_dest = std::fs::symlink_metadata(dest).is_ok();
    if had_dest {
        if let Err(e) = std::fs::rename(dest, &old) {
            let _ = remove_entry(&staging);
            return Err(AppError::io(dest, &e));
        }
    }
    if let Err(e) = std::fs::rename(&staging, dest) {
        if had_dest {
            let _ = std::fs::rename(&old, dest);
        }
        let _ = remove_entry(&staging);
        return Err(AppError::io(dest, &e));
    }
    if had_dest {
        if let Err(e) = remove_entry(&old) {
            log::warn!("pasta antiga do mod não apagada (sai na próxima vez): {e}");
        }
    }
    Ok(files)
}

pub(crate) fn hash_file(path: &Path) -> std::io::Result<(String, u64)> {
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
#[derive(Clone)]
pub struct ModInstaller {
    mods_dir: PathBuf,
    pubkey: String,
    retry_delay: Duration,
    http: reqwest::Client,
    launcher_version: semver::Version,
}

impl ModInstaller {
    pub fn new(mods_dir: PathBuf, pubkey: String) -> Self {
        Self {
            mods_dir,
            pubkey,
            retry_delay: RETRY_DELAY,
            http: hub::download_client(),
            launcher_version: semver::Version::parse(env!("CARGO_PKG_VERSION"))
                .expect("versão do pacote é semver"),
        }
    }

    /// Versão do launcher comparada com o `minLauncher` do pacote. Padrão: a deste build.
    pub fn with_launcher_version(mut self, v: semver::Version) -> Self {
        self.launcher_version = v;
        self
    }

    /// Grava `mods\installed.json` (arquivo temporário + rename).
    pub fn save_record(&self, r: &Installed) -> AppResult<()> {
        write_record(&self.record_path(), r)
    }

    /// `mods\package\cbmp-mod.zip` (último pacote instalado, assinado).
    pub fn package_zip(&self) -> PathBuf {
        self.package_dir().join(PACKAGE_ZIP)
    }

    /// `mods\package\cbmp-mod.zip.sig`.
    pub fn package_sig(&self) -> PathBuf {
        self.package_dir().join(PACKAGE_SIG)
    }

    /// Chave pública que confere o mod.
    pub fn pubkey(&self) -> &str {
        &self.pubkey
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

    fn package_dir(&self) -> PathBuf {
        self.mods_dir.join(PACKAGE_DIR)
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
        // Pasta do jogo trocada: antes de instalar nesta, o mod sai das pastas anteriores. O que não sair (pasta
        // inacessível, falha) continua registrado e não impede de jogar aqui.
        {
            let (me, g) = (self.clone(), game.clone());
            // Manifesto de backups desta pasta antes (ela pode ter sido movida com os arquivos do jogador dentro).
            let left = blocking(move || {
                crate::modact::adopt_folder(&g.dir, &me);
                Ok(crate::modact::leave_other_folders(&g, &me))
            })
            .await?;
            for d in left {
                log::warn!("mod continua na pasta antiga do jogo {}", d.display());
            }
        }
        let dest = mod_dir(game);
        {
            let d = dest.clone();
            blocking(move || recover_mod_dir(&d).map_err(admin_if_denied)).await?;
        }
        // Removido nas Configurações: volta do pacote local, sem o Hub.
        if let Some(r) = self
            .installed()
            .filter(|r| r.removed && r.protocol == protocol && r.game_dir == game.dir)
        {
            match self.repair(&r, game).await {
                Ok(()) => {
                    return Ok(ModState {
                        version: r.version,
                        protocol,
                        changed: true,
                    })
                }
                Err(e) => log::warn!("mod removido não reinstalado do pacote local: {}", e.code),
            }
        }
        let info = hub.mod_latest(protocol).await?.ok_or_else(|| {
            AppError::new(
                "mod_unavailable",
                format!("Ainda não há mod publicado para o protocolo {protocol} deste servidor."),
            )
        })?;
        let url = validate_offer(&info, protocol, hub.base())?;
        // Versão anunciada (não assinada): só decide se vale baixar. Instalar e registrar usam a assinada.
        let offered = semver::Version::parse(&info.version).map_err(|_| bad_offer())?;

        let current = self
            .installed()
            .filter(|r| r.protocol == protocol && r.game_dir == game.dir)
            .and_then(|r| semver::Version::parse(&r.version).ok().map(|v| (r, v)));
        if let Some((r, cur)) = &current {
            let intact = {
                let (r, g) = (r.clone(), game.clone());
                tokio::task::spawn_blocking(move || files_intact(&r, &g))
                    .await
                    .unwrap_or(false)
            };
            let unchanged = ModState {
                version: r.version.clone(),
                protocol,
                changed: false,
            };
            if offered <= *cur {
                if intact {
                    if offered < *cur {
                        log::info!(
                            "Hub oferece mod {} menor que o instalado {}; mantido",
                            info.version,
                            r.version
                        );
                    }
                    return Ok(unchanged);
                }
                log::warn!("arquivos do mod alterados; reparando pelo pacote local");
                match self.repair(r, game).await {
                    Ok(()) => {
                        return Ok(ModState {
                            changed: true,
                            ..unchanged
                        })
                    }
                    Err(e) => log::warn!("reparo local do mod falhou: {}", e.code),
                }
                if offered < *cur {
                    return Err(AppError::new(
                        "mod_altered",
                        format!(
                            "Os arquivos do mod {} foram alterados, o pacote local não serve para reparar e o Hub \
                             só oferece uma versão anterior. Não fazemos downgrade: verifique os arquivos do jogo \
                             na Steam/Epic ou aguarde uma versão nova do mod.",
                            r.version
                        ),
                    ));
                }
            } else if !intact {
                log::warn!("arquivos do mod alterados; instalando a versão nova");
            }
        }
        let signed = self
            .download_and_install(
                &info,
                &url,
                protocol,
                current.as_ref().map(|(_, v)| v),
                game,
                cancel,
            )
            .await?;
        Ok(ModState {
            version: signed.version.to_string(),
            protocol,
            changed: true,
        })
    }

    /// Reinstala a versão registrada a partir de `mods\package\`, conferindo a assinatura e o `cbmp-mod.json` de
    /// novo. Limpa `removed`.
    async fn repair(&self, r: &Installed, game: &GameInstall) -> AppResult<()> {
        let (zip, sig_path) = (self.package_zip(), self.package_sig());
        let sig = std::fs::read_to_string(&sig_path).map_err(|e| AppError::io(&sig_path, &e))?;
        let (pk, record, rec) = (self.pubkey.clone(), self.record_path(), r.clone());
        let (launcher, game) = (self.launcher_version.clone(), game.clone());
        blocking(move || {
            let signed = verify_mod_file(&zip, &sig, &pk)?;
            let same = signed.protocol == rec.protocol
                && signed.sha256 == rec.sha256
                && semver::Version::parse(&rec.version).is_ok_and(|v| v == signed.version);
            if !same {
                return Err(signature_invalid());
            }
            let new = install_package(&zip, &signed, &launcher, &game, Some(&rec))?;
            write_record(&record, &new)
        })
        .await?;
        log::info!("mod {} reparado pelo pacote local", r.version);
        Ok(())
    }

    /// Baixa, confere a assinatura e o que ela garante, instala de forma atômica, guarda o pacote e grava o registro.
    async fn download_and_install(
        &self,
        info: &ModInfo,
        url: &Url,
        protocol: u32,
        installed: Option<&semver::Version>,
        game: &GameInstall,
        cancel: CancellationToken,
    ) -> AppResult<SignedMod> {
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

        let signed = {
            let (zp, sig, pk) = (
                zip_path.clone(),
                info.signature.clone(),
                self.pubkey.clone(),
            );
            blocking(move || verify_mod_file(&zp, &sig, &pk)).await?
        };
        // O assinado tem de ser exatamente o que foi pedido e anunciado.
        let announced = semver::Version::parse(&info.version).ok();
        if signed.protocol != protocol
            || signed.sha256 != info.sha256
            || announced.as_ref() != Some(&signed.version)
        {
            log::warn!(
                "assinatura do mod é de outro pacote (assinado p{} v{}, anunciado p{} v{})",
                signed.protocol,
                signed.version,
                protocol,
                info.version
            );
            return Err(signature_invalid());
        }
        // Regra de downgrade com a versão assinada.
        if installed.is_some_and(|cur| signed.version < *cur) {
            return Err(signature_invalid());
        }
        if cancel.is_cancelled() {
            return Err(cancelled());
        }

        let record_path = self.record_path();
        let pkg = self.package_dir();
        let prev = self.installed();
        let (launcher, game, signed_c) =
            (self.launcher_version.clone(), game.clone(), signed.clone());
        let sig = info.signature.clone();
        blocking(move || -> AppResult<()> {
            // Troca atômica: em qualquer falha o mod atual e o registro ficam como estavam.
            let new = install_package(&zip_path, &signed_c, &launcher, &game, prev.as_ref())?;
            // Pacote para reparo local (falha aqui só perde o reparo, não a instalação).
            if let Err(e) = keep_package(&pkg, &zip_path, &sig) {
                log::warn!("pacote do mod não guardado para reparo: {}", e.code);
            }
            write_record(&record_path, &new)
        })
        .await?;
        log::info!("mod {} (protocolo {}) instalado", signed.version, protocol);
        Ok(signed)
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

async fn blocking<T: Send + 'static>(
    f: impl FnOnce() -> AppResult<T> + Send + 'static,
) -> AppResult<T> {
    tokio::task::spawn_blocking(f)
        .await
        .map_err(|_| AppError::new("internal_error", "Falha interna ao instalar o mod."))?
}

/// Guarda o zip assinado e o `.sig` em `mods\package\` (substitui o anterior).
fn keep_package(pkg: &Path, zip: &Path, sig: &str) -> AppResult<()> {
    remove_entry(pkg).map_err(|e| AppError::io(pkg, &e))?;
    std::fs::create_dir_all(pkg).map_err(|e| AppError::io(pkg, &e))?;
    let dst = pkg.join(PACKAGE_ZIP);
    std::fs::rename(zip, &dst)
        .or_else(|_| std::fs::copy(zip, &dst).map(|_| ()))
        .map_err(|e| AppError::io(&dst, &e))?;
    let sig_path = pkg.join(PACKAGE_SIG);
    std::fs::write(&sig_path, sig.trim()).map_err(|e| AppError::io(&sig_path, &e))
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
    if !same_origin || !hub::allowed_url(&url, crate::profile::RELAXED_NETWORK) {
        return Err(bad_offer());
    }
    Ok(url)
}

fn files_intact(r: &Installed, game: &GameInstall) -> bool {
    let dir = mod_dir(game);
    r.layout == LAYOUT
        && !r.removed
        && !r.files.is_empty()
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
        let win64 = PathBuf::from("C:/HL")
            .join("Phoenix")
            .join("Binaries")
            .join("Win64");
        assert_eq!(win64_dir(&g), win64);
        assert_eq!(container_dir(&g), win64.join("cbmp"));
        assert_eq!(mod_dir(&g), win64.join("cbmp").join("atual"));
    }

    #[test]
    fn historico_de_carregadores_sem_repetir_e_no_maximo_8() {
        let mut h = Vec::new();
        push_loader_history(&mut h, "a");
        push_loader_history(&mut h, "b");
        push_loader_history(&mut h, "a");
        assert_eq!(h, ["b", "a"]);
        for i in 0..10 {
            push_loader_history(&mut h, &i.to_string());
        }
        assert_eq!(h.len(), MAX_LOADER_HISTORY);
        assert_eq!(h.first().unwrap(), "2", "o mais velho sai");
        assert_eq!(h.last().unwrap(), "9");
    }

    fn signed(version: &str, protocol: u32) -> SignedMod {
        SignedMod {
            protocol,
            version: semver::Version::parse(version).unwrap(),
            sha256: "a".repeat(64),
        }
    }

    #[test]
    fn cbmp_mod_json_conferido_contra_o_assinado() {
        let l = semver::Version::new(0, 2, 0);
        let ok = br#"{"layout":2,"version":"0.2.0","protocol":1,"ue4ss":"3.0.1+4fb9b615","minLauncher":"0.2.0"}"#;
        let m = check_mod_meta(ok, &signed("0.2.0", 1), &l).unwrap();
        assert_eq!(m.ue4ss, "3.0.1+4fb9b615");
        assert_eq!(m.min_launcher, "0.2.0");
        assert_eq!(
            check_mod_meta(ok, &signed("0.2.1", 1), &l)
                .unwrap_err()
                .code,
            "mod_invalid"
        );
        assert_eq!(
            check_mod_meta(ok, &signed("0.2.0", 2), &l)
                .unwrap_err()
                .code,
            "mod_invalid"
        );
        assert_eq!(
            check_mod_meta(ok, &signed("0.2.0", 1), &semver::Version::new(0, 1, 3))
                .unwrap_err()
                .code,
            "mod_incompatible"
        );
        // Sem campo obrigatório.
        let no_ue4ss = br#"{"layout":2,"version":"0.2.0","protocol":1,"minLauncher":"0.2.0"}"#;
        assert_eq!(
            check_mod_meta(no_ue4ss, &signed("0.2.0", 1), &l)
                .unwrap_err()
                .code,
            "mod_invalid"
        );
    }

    #[test]
    fn registro_antigo_le_com_os_campos_novos_no_padrao() {
        let old = r#"{"version":"0.1.0","protocol":1,"gameDir":"C:/HL","sha256":"x","files":[]}"#;
        let r: Installed = serde_json::from_str(old).unwrap();
        assert_eq!(r.layout, 0);
        assert!(r.loader.is_none() && r.loader_history.is_empty() && r.backups.is_empty());
        assert!(!r.removed);
    }

    #[test]
    fn permissao_negada_vira_mod_needs_admin() {
        let denied = std::io::Error::from(std::io::ErrorKind::PermissionDenied);
        let e = admin_if_denied(AppError::io(Path::new("C:/x"), &denied));
        assert_eq!(e.code, "mod_needs_admin");
        assert_eq!(
            e.message,
            "O launcher não tem permissão para instalar o mod na pasta do jogo."
        );
        let other = AppError::new("io_error", "x");
        assert_eq!(admin_if_denied(other.clone()), other);
    }
}
