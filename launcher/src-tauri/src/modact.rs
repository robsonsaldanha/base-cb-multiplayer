//! Ativação do mod na pasta do jogo (`Win64` = `<jogo>\Phoenix\Binaries\Win64`), conflito com outro carregador,
//! remoção e estado.
//!
//! - **Ativo** = `Win64\dwmapi.dll` com o sha256 do carregador registrado e `Win64\override.txt` com a linha
//!   `cbmp\atual\ue4ss` (sem espaços e quebras nas pontas). `dwmapi.dll` com hash de um carregador anterior nosso
//!   (`loaderHistory`) conta como nosso e é atualizado.
//! - **Conflito**: `dwmapi.dll` ou `override.txt` que não são nossos. Só com a confirmação do jogador eles são
//!   **movidos** (rename no mesmo volume) para `Win64\cbmp-backup\` (nome repetido ganha `.1`, `.2`…) e registrados;
//!   nunca sobrescritos nem apagados. `xinput1_3.dll` junto de `UE4SS.dll` (UE4SS antigo por outro proxy) é conflito
//!   sem opção automática: o launcher não mexe.
//! - **Remover**: apaga só os nossos, devolve os backups (só para destino vazio), apaga `cbmp\atual*` e o ponteiro;
//!   `cbmp\logs\`, `crash\`, `cb-mp.json` e `mods\package\` ficam; o registro fica com `removed`.
//! - Permissão negada em `Win64` → `mod_needs_admin`; outra falha → `mod_activation_failed`.

use crate::content::{remove_entry, safe_rel_path};
use crate::error::{AppError, AppResult};
use crate::game::GameInstall;
use crate::modpkg::{
    container_dir, hash_file, mod_dir, mod_needs_admin, push_loader_history, win64_dir,
    BackupEntry, Installed, ModInstaller, CURRENT_FOLDER, LAYOUT,
};
use serde::Serialize;
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
const POINTER_NAME: &str = "cbmp-run.json";

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

fn legacy_conflict() -> AppError {
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

/// Metadados de `p` (`None` se não existe).
fn meta(p: &Path) -> AppResult<Option<std::fs::Metadata>> {
    match std::fs::symlink_metadata(p) {
        Ok(m) => Ok(Some(m)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(act_io(p, &e)),
    }
}

fn loader_slot(win64: &Path, rec: &Installed) -> AppResult<Slot> {
    let path = win64.join(LOADER_NAME);
    let Some(m) = meta(&path)? else {
        return Ok(Slot::Missing);
    };
    if !m.is_file() {
        return Ok(Slot::Foreign);
    }
    let (sha, _) = hash_file(&path).map_err(|e| act_io(&path, &e))?;
    Ok(if rec.loader.as_ref().is_some_and(|l| l.sha256 == sha) {
        Slot::Ours
    } else if rec.loader_history.contains(&sha) {
        Slot::Outdated
    } else {
        Slot::Foreign
    })
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
    if exists(&win64.join(LEGACY_PROXY)) && exists(&win64.join(LEGACY_UE4SS)) {
        return Ok(Activation::LegacyUe4ss);
    }
    let loader = loader_slot(win64, rec)?;
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

/// Ativa o mod de `current` (`cbmp\atual`) em `win64`. Conflito só é resolvido com `confirm_conflict` (arquivos
/// estranhos movidos para `cbmp-backup\` e registrados em `rec.backups`; quem chama grava o registro).
pub fn activate(
    win64: &Path,
    current: &Path,
    rec: &mut Installed,
    confirm_conflict: bool,
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
        std::fs::create_dir_all(&backup).map_err(|e| act_io(&backup, &e))?;
        if move_loader {
            move_to_backup(win64, &backup, LOADER_NAME, rec)?;
        }
        if move_override {
            move_to_backup(win64, &backup, OVERRIDE_NAME, rec)?;
        }
    }
    // Conferido de novo depois de mover: nunca sobrescreve o que não é nosso.
    match loader_slot(win64, rec)? {
        Slot::Ours => {}
        Slot::Missing | Slot::Outdated => install_loader(win64, current, rec)?,
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

/// Move `Win64\<name>` para `cbmp-backup\` (primeiro nome livre: `<name>`, `<name>.1`, …) e registra.
fn move_to_backup(win64: &Path, backup: &Path, name: &str, rec: &mut Installed) -> AppResult<()> {
    let src = win64.join(name);
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
    let dst = backup.join(&stored);
    std::fs::rename(&src, &dst).map_err(|e| act_io(&src, &e))?;
    log::info!("{name} do jogador movido para cbmp-backup\\{stored}");
    rec.backups.push(BackupEntry {
        name: name.to_string(),
        stored,
    });
    Ok(())
}

fn tmp_beside(path: &Path) -> PathBuf {
    let mut s = path.as_os_str().to_owned();
    s.push(TMP_SUFFIX);
    PathBuf::from(s)
}

/// Copia `atual\loader\dwmapi.dll` para `Win64\dwmapi.dll` (temporário + rename), conferindo o hash registrado.
fn install_loader(win64: &Path, current: &Path, rec: &Installed) -> AppResult<()> {
    let expected = rec.loader.as_ref().ok_or_else(|| {
        log::warn!("registro do mod sem carregador");
        activation_failed()
    })?;
    let src = current.join("loader").join(LOADER_NAME);
    let dst = win64.join(LOADER_NAME);
    let tmp = tmp_beside(&dst);
    remove_entry(&tmp).map_err(|e| act_io(&tmp, &e))?;
    let copied = (|| {
        std::fs::copy(&src, &tmp).map_err(|e| act_io(&tmp, &e))?;
        let (sha, len) = hash_file(&tmp).map_err(|e| act_io(&tmp, &e))?;
        if sha != expected.sha256 || len != expected.size {
            log::warn!("carregador em cbmp\\atual difere do registrado");
            return Err(activation_failed());
        }
        std::fs::rename(&tmp, &dst).map_err(|e| act_io(&dst, &e))
    })();
    if copied.is_err() {
        let _ = remove_entry(&tmp);
    }
    copied
}

/// Grava `Win64\override.txt` com a linha + `\r\n` (temporário + rename).
fn write_override(win64: &Path) -> AppResult<()> {
    let dst = win64.join(OVERRIDE_NAME);
    let tmp = tmp_beside(&dst);
    let written = (|| {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(format!("{OVERRIDE_LINE}\r\n").as_bytes())?;
        f.sync_all()?;
        drop(f);
        std::fs::rename(&tmp, &dst)
    })();
    written.map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        act_io(&dst, &e)
    })
}

/// Registro do mod instalado (layout 2, não removido) nesta pasta do jogo.
fn active_record(game: &GameInstall, mods: &ModInstaller) -> Option<Installed> {
    mods.installed()
        .filter(|r| r.game_dir == game.dir && !r.removed && r.layout == LAYOUT)
}

/// Confere a ativação e ativa o que faltar (em todo Jogar, depois do `ensure`); grava o registro se mudou.
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
    if inspect(&win64, &rec)? == Activation::Active {
        return Ok(());
    }
    let before = rec.clone();
    let result = activate(&win64, &mod_dir(game), &mut rec, confirm_conflict);
    if rec != before {
        mods.save_record(&rec)?;
    }
    result
}

/// Nome de um arquivo só (sem pasta) em `Win64` ou `cbmp-backup\`.
fn plain_name(s: &str) -> bool {
    !s.contains('/') && safe_rel_path(s)
}

/// Devolve cada backup para `Win64\` se o destino estiver vazio; o que não volta fica no registro.
fn restore_backups(win64: &Path, rec: &mut Installed) -> AppResult<()> {
    let backup = win64.join(BACKUP_FOLDER);
    let mut pending = std::mem::take(&mut rec.backups).into_iter();
    while let Some(b) = pending.next() {
        if !plain_name(&b.name) || !plain_name(&b.stored) {
            log::warn!("backup com nome inválido no registro; ignorado");
            continue;
        }
        let (src, dst) = (backup.join(&b.stored), win64.join(&b.name));
        if !exists(&src) {
            log::warn!("cbmp-backup\\{} não existe mais", b.stored);
            continue;
        }
        if exists(&dst) {
            log::info!("{} ocupado; cbmp-backup\\{} fica", b.name, b.stored);
            rec.backups.push(b);
            continue;
        }
        if let Err(e) = std::fs::rename(&src, &dst) {
            rec.backups.push(b);
            rec.backups.extend(pending);
            return Err(act_io(&dst, &e));
        }
        log::info!("cbmp-backup\\{} devolvido como {}", b.stored, b.name);
    }
    // Pasta vazia sai; com algo dentro, fica.
    let _ = std::fs::remove_dir(&backup);
    Ok(())
}

/// Remove o mod do jogo (ver o topo do módulo). Não confere se o jogo está aberto (é do comando).
pub fn remove_mod(game: &GameInstall, mods: &ModInstaller) -> AppResult<()> {
    let Some(mut rec) = mods.installed().filter(|r| r.game_dir == game.dir) else {
        log::info!("remover mod: nada registrado nesta pasta do jogo");
        return Ok(());
    };
    let before = rec.clone();
    let (win64, container) = (win64_dir(game), container_dir(game));
    let result = (|| -> AppResult<()> {
        if matches!(loader_slot(&win64, &rec)?, Slot::Ours | Slot::Outdated) {
            let p = win64.join(LOADER_NAME);
            std::fs::remove_file(&p).map_err(|e| act_io(&p, &e))?;
        }
        if override_slot(&win64)? == Slot::Ours {
            let p = win64.join(OVERRIDE_NAME);
            std::fs::remove_file(&p).map_err(|e| act_io(&p, &e))?;
        }
        restore_backups(&win64, &mut rec)?;
        let staging = format!("{CURRENT_FOLDER}.staging");
        let old = format!("{CURRENT_FOLDER}.old");
        for name in [CURRENT_FOLDER, &staging, &old, POINTER_NAME] {
            let p = container.join(name);
            remove_entry(&p).map_err(|e| act_io(&p, &e))?;
        }
        Ok(())
    })();
    if result.is_ok() {
        rec.removed = true;
        rec.files.clear();
        if let Some(l) = rec.loader.take() {
            push_loader_history(&mut rec.loader_history, &l.sha256);
        }
        log::info!("mod {} removido do jogo", rec.version);
    }
    if rec != before {
        mods.save_record(&rec)?;
    }
    result
}

/// Estado do mod para as Configurações.
pub fn status(game: Option<&GameInstall>, mods: &ModInstaller) -> ModStatus {
    let Some(game) = game else {
        return ModStatus::NotInstalled;
    };
    let Some(rec) = mods
        .installed()
        .filter(|r| r.game_dir == game.dir && !r.removed)
    else {
        return ModStatus::NotInstalled;
    };
    match inspect(&win64_dir(game), &rec) {
        Ok(Activation::Conflict { .. } | Activation::LegacyUe4ss) => ModStatus::Conflict,
        _ => ModStatus::Installed {
            version: rec.version,
        },
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
}
