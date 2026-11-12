//! Ativação do mod na pasta do jogo (`Win64` = `<jogo>\Phoenix\Binaries\Win64`), conflito com outro carregador,
//! remoção e estado.
//!
//! - **Ativo** = `Win64\dwmapi.dll` com o sha256 do carregador registrado e `Win64\override.txt` com a linha
//!   `cbmp\atual\ue4ss` (sem espaços e quebras nas pontas). `dwmapi.dll` com hash de um carregador anterior nosso
//!   (`loaderHistory`) conta como nosso e é atualizado.
//! - **Conflito**: `dwmapi.dll` ou `override.txt` que não são nossos. Só com a confirmação do jogador eles são
//!   **movidos** (rename no mesmo volume, sem substituir) para `Win64\cbmp-backup\` (nome repetido ganha `.1`, `.2`…)
//!   e registrados; nunca sobrescritos nem apagados. `xinput1_3.dll` junto de `UE4SS.dll` (UE4SS antigo por outro
//!   proxy) é conflito sem opção automática: o launcher não mexe.
//! - **Backup com registro antes** (write-ahead): a entrada é gravada como `pending` no manifesto da pasta do jogo e
//!   em `installed.json` antes de mover, e marcada `done` depois. Falha ao gravar aborta antes de mover. Entrada
//!   `pending` é resolvida na próxima conferência ([`ensure_active`], remoção, abertura do launcher): arquivo em
//!   `cbmp-backup\` → `done`; ainda em `Win64\` → a entrada sai.
//! - **Manifesto dentro da pasta do jogo** (`Win64\cbmp-backup\cbmp-backup.json`, `{v: 1, entries: [{name, stored,
//!   sha256, state}]}`): a lista de backups viaja com a pasta (Steam "mover pasta de instalação", renomear). Ao
//!   instalar, ativar, conferir o estado e remover numa pasta, o manifesto dela é **adotado** ([`adopt_folder`]): cada
//!   entrada é conferida (nome só `dwmapi.dll`/`override.txt`, guardado como `<nome>` ou `<nome>.<N>`, arquivo comum
//!   dentro de `cbmp-backup\` — que tem de ser pasta de verdade, sem link/junção — com o sha256 do manifesto) e o
//!   registro passa a conhecê-la; a mesma entrada (nome + sha256) registrada numa pasta que não existe mais sai de lá
//!   (foi movida). Pasta sumida (`Gone`) com backups que ninguém adotou fica registrada (inacessível), não é
//!   descartada. O manifesto nunca vale fora do `Win64` da própria pasta: só devolve para os nomes fixos e nunca
//!   substitui nada. É o mesmo arquivo que o modo elevado (`--mod-admin-setup`) grava.
//! - **Várias pastas do jogo**: o registro guarda, além da pasta atual, as anteriores onde ainda há algo nosso ou
//!   backups do jogador (`otherFolders`). Antes de instalar numa pasta nova o mod sai das anteriores
//!   ([`leave_other_folders`]); pasta inacessível ou falha → o registro dela fica e o Jogar segue.
//! - **Remover**: apaga só os nossos, devolve os backups (só para destino vazio), apaga `cbmp\atual*`, o ponteiro e
//!   temporários nossos; `cbmp\logs\`, `crash\`, `cb-mp.json` e `mods\package\` ficam; o registro fica com `removed`.
//!   [`remove_everywhere`] faz isso em todas as pastas registradas e devolve o resultado de cada uma.
//! - Permissão negada em `Win64` → `mod_needs_admin`; outra falha → `mod_activation_failed`.

use crate::content::{remove_entry, safe_rel_path};
use crate::error::{AppError, AppResult};
use crate::game::{GameInstall, BINARIES};
use crate::modpkg::{
    hash_file, mod_dir, mod_needs_admin, push_loader_history, win64_dir, BackupEntry, BackupState,
    FolderRecord, Installed, LoaderRef, ModInstaller, CURRENT_FOLDER, LAYOUT, MOD_FOLDER,
};
use serde::{Deserialize, Serialize};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

/// Carregador do UE4SS em `Win64\` (proxy).
pub const LOADER_NAME: &str = "dwmapi.dll";
/// Arquivo do UE4SS que aponta a pasta dele.
pub const OVERRIDE_NAME: &str = "override.txt";
/// Conteúdo do `override.txt` (relativo a `Win64\`).
pub const OVERRIDE_LINE: &str = "cbmp\\atual\\ue4ss";
/// Arquivos do jogador movidos no conflito.
pub const BACKUP_FOLDER: &str = "cbmp-backup";
/// Texto do diálogo nativo de conflito.
pub const CONFLICT_DIALOG: &str = r"Encontramos outro carregador de mods (UE4SS) na pasta do jogo. Para jogar no CB Multiplayer é preciso usar o do launcher. Seus arquivos não são apagados: ficam em Phoenix\Binaries\Win64\cbmp-backup e voltam quando você remover o mod do CB Multiplayer nas Configurações. Enquanto isso, seus outros mods do UE4SS ficam desligados.";

/// UE4SS antigo por outro proxy: os dois juntos.
const LEGACY_PROXY: &str = "xinput1_3.dll";
const LEGACY_UE4SS: &str = "UE4SS.dll";
/// `override.txt` maior que isto não é o nosso.
const MAX_OVERRIDE_BYTES: u64 = 4096;
/// Sufixos tentados em `cbmp-backup\` (`.1` … `.999`).
const MAX_BACKUP_SUFFIX: u32 = 999;
/// Arquivo temporário ao lado do destino (cópia + rename).
const TMP_SUFFIX: &str = ".cbmp-tmp";
/// Ponteiro do launcher para o mod (`cbmp\cbmp-run.json`).
const POINTER_NAME: &str = crate::launch::POINTER_FILE;
/// Manifesto dos backups dentro da pasta do jogo (`Win64\cbmp-backup\cbmp-backup.json`).
pub const BACKUP_MANIFEST: &str = "cbmp-backup.json";
const MANIFEST_VERSION: u32 = 1;
const MAX_MANIFEST_BYTES: u64 = 64 * 1024;
const MAX_MANIFEST_ENTRIES: usize = 64;
/// Únicos nomes que vão para `cbmp-backup\` e voltam para `Win64\`.
const BACKUP_NAMES: [&str; 2] = [LOADER_NAME, OVERRIDE_NAME];

/// Estado da ativação em `Win64`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Activation {
    /// Carregador e `override.txt` nossos e atuais.
    Active,
    /// Algo ausente (ou carregador nosso de hash antigo), nada estranho.
    Missing { loader: bool, override_file: bool },
    /// Arquivo que não é nosso no lugar.
    Conflict { loader: bool, override_file: bool },
    /// `xinput1_3.dll` + `UE4SS.dll`.
    LegacyUe4ss,
}

/// Estado do mod mostrado nas Configurações.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(tag = "state", rename_all = "camelCase")]
pub enum ModStatus {
    NotInstalled,
    Installed { version: String },
    Conflict,
}

/// Resultado da remoção numa pasta do jogo registrada (`error` nulo = removido).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FolderRemoval {
    pub game_dir: PathBuf,
    pub error: Option<AppError>,
}

/// O que ocupa um dos dois lugares em `Win64`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Slot {
    Missing,
    Ours,
    /// Carregador nosso de uma versão anterior.
    Outdated,
    Foreign,
}

fn mod_conflict() -> AppError {
    AppError::new(
        "mod_conflict",
        "Já existe outro carregador de mods (UE4SS) na pasta do jogo.",
    )
}

pub(crate) fn legacy_conflict() -> AppError {
    AppError::new(
        "mod_conflict",
        "Já existe outro carregador de mods (UE4SS) na pasta do jogo. Remova o UE4SS antigo \
         (xinput1_3.dll e UE4SS.dll em Phoenix\\Binaries\\Win64) e tente de novo.",
    )
}

fn activation_failed() -> AppError {
    AppError::new(
        "mod_activation_failed",
        "Não foi possível ativar o mod na pasta do jogo.",
    )
}

/// Pasta do jogo registrada que não dá para alcançar agora (disco desconectado, rede, permissão).
pub fn folder_unreachable(dir: &Path) -> AppError {
    AppError::new(
        "mod_folder_unreachable",
        format!(
            "A pasta do jogo {} não está acessível agora. O mod continua registrado lá e sai na próxima vez.",
            dir.display()
        ),
    )
}

/// Falha de disco em `Win64`: permissão negada → `mod_needs_admin`; o resto → `mod_activation_failed`.
fn act_io(path: &Path, e: &std::io::Error) -> AppError {
    if e.kind() == std::io::ErrorKind::PermissionDenied {
        log::warn!("sem permissão em {}: {e}", path.display());
        mod_needs_admin()
    } else {
        log::warn!("falha em {}: {e}", path.display());
        activation_failed()
    }
}

fn exists(p: &Path) -> bool {
    std::fs::symlink_metadata(p).is_ok()
}

/// `<dir>\Phoenix\Binaries\Win64`.
fn win64_of(dir: &Path) -> PathBuf {
    BINARIES.iter().fold(dir.to_path_buf(), |p, s| p.join(s))
}

/// Metadados de `p` (`None` se não existe).
fn meta(p: &Path) -> AppResult<Option<std::fs::Metadata>> {
    match std::fs::symlink_metadata(p) {
        Ok(m) => Ok(Some(m)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(act_io(p, &e)),
    }
}

/// Move `src` para `dst` sem nunca substituir o que estiver em `dst` (o `rename` da std substitui no Windows).
fn move_no_replace(src: &Path, dst: &Path) -> std::io::Result<()> {
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::Storage::FileSystem::MoveFileExW;
        let wide = |p: &Path| {
            p.as_os_str()
                .encode_wide()
                .chain(Some(0))
                .collect::<Vec<u16>>()
        };
        let (s, d) = (wide(src), wide(dst));
        // SAFETY: strings UTF-16 terminadas em zero que vivem até o fim da chamada. Sem flags: mesmo volume e sem
        // MOVEFILE_REPLACE_EXISTING (destino existente → ERROR_ALREADY_EXISTS).
        let ok = unsafe { MoveFileExW(s.as_ptr(), d.as_ptr(), 0) };
        if ok != 0 {
            Ok(())
        } else {
            Err(std::io::Error::last_os_error())
        }
    }
    #[cfg(not(windows))]
    {
        if exists(dst) {
            return Err(std::io::Error::from(std::io::ErrorKind::AlreadyExists));
        }
        std::fs::rename(src, dst)
    }
}

fn loader_slot(win64: &Path, loader: Option<&LoaderRef>, history: &[String]) -> AppResult<Slot> {
    let path = win64.join(LOADER_NAME);
    let Some(m) = meta(&path)? else {
        return Ok(Slot::Missing);
    };
    if !m.is_file() {
        return Ok(Slot::Foreign);
    }
    let (sha, _) = hash_file(&path).map_err(|e| act_io(&path, &e))?;
    Ok(if loader.is_some_and(|l| l.sha256 == sha) {
        Slot::Ours
    } else if history.contains(&sha) {
        Slot::Outdated
    } else {
        Slot::Foreign
    })
}

fn rec_loader_slot(win64: &Path, rec: &Installed) -> AppResult<Slot> {
    loader_slot(win64, rec.loader.as_ref(), &rec.loader_history)
}

fn override_slot(win64: &Path) -> AppResult<Slot> {
    let path = win64.join(OVERRIDE_NAME);
    let Some(m) = meta(&path)? else {
        return Ok(Slot::Missing);
    };
    if !m.is_file() || m.len() > MAX_OVERRIDE_BYTES {
        return Ok(Slot::Foreign);
    }
    let mut bytes = Vec::new();
    std::fs::File::open(&path)
        .and_then(|f| f.take(MAX_OVERRIDE_BYTES + 1).read_to_end(&mut bytes))
        .map_err(|e| act_io(&path, &e))?;
    Ok(match std::str::from_utf8(&bytes) {
        Ok(s) if s.trim() == OVERRIDE_LINE => Slot::Ours,
        _ => Slot::Foreign,
    })
}

/// Estado da ativação em `win64` para o mod registrado em `rec`.
pub fn inspect(win64: &Path, rec: &Installed) -> AppResult<Activation> {
    inspect_with(win64, rec.loader.as_ref(), &rec.loader_history)
}

/// sha256 de `Win64\dwmapi.dll`, se for um arquivo comum.
pub fn win64_loader_sha(win64: &Path) -> AppResult<Option<String>> {
    let path = win64.join(LOADER_NAME);
    match meta(&path)? {
        Some(m) if m.is_file() && !is_reparse(&m) => {
            Ok(Some(hash_file(&path).map_err(|e| act_io(&path, &e))?.0))
        }
        _ => Ok(None),
    }
}

/// [`inspect`] com o carregador esperado (`loader`) e os anteriores nossos (`history`) dados à parte.
pub fn inspect_with(
    win64: &Path,
    loader: Option<&LoaderRef>,
    history: &[String],
) -> AppResult<Activation> {
    if exists(&win64.join(LEGACY_PROXY)) && exists(&win64.join(LEGACY_UE4SS)) {
        return Ok(Activation::LegacyUe4ss);
    }
    let loader = loader_slot(win64, loader, history)?;
    let over = override_slot(win64)?;
    let (lf, of) = (loader == Slot::Foreign, over == Slot::Foreign);
    if lf || of {
        return Ok(Activation::Conflict {
            loader: lf,
            override_file: of,
        });
    }
    let (lm, om) = (loader != Slot::Ours, over != Slot::Ours);
    Ok(if lm || om {
        Activation::Missing {
            loader: lm,
            override_file: om,
        }
    } else {
        Activation::Active
    })
}

/// Resolve as entradas `pending` de `backups` (ver o topo do módulo). Devolve `true` se algo mudou.
pub fn reconcile_backups(win64: &Path, backups: &mut Vec<BackupEntry>) -> bool {
    let backup = win64.join(BACKUP_FOLDER);
    let mut changed = false;
    backups.retain_mut(|b| {
        if b.state != BackupState::Pending {
            return true;
        }
        changed = true;
        if plain_name(&b.stored) && exists(&backup.join(&b.stored)) {
            log::info!(
                "backup pendente de {} encontrado em cbmp-backup\\{}",
                b.name,
                b.stored
            );
            b.state = BackupState::Done;
            true
        } else {
            log::info!(
                "backup pendente de {} não aconteceu; entrada descartada",
                b.name
            );
            false
        }
    });
    changed
}

/// Ativa o mod de `current` (`cbmp\atual`) em `win64` **sem gravar nada além dos arquivos** (nem registro nem
/// manifesto): só para testes. Com registro durável use [`ensure_active`] (ou [`activate_with`] com a gravação), senão
/// um backup feito aqui fica sem registro.
#[doc(hidden)]
pub fn activate(
    win64: &Path,
    current: &Path,
    rec: &mut Installed,
    confirm_conflict: bool,
) -> AppResult<()> {
    activate_with(win64, current, rec, confirm_conflict, &mut |_| Ok(()))
}

/// [`activate`] gravando o registro com `persist` antes e depois de cada movimento para `cbmp-backup\`. Falha de
/// `persist` antes de mover aborta sem mexer no arquivo do jogador.
pub fn activate_with(
    win64: &Path,
    current: &Path,
    rec: &mut Installed,
    confirm_conflict: bool,
    persist: &mut dyn FnMut(&Installed) -> AppResult<()>,
) -> AppResult<()> {
    activate_from(
        win64,
        LoaderSource::Current(current),
        rec,
        confirm_conflict,
        persist,
    )
}

/// De onde vem o carregador copiado para `Win64\dwmapi.dll`.
#[derive(Debug, Clone, Copy)]
pub enum LoaderSource<'a> {
    /// `cbmp\atual` (o `loader\dwmapi.dll` dentro dele).
    Current(&'a Path),
    /// Bytes já conferidos (modo elevado: lidos do zip assinado, nada passa por `cbmp\atual`).
    Bytes(&'a [u8]),
}

/// [`activate_with`] com o carregador vindo de `src`. As mesmas regras: ausente → grava sem substituir; nosso de
/// versão anterior (`rec.loader_history`) → substitui; estranho → só com `confirm_conflict`, movido para
/// `cbmp-backup\` com `persist` antes e depois; o temporário é conferido contra `rec.loader` antes do rename.
pub fn activate_from(
    win64: &Path,
    src: LoaderSource<'_>,
    rec: &mut Installed,
    confirm_conflict: bool,
    persist: &mut dyn FnMut(&Installed) -> AppResult<()>,
) -> AppResult<()> {
    let (move_loader, move_override) = match inspect(win64, rec)? {
        Activation::Active => return Ok(()),
        Activation::LegacyUe4ss => {
            log::warn!("UE4SS antigo (xinput1_3.dll + UE4SS.dll) na pasta do jogo");
            return Err(legacy_conflict());
        }
        Activation::Conflict { .. } if !confirm_conflict => return Err(mod_conflict()),
        Activation::Conflict {
            loader,
            override_file,
        } => (loader, override_file),
        Activation::Missing { .. } => (false, false),
    };
    if move_loader || move_override {
        let backup = win64.join(BACKUP_FOLDER);
        if move_loader {
            move_to_backup(win64, &backup, LOADER_NAME, rec, persist)?;
        }
        if move_override {
            move_to_backup(win64, &backup, OVERRIDE_NAME, rec, persist)?;
        }
    }
    // Conferido de novo depois de mover: nunca sobrescreve o que não é nosso.
    match rec_loader_slot(win64, rec)? {
        Slot::Ours => {}
        Slot::Missing => install_loader(win64, src, rec, false)?,
        Slot::Outdated => install_loader(win64, src, rec, true)?,
        Slot::Foreign => return Err(mod_conflict()),
    }
    match override_slot(win64)? {
        Slot::Ours => {}
        Slot::Missing | Slot::Outdated => write_override(win64)?,
        Slot::Foreign => return Err(mod_conflict()),
    }
    log::info!("mod {} ativado na pasta do jogo", rec.version);
    Ok(())
}

/// Move `Win64\<name>` para `cbmp-backup\` (primeiro nome livre: `<name>`, `<name>.1`, …): registra `pending`, move
/// e marca `done`.
fn move_to_backup(
    win64: &Path,
    backup: &Path,
    name: &str,
    rec: &mut Installed,
    persist: &mut dyn FnMut(&Installed) -> AppResult<()>,
) -> AppResult<()> {
    let src = win64.join(name);
    // sha256 do arquivo do jogador (pasta: sem hash), para o manifesto conferir depois de uma mudança de pasta.
    let sha256 = match meta(&src)? {
        Some(m) if m.is_file() => Some(hash_file(&src).map_err(|e| act_io(&src, &e))?.0),
        _ => None,
    };
    let stored = (0..=MAX_BACKUP_SUFFIX)
        .map(|i| {
            if i == 0 {
                name.to_string()
            } else {
                format!("{name}.{i}")
            }
        })
        .find(|s| !exists(&backup.join(s)))
        .ok_or_else(|| {
            log::warn!("cbmp-backup sem nome livre para {name}");
            activation_failed()
        })?;
    rec.backups.push(BackupEntry {
        name: name.to_string(),
        stored: stored.clone(),
        sha256,
        state: BackupState::Pending,
    });
    if let Err(e) = persist(rec) {
        rec.backups.pop();
        // Desfaz o que chegou a ser gravado (ex.: registro gravado e manifesto não); se falhar, a entrada `pending`
        // sai sozinha na próxima conferência (o arquivo continua em `Win64`).
        let _ = persist(rec);
        log::warn!(
            "registro do mod não gravado; {name} não foi movido: {}",
            e.code
        );
        return Err(e);
    }
    let dst = backup.join(&stored);
    if let Err(e) = std::fs::create_dir_all(backup).and_then(|()| move_no_replace(&src, &dst)) {
        rec.backups.pop();
        // Se nem isto gravar, a entrada `pending` sai sozinha na próxima conferência (o arquivo ficou em `Win64`).
        if let Err(pe) = persist(rec) {
            log::warn!("registro do mod não regravado: {}", pe.code);
        }
        return Err(act_io(&src, &e));
    }
    if let Some(b) = rec.backups.last_mut() {
        b.state = BackupState::Done;
    }
    if let Err(e) = persist(rec) {
        // Arquivo já em `cbmp-backup\` com a entrada `pending` gravada: a próxima conferência marca `done`.
        log::warn!("backup de {name} feito; registro fica pendente: {}", e.code);
    }
    log::info!("{name} do jogador movido para cbmp-backup\\{stored}");
    Ok(())
}

fn tmp_beside(path: &Path) -> PathBuf {
    let mut s = path.as_os_str().to_owned();
    s.push(TMP_SUFFIX);
    PathBuf::from(s)
}

/// Grava o carregador de `src` em `Win64\dwmapi.dll` (temporário novo e sincronizado + rename), conferindo o hash
/// registrado. `replace` só para o carregador nosso de versão anterior; ausente → nunca substitui.
fn install_loader(
    win64: &Path,
    src: LoaderSource<'_>,
    rec: &Installed,
    replace: bool,
) -> AppResult<()> {
    let expected = rec.loader.as_ref().ok_or_else(|| {
        log::warn!("registro do mod sem carregador");
        activation_failed()
    })?;
    let dst = win64.join(LOADER_NAME);
    let tmp = tmp_beside(&dst);
    remove_entry(&tmp).map_err(|e| act_io(&tmp, &e))?;
    let copied = (|| {
        // `create_new`: um link plantado no lugar do temporário faz falhar em vez de ser seguido.
        let mut out = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&tmp)
            .map_err(|e| act_io(&tmp, &e))?;
        match src {
            LoaderSource::Current(current) => {
                let from = current.join("loader").join(LOADER_NAME);
                let mut f = std::fs::File::open(&from).map_err(|e| act_io(&from, &e))?;
                std::io::copy(&mut f, &mut out).map_err(|e| act_io(&tmp, &e))?;
            }
            LoaderSource::Bytes(b) => out.write_all(b).map_err(|e| act_io(&tmp, &e))?,
        }
        out.sync_all().map_err(|e| act_io(&tmp, &e))?;
        drop(out);
        let (sha, len) = hash_file(&tmp).map_err(|e| act_io(&tmp, &e))?;
        if sha != expected.sha256 || len != expected.size {
            log::warn!("carregador em cbmp\\atual difere do registrado");
            return Err(activation_failed());
        }
        let moved = if replace {
            std::fs::rename(&tmp, &dst)
        } else {
            move_no_replace(&tmp, &dst)
        };
        moved.map_err(|e| {
            if e.kind() == std::io::ErrorKind::AlreadyExists {
                log::warn!("dwmapi.dll apareceu durante a ativação; não substituído");
                mod_conflict()
            } else {
                act_io(&dst, &e)
            }
        })
    })();
    if copied.is_err() {
        let _ = remove_entry(&tmp);
    }
    copied
}

/// Grava `Win64\override.txt` com a linha + `\r\n` (temporário novo + rename sem substituir).
fn write_override(win64: &Path) -> AppResult<()> {
    let dst = win64.join(OVERRIDE_NAME);
    let tmp = tmp_beside(&dst);
    remove_entry(&tmp).map_err(|e| act_io(&tmp, &e))?;
    let written = (|| {
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&tmp)?;
        f.write_all(format!("{OVERRIDE_LINE}\r\n").as_bytes())?;
        f.sync_all()?;
        drop(f);
        move_no_replace(&tmp, &dst)
    })();
    written.map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        if e.kind() == std::io::ErrorKind::AlreadyExists {
            log::warn!("override.txt apareceu durante a ativação; não substituído");
            mod_conflict()
        } else {
            act_io(&dst, &e)
        }
    })
}

/// Registro do mod instalado (layout 2, não removido) nesta pasta do jogo.
fn active_record(game: &GameInstall, mods: &ModInstaller) -> Option<Installed> {
    mods.installed()
        .filter(|r| r.game_dir == game.dir && !r.removed && r.layout == LAYOUT)
}

/// Confere a ativação e ativa o que faltar (em todo Jogar, depois do `ensure`); grava o registro antes e depois de
/// cada movimento e no fim, se mudou.
pub fn ensure_active(
    game: &GameInstall,
    mods: &ModInstaller,
    confirm_conflict: bool,
) -> AppResult<()> {
    let mut rec = active_record(game, mods).ok_or_else(|| {
        log::warn!("ativação sem mod instalado nesta pasta do jogo");
        activation_failed()
    })?;
    let win64 = win64_dir(game);
    let reconciled = reconcile_backups(&win64, &mut rec.backups);
    if adopt_into(&mut rec, &game.dir) || reconciled {
        mods.save_record(&rec)?;
    }
    if inspect(&win64, &rec)? == Activation::Active {
        return Ok(());
    }
    let before = rec.clone();
    // Registro e manifesto da pasta, nessa ordem (registro que não grava não toca na pasta do jogo), antes e depois
    // de cada movimento.
    let result = activate_with(
        &win64,
        &mod_dir(game),
        &mut rec,
        confirm_conflict,
        &mut |r| {
            mods.save_record(r)?;
            write_manifest(&win64, &r.backups)
        },
    );
    if rec != before {
        mods.save_record(&rec)?;
    }
    result
}

/// Nome de um arquivo só (sem pasta) em `Win64` ou `cbmp-backup\`.
fn plain_name(s: &str) -> bool {
    !s.contains('/') && safe_rel_path(s)
}

/// Devolve cada backup para `Win64\` se o destino estiver vazio; o que não volta fica em `backups`.
fn restore_backups(win64: &Path, backups: &mut Vec<BackupEntry>) -> AppResult<()> {
    let backup = win64.join(BACKUP_FOLDER);
    let mut pending = std::mem::take(backups).into_iter();
    while let Some(b) = pending.next() {
        if !backup_names_ok(&b.name, &b.stored) {
            log::warn!("backup com nome inválido no registro; ignorado");
            continue;
        }
        let (src, dst) = (backup.join(&b.stored), win64.join(&b.name));
        if !exists(&src) {
            log::warn!("cbmp-backup\\{} não existe mais", b.stored);
            continue;
        }
        match move_no_replace(&src, &dst) {
            Ok(()) => log::info!("cbmp-backup\\{} devolvido como {}", b.stored, b.name),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
                log::info!("{} ocupado; cbmp-backup\\{} fica", b.name, b.stored);
                backups.push(b);
            }
            Err(e) => {
                backups.push(b);
                backups.extend(pending);
                return Err(act_io(&dst, &e));
            }
        }
    }
    // O manifesto fica com o que não voltou (vazio: sai); a pasta vazia sai, com algo dentro fica.
    if backup_dir_ok(win64) {
        if let Err(e) = write_manifest(win64, backups) {
            log::warn!("manifesto de cbmp-backup não atualizado: {}", e.code);
        }
        let _ = std::fs::remove_dir(&backup);
    }
    Ok(())
}

/// Situação de uma pasta do jogo registrada.
enum Reach {
    Present,
    /// `Win64` não existe mais num volume que existe (jogo desinstalado/apagado): nada a devolver.
    Gone,
    /// Volume ausente ou sem acesso: o registro fica.
    Unreachable,
}

fn reach(dir: &Path) -> Reach {
    let win64 = win64_of(dir);
    match std::fs::symlink_metadata(&win64) {
        Ok(m) if m.is_dir() => Reach::Present,
        Ok(_) => Reach::Gone,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            let root = dir.ancestors().last().unwrap_or(dir);
            if dir.is_absolute() && root.is_dir() {
                Reach::Gone
            } else {
                Reach::Unreachable
            }
        }
        Err(_) => Reach::Unreachable,
    }
}

/// Tira da pasta do jogo `dir` tudo o que é nosso e devolve os backups. `backups` fica com o que não voltou.
fn remove_in(
    dir: &Path,
    loader: Option<&LoaderRef>,
    history: &[String],
    backups: &mut Vec<BackupEntry>,
) -> AppResult<()> {
    match reach(dir) {
        Reach::Present => {}
        // Sem backups do jogador: nada a devolver, o registro dela sai. Com backups: os arquivos podem ter ido junto
        // com a pasta (movida/renomeada) e voltam quando o manifesto for adotado onde ela estiver; até lá fica.
        Reach::Gone if backups.is_empty() => {
            log::info!(
                "pasta do jogo {} não existe mais; registro dela descartado",
                dir.display()
            );
            return Ok(());
        }
        Reach::Gone => {
            log::warn!(
                "pasta do jogo {} não existe mais e tinha backups do jogador; registro mantido",
                dir.display()
            );
            return Err(folder_unreachable(dir));
        }
        Reach::Unreachable => return Err(folder_unreachable(dir)),
    }
    let win64 = win64_of(dir);
    reconcile_backups(&win64, backups);
    if matches!(
        loader_slot(&win64, loader, history)?,
        Slot::Ours | Slot::Outdated
    ) {
        let p = win64.join(LOADER_NAME);
        std::fs::remove_file(&p).map_err(|e| act_io(&p, &e))?;
    }
    if override_slot(&win64)? == Slot::Ours {
        let p = win64.join(OVERRIDE_NAME);
        std::fs::remove_file(&p).map_err(|e| act_io(&p, &e))?;
    }
    for name in [LOADER_NAME, OVERRIDE_NAME] {
        let p = tmp_beside(&win64.join(name));
        remove_entry(&p).map_err(|e| act_io(&p, &e))?;
    }
    restore_backups(&win64, backups)?;
    let container = win64.join(MOD_FOLDER);
    let staging = format!("{CURRENT_FOLDER}.staging");
    let old = format!("{CURRENT_FOLDER}.old");
    let pointer_tmp = format!("{POINTER_NAME}.tmp");
    for name in [CURRENT_FOLDER, &staging, &old, POINTER_NAME, &pointer_tmp] {
        let p = container.join(name);
        remove_entry(&p).map_err(|e| act_io(&p, &e))?;
    }
    Ok(())
}

/// Remove o mod da pasta do jogo `dir` (a do registro atual, uma anterior registrada, ou uma presente que o registro
/// não conhece: só o que é nosso pelo hash/linha sai) e grava o registro. O manifesto da pasta é adotado antes.
fn remove_folder(dir: &Path, mods: &ModInstaller) -> AppResult<()> {
    let Some(mut rec) = mods.installed() else {
        log::info!("remover mod: nada registrado");
        return Ok(());
    };
    let before = rec.clone();
    let present = matches!(reach(dir), Reach::Present);
    if present {
        adopt_into(&mut rec, dir);
    }
    // Carregadores nossos: o atual e os anteriores (uma pasta adotada não sabe qual foi ativado nela).
    let mut ours = rec.loader_history.clone();
    if let Some(l) = &rec.loader {
        ours.push(l.sha256.clone());
    }
    let result = if rec.game_dir == dir {
        let (loader, history) = (rec.loader.clone(), rec.loader_history.clone());
        let r = remove_in(dir, loader.as_ref(), &history, &mut rec.backups);
        if r.is_ok() {
            rec.removed = true;
            rec.files.clear();
            if let Some(l) = rec.loader.take() {
                push_loader_history(&mut rec.loader_history, &l.sha256);
            }
            log::info!("mod {} removido do jogo", rec.version);
        }
        r
    } else if let Some(i) = rec.other_folders.iter().position(|f| f.game_dir == dir) {
        let mut f = rec.other_folders[i].clone();
        let r = remove_in(dir, f.loader.as_ref(), &ours, &mut f.backups);
        if r.is_ok() {
            f.removed = true;
            log::info!("mod removido da pasta anterior do jogo {}", dir.display());
        }
        if f.removed && f.backups.is_empty() {
            rec.other_folders.remove(i);
        } else {
            rec.other_folders[i] = f;
        }
        r
    } else if present {
        // Pasta presente sem registro nem manifesto (ex.: movida sem conflito): tira o que é nosso.
        let mut none = Vec::new();
        remove_in(dir, None, &ours, &mut none)
    } else {
        log::info!("remover mod: nada registrado nesta pasta do jogo");
        Ok(())
    };
    if rec != before {
        mods.save_record(&rec)?;
    }
    result
}

/// Remove o mod de `game` (a pasta do registro atual ou uma anterior registrada). Não confere se o jogo está aberto
/// (é do comando).
pub fn remove_mod(game: &GameInstall, mods: &ModInstaller) -> AppResult<()> {
    remove_folder(&game.dir, mods)
}

/// Pastas registradas com algo nosso ou backups do jogador.
fn registered_dirs(rec: &Installed) -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    if rec.has_traces() {
        dirs.push(rec.game_dir.clone());
    }
    dirs.extend(rec.other_folders.iter().map(|f| f.game_dir.clone()));
    dirs
}

/// "Remover mod do jogo": remove de todas as pastas registradas e da pasta do jogo atual (`current`); resultado por
/// pasta. Antes, adota o manifesto de cada pasta presente: os backups de uma pasta movida são achados antes de a pasta
/// antiga (sumida) ser julgada.
pub fn remove_everywhere(mods: &ModInstaller, current: Option<&Path>) -> Vec<FolderRemoval> {
    let Some(rec) = mods.installed() else {
        return Vec::new();
    };
    let mut seen = registered_dirs(&rec);
    if let Some(c) = current {
        if !seen.iter().any(|d| d == c) {
            seen.push(c.to_path_buf());
        }
    }
    for d in &seen {
        adopt_folder(d, mods);
    }
    let Some(rec) = mods.installed() else {
        return Vec::new();
    };
    let mut dirs = registered_dirs(&rec);
    if let Some(c) = current {
        if !dirs.iter().any(|d| d == c) && matches!(reach(c), Reach::Present) {
            dirs.push(c.to_path_buf());
        }
    }
    dirs.into_iter()
        .map(|d| {
            let error = remove_folder(&d, mods).err();
            FolderRemoval { game_dir: d, error }
        })
        .collect()
}

/// Pastas registradas, fora `except`, onde ainda há algo nosso ou backups do jogador.
pub fn leftover_folders(except: Option<&Path>, mods: &ModInstaller) -> Vec<PathBuf> {
    let Some(rec) = mods.installed() else {
        return Vec::new();
    };
    let mut out = Vec::new();
    if rec.has_traces() && Some(rec.game_dir.as_path()) != except {
        out.push(rec.game_dir.clone());
    }
    out.extend(
        rec.other_folders
            .iter()
            .filter(|f| Some(f.game_dir.as_path()) != except)
            .map(|f| f.game_dir.clone()),
    );
    out
}

/// Antes de instalar em `game`: tenta tirar o mod de todas as outras pastas registradas. Devolve as que ainda têm
/// algo (inacessíveis ou com falha); o registro delas fica.
pub fn leave_other_folders(game: &GameInstall, mods: &ModInstaller) -> Vec<PathBuf> {
    for d in leftover_folders(Some(&game.dir), mods) {
        match remove_folder(&d, mods) {
            Ok(()) => log::info!("pasta do jogo trocada: mod removido de {}", d.display()),
            Err(e) => log::warn!(
                "pasta do jogo trocada: mod não removido de {}: {}",
                d.display(),
                e.code
            ),
        }
    }
    leftover_folders(Some(&game.dir), mods)
}

/// Na abertura do launcher: resolve backups `pending` e adota os manifestos de todas as pastas registradas que
/// estiverem acessíveis.
pub fn reconcile_all(mods: &ModInstaller) {
    let Some(mut rec) = mods.installed() else {
        return;
    };
    let before = rec.clone();
    if matches!(reach(&rec.game_dir), Reach::Present) {
        reconcile_backups(&win64_of(&rec.game_dir), &mut rec.backups);
    }
    for f in &mut rec.other_folders {
        if matches!(reach(&f.game_dir), Reach::Present) {
            reconcile_backups(&win64_of(&f.game_dir), &mut f.backups);
        }
    }
    for d in registered_dirs(&rec) {
        if matches!(reach(&d), Reach::Present) {
            adopt_into(&mut rec, &d);
        }
    }
    if rec != before {
        if let Err(e) = mods.save_record(&rec) {
            log::warn!("registro do mod não regravado na abertura: {}", e.code);
        }
    }
}

// ----- manifesto dos backups dentro da pasta do jogo -----

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Manifest {
    v: u32,
    entries: Vec<BackupEntry>,
}

#[derive(Serialize)]
struct ManifestOut<'a> {
    v: u32,
    entries: &'a [BackupEntry],
}

/// Ponto de reanálise (link simbólico, junção, arquivo de nuvem…): nunca seguido nem aceito.
pub(crate) fn is_reparse(m: &std::fs::Metadata) -> bool {
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
        m.file_type().is_symlink() || m.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0
    }
    #[cfg(not(windows))]
    {
        m.file_type().is_symlink()
    }
}

/// `stored` vale para `name`: `name` da lista fixa e `stored` = `<name>` ou `<name>.<1..999>`.
fn backup_names_ok(name: &str, stored: &str) -> bool {
    if !BACKUP_NAMES.contains(&name) {
        return false;
    }
    if stored == name {
        return true;
    }
    stored
        .strip_prefix(name)
        .and_then(|s| s.strip_prefix('.'))
        .is_some_and(|n| {
            !n.is_empty()
                && n.len() <= 3
                && !n.starts_with('0')
                && n.bytes().all(|b| b.is_ascii_digit())
        })
}

fn valid_sha(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f'))
}

/// `Win64\cbmp-backup` é pasta de verdade (não link/junção).
fn backup_dir_ok(win64: &Path) -> bool {
    std::fs::symlink_metadata(win64.join(BACKUP_FOLDER))
        .is_ok_and(|m| m.is_dir() && !is_reparse(&m))
}

/// `cbmp-backup\<stored>` confere com a entrada: arquivo comum com o sha256 dela (ou pasta, quando sem sha256).
fn backup_matches(backup: &Path, e: &BackupEntry) -> bool {
    let p = backup.join(&e.stored);
    let Ok(m) = std::fs::symlink_metadata(&p) else {
        return false;
    };
    if is_reparse(&m) {
        return false;
    }
    match &e.sha256 {
        Some(sha) => m.is_file() && hash_file(&p).is_ok_and(|(h, _)| h == *sha),
        None => m.is_dir(),
    }
}

/// Entradas do manifesto de `win64` que conferem com o que está em `cbmp-backup\` (todas como `done`). Manifesto
/// ausente, ilegível, link, grande demais, de outra versão ou com campos desconhecidos → nenhuma; entrada com nome
/// fora da lista fixa, caminho, sha256 inválido ou arquivo que não confere → ignorada.
pub fn read_manifest(win64: &Path) -> Vec<BackupEntry> {
    if !backup_dir_ok(win64) {
        return Vec::new();
    }
    let backup = win64.join(BACKUP_FOLDER);
    let path = backup.join(BACKUP_MANIFEST);
    let ok = std::fs::symlink_metadata(&path)
        .is_ok_and(|m| m.is_file() && !is_reparse(&m) && m.len() <= MAX_MANIFEST_BYTES);
    if !ok {
        return Vec::new();
    }
    let mut bytes = Vec::new();
    if std::fs::File::open(&path)
        .and_then(|f| f.take(MAX_MANIFEST_BYTES + 1).read_to_end(&mut bytes))
        .is_err()
    {
        return Vec::new();
    }
    let m = match serde_json::from_slice::<Manifest>(&bytes) {
        Ok(m) if m.v == MANIFEST_VERSION && m.entries.len() <= MAX_MANIFEST_ENTRIES => m,
        _ => {
            log::warn!(
                "manifesto de cbmp-backup inválido em {}; ignorado",
                win64.display()
            );
            return Vec::new();
        }
    };
    let mut out: Vec<BackupEntry> = Vec::new();
    for mut e in m.entries {
        let sha_ok = e.sha256.as_deref().is_none_or(valid_sha);
        if !backup_names_ok(&e.name, &e.stored)
            || !sha_ok
            || out.iter().any(|o| o.stored == e.stored)
        {
            log::warn!("entrada inválida no manifesto de cbmp-backup; ignorada");
            continue;
        }
        if !backup_matches(&backup, &e) {
            log::info!(
                "cbmp-backup\\{} não confere com o manifesto; ignorado",
                e.stored
            );
            continue;
        }
        e.state = BackupState::Done;
        out.push(e);
    }
    out
}

/// Grava o manifesto de `win64` com `entries` (temporário novo + rename); lista vazia apaga o manifesto. Entrada
/// `done` sem sha256 (registro antigo) ganha o do arquivo em `cbmp-backup\`, se for arquivo comum.
pub fn write_manifest(win64: &Path, entries: &[BackupEntry]) -> AppResult<()> {
    let backup = win64.join(BACKUP_FOLDER);
    let path = backup.join(BACKUP_MANIFEST);
    if entries.is_empty() {
        return match meta(&path)? {
            None => Ok(()),
            Some(_) => remove_entry(&path).map_err(|e| act_io(&path, &e)),
        };
    }
    match meta(&backup)? {
        Some(m) if m.is_dir() && !is_reparse(&m) => {}
        Some(_) => {
            log::warn!("cbmp-backup não é uma pasta de verdade; manifesto não gravado");
            return Err(activation_failed());
        }
        None => std::fs::create_dir(&backup).map_err(|e| act_io(&backup, &e))?,
    }
    let filled: Vec<BackupEntry> = entries
        .iter()
        .map(|e| {
            let mut e = e.clone();
            if e.sha256.is_none() && e.state == BackupState::Done {
                let p = backup.join(&e.stored);
                let file =
                    std::fs::symlink_metadata(&p).is_ok_and(|m| m.is_file() && !is_reparse(&m));
                if file && plain_name(&e.stored) {
                    e.sha256 = hash_file(&p).ok().map(|(h, _)| h);
                }
            }
            e
        })
        .collect();
    let json = serde_json::to_vec_pretty(&ManifestOut {
        v: MANIFEST_VERSION,
        entries: &filled,
    })
    .expect("manifesto sempre serializa");
    let tmp = tmp_beside(&path);
    remove_entry(&tmp).map_err(|e| act_io(&tmp, &e))?;
    let written = (|| {
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&tmp)?;
        f.write_all(&json)?;
        f.sync_all()?;
        drop(f);
        std::fs::rename(&tmp, &path)
    })();
    written.map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        act_io(&path, &e)
    })
}

/// Lista de backups da pasta `dir` no registro: a do registro atual, a de uma pasta anterior ou uma pasta nova em
/// `other_folders` (criada se `create`).
fn folder_backups<'a>(
    rec: &'a mut Installed,
    dir: &Path,
    create: bool,
) -> Option<&'a mut Vec<BackupEntry>> {
    if rec.game_dir == dir {
        return Some(&mut rec.backups);
    }
    let i = match rec.other_folders.iter().position(|f| f.game_dir == dir) {
        Some(i) => i,
        None if create => {
            rec.other_folders.push(FolderRecord {
                game_dir: dir.to_path_buf(),
                version: rec.version.clone(),
                loader: None,
                backups: Vec::new(),
                removed: false,
            });
            rec.other_folders.len() - 1
        }
        None => return None,
    };
    Some(&mut rec.other_folders[i].backups)
}

/// Adota o manifesto da pasta presente `dir` em `rec` (ver o topo do módulo). Devolve `true` se `rec` mudou.
fn adopt_into(rec: &mut Installed, dir: &Path) -> bool {
    let win64 = win64_of(dir);
    let found = read_manifest(&win64);
    let mut changed = false;
    if !found.is_empty() {
        let list = folder_backups(rec, dir, true).expect("criada");
        for e in &found {
            if !list.iter().any(|b| b.stored == e.stored) {
                log::info!(
                    "backup cbmp-backup\\{} adotado do manifesto de {}",
                    e.stored,
                    dir.display()
                );
                list.push(e.clone());
                changed = true;
            }
        }
        changed |= forget_moved(rec, dir, &found);
    }
    // Registro com entradas que o manifesto não tem (registro antigo, ou manifesto apagado): grava o manifesto. Falha
    // (pasta protegida) só loga.
    if let Some(list) = folder_backups(rec, dir, false) {
        let missing = list
            .iter()
            .any(|b| b.state == BackupState::Done && !found.iter().any(|f| f.stored == b.stored));
        if missing && backup_dir_ok(&win64) {
            if let Err(e) = write_manifest(&win64, list) {
                log::warn!("manifesto de cbmp-backup não gravado: {}", e.code);
            }
        }
    }
    changed
}

/// A mesma entrada (nome + sha256) adotada em `dir` sai das pastas registradas que não estão presentes (a pasta foi
/// movida/renomeada); pasta anterior sumida que fica sem backups sai do registro.
fn forget_moved(rec: &mut Installed, dir: &Path, found: &[BackupEntry]) -> bool {
    let same = |b: &BackupEntry| {
        b.sha256.is_some()
            && found
                .iter()
                .any(|f| f.name == b.name && f.sha256 == b.sha256)
    };
    let mut changed = false;
    if rec.game_dir != dir
        && rec.backups.iter().any(same)
        && !matches!(reach(&rec.game_dir), Reach::Present)
    {
        rec.backups.retain(|b| !same(b));
        log::info!(
            "backups de {} adotados em {}",
            rec.game_dir.display(),
            dir.display()
        );
        changed = true;
    }
    let mut emptied = Vec::new();
    for f in rec.other_folders.iter_mut().filter(|f| f.game_dir != dir) {
        if f.backups.iter().any(same) && !matches!(reach(&f.game_dir), Reach::Present) {
            f.backups.retain(|b| !same(b));
            log::info!(
                "backups de {} adotados em {}",
                f.game_dir.display(),
                dir.display()
            );
            changed = true;
            if f.backups.is_empty() {
                emptied.push(f.game_dir.clone());
            }
        }
    }
    rec.other_folders
        .retain(|f| !(emptied.contains(&f.game_dir) && matches!(reach(&f.game_dir), Reach::Gone)));
    changed
}

/// Adota o manifesto da pasta do jogo `dir` (se presente) no registro e o grava se mudou. Falhas só logam.
pub fn adopt_folder(dir: &Path, mods: &ModInstaller) {
    let Some(mut rec) = mods.installed() else {
        return;
    };
    if !matches!(reach(dir), Reach::Present) {
        return;
    }
    if adopt_into(&mut rec, dir) {
        if let Err(e) = mods.save_record(&rec) {
            log::warn!(
                "registro do mod não regravado ao adotar backups: {}",
                e.code
            );
        }
    }
}

/// Estado do mod para as Configurações (só o layout atual conta como instalado).
pub fn status(game: Option<&GameInstall>, mods: &ModInstaller) -> ModStatus {
    let Some(game) = game else {
        return ModStatus::NotInstalled;
    };
    let Some(rec) = active_record(game, mods) else {
        return ModStatus::NotInstalled;
    };
    match inspect(&win64_dir(game), &rec) {
        Ok(Activation::Conflict { .. } | Activation::LegacyUe4ss) => ModStatus::Conflict,
        Ok(_) => ModStatus::Installed {
            version: rec.version,
        },
        Err(e) => {
            log::warn!("estado do mod: pasta do jogo não conferida ({})", e.code);
            ModStatus::Installed {
                version: rec.version,
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn estado_serializa_como_a_ui_espera() {
        let v = |s: ModStatus| serde_json::to_value(s).unwrap();
        assert_eq!(
            v(ModStatus::NotInstalled),
            serde_json::json!({ "state": "notInstalled" })
        );
        assert_eq!(
            v(ModStatus::Installed {
                version: "0.2.0".into()
            }),
            serde_json::json!({ "state": "installed", "version": "0.2.0" })
        );
        assert_eq!(
            v(ModStatus::Conflict),
            serde_json::json!({ "state": "conflict" })
        );
        let r = FolderRemoval {
            game_dir: PathBuf::from("D:\\HL"),
            error: None,
        };
        assert_eq!(
            serde_json::to_value(r).unwrap(),
            serde_json::json!({ "gameDir": "D:\\HL", "error": null })
        );
    }

    #[test]
    fn mensagens_exatas() {
        assert_eq!(
            mod_conflict().message,
            "Já existe outro carregador de mods (UE4SS) na pasta do jogo."
        );
        assert!(legacy_conflict()
            .message
            .starts_with(&mod_conflict().message));
        assert!(legacy_conflict()
            .message
            .contains("xinput1_3.dll e UE4SS.dll"));
        assert_eq!(
            activation_failed().message,
            "Não foi possível ativar o mod na pasta do jogo."
        );
        assert_eq!(OVERRIDE_LINE, r"cbmp\atual\ue4ss");
        assert!(CONFLICT_DIALOG.contains(r"Phoenix\Binaries\Win64\cbmp-backup"));
        let denied = std::io::Error::from(std::io::ErrorKind::PermissionDenied);
        assert_eq!(act_io(Path::new("x"), &denied).code, "mod_needs_admin");
        let other = std::io::Error::other("x");
        assert_eq!(act_io(Path::new("x"), &other).code, "mod_activation_failed");
    }

    #[test]
    fn nome_de_backup_e_um_arquivo_so() {
        assert!(plain_name("dwmapi.dll.1"));
        for bad in ["", "..", "a/b", "a\\b", "C:x"] {
            assert!(!plain_name(bad), "{bad}");
        }
    }

    #[test]
    fn mover_sem_substituir_recusa_destino_existente() {
        let tmp = tempfile::tempdir().unwrap();
        let (a, b) = (tmp.path().join("a"), tmp.path().join("b"));
        std::fs::write(&a, b"novo").unwrap();
        std::fs::write(&b, b"do jogador").unwrap();
        let e = move_no_replace(&a, &b).unwrap_err();
        assert_eq!(e.kind(), std::io::ErrorKind::AlreadyExists);
        assert_eq!(std::fs::read(&b).unwrap(), b"do jogador");
        std::fs::remove_file(&b).unwrap();
        move_no_replace(&a, &b).unwrap();
        assert_eq!(std::fs::read(&b).unwrap(), b"novo");
    }
}
