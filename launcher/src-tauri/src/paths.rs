//! Pastas do launcher em `%LOCALAPPDATA%\CBMultiplayer` (build de teste local: `CBMultiplayer-TesteLocal`; de teste
//! online: `CBMultiplayer-Teste`).
//!
//! No Windows a raiz vem da pasta conhecida `FOLDERID_LocalAppData` (`SHGetKnownFolderPath`), não da variável de
//! ambiente (que qualquer processo pai pode trocar). `CBMP_DATA_DIR` troca a raiz, mas só em builds de debug
//! (testes/dev); o release ignora a variável.

use std::path::PathBuf;

/// Pasta do app em `%LOCALAPPDATA%`. O build de teste local tem a sua: os dois podem rodar ao mesmo tempo
/// (identificadores diferentes) e não podem dividir cache, `run\`, índices nem log.
pub const APP_DIR: &str = if crate::profile::LOCAL_TEST {
    "CBMultiplayer-TesteLocal"
} else if crate::profile::ONLINE_TEST {
    "CBMultiplayer-Teste"
} else {
    "CBMultiplayer"
};

pub fn data_dir() -> PathBuf {
    #[cfg(debug_assertions)]
    if let Some(dir) = std::env::var_os("CBMP_DATA_DIR").filter(|d| !d.is_empty()) {
        return PathBuf::from(dir);
    }
    default_data_dir()
}

fn default_data_dir() -> PathBuf {
    #[cfg(windows)]
    if let Some(local) = local_app_data() {
        return local.join(APP_DIR);
    }
    // Fora do Windows (não suportado oficialmente): convenção XDG.
    if let Some(xdg) = std::env::var_os("XDG_DATA_HOME").filter(|d| !d.is_empty()) {
        return PathBuf::from(xdg).join(APP_DIR);
    }
    if let Some(home) = std::env::var_os("HOME").filter(|d| !d.is_empty()) {
        return PathBuf::from(home)
            .join(".local")
            .join("share")
            .join(APP_DIR);
    }
    std::env::temp_dir().join(APP_DIR)
}

/// `FOLDERID_LocalAppData` do usuário atual (`None` se a API falhar).
#[cfg(windows)]
pub fn local_app_data() -> Option<PathBuf> {
    known_folder(&windows_sys::Win32::UI::Shell::FOLDERID_LocalAppData)
}

/// `FOLDERID_ProgramData` (`C:\ProgramData`), pela API e não pela variável de ambiente.
#[cfg(windows)]
pub fn program_data() -> Option<PathBuf> {
    known_folder(&windows_sys::Win32::UI::Shell::FOLDERID_ProgramData)
}

#[cfg(windows)]
fn known_folder(id: &windows_sys::core::GUID) -> Option<PathBuf> {
    use std::os::windows::ffi::OsStringExt;
    use windows_sys::Win32::System::Com::CoTaskMemFree;
    use windows_sys::Win32::UI::Shell::{SHGetKnownFolderPath, KF_FLAG_DEFAULT};
    let mut raw: windows_sys::core::PWSTR = std::ptr::null_mut();
    // SAFETY: GUID válido; `raw` recebe um buffer alocado pela API que liberamos com CoTaskMemFree em qualquer caso.
    let hr =
        unsafe { SHGetKnownFolderPath(id, KF_FLAG_DEFAULT as _, std::ptr::null_mut(), &mut raw) };
    let out = if hr >= 0 && !raw.is_null() {
        // SAFETY: em sucesso `raw` é uma string UTF-16 terminada em zero.
        let len = unsafe { (0..).take_while(|&i| *raw.add(i) != 0).count() };
        let slice = unsafe { std::slice::from_raw_parts(raw, len) };
        Some(PathBuf::from(std::ffi::OsString::from_wide(slice)))
    } else {
        None
    };
    // SAFETY: liberar nulo é permitido.
    unsafe { CoTaskMemFree(raw as *const _) };
    out.filter(|p| p.is_absolute())
}

pub fn cache_dir() -> PathBuf {
    data_dir().join("cache")
}

pub fn servers_dir() -> PathBuf {
    data_dir().join("servers")
}

pub fn mods_dir() -> PathBuf {
    data_dir().join("mods")
}

pub fn run_dir() -> PathBuf {
    data_dir().join("run")
}

pub fn logs_dir() -> PathBuf {
    data_dir().join("logs")
}

pub fn settings_file() -> PathBuf {
    data_dir().join("settings.json")
}

/// Última lista de servidores do Hub (modo offline).
pub fn servers_cache_file() -> PathBuf {
    data_dir().join("servers-cache.json")
}

#[cfg(test)]
mod tests {
    use super::*;

    // Único teste que mexe na variável de ambiente; os demais módulos recebem caminhos explícitos.
    #[test]
    fn subpastas_ficam_sob_data_dir_e_override_vale_em_debug() {
        let tmp = tempfile::tempdir().unwrap();
        std::env::set_var("CBMP_DATA_DIR", tmp.path());
        assert_eq!(data_dir(), tmp.path());
        assert_eq!(cache_dir(), tmp.path().join("cache"));
        assert_eq!(servers_dir(), tmp.path().join("servers"));
        assert_eq!(mods_dir(), tmp.path().join("mods"));
        assert_eq!(run_dir(), tmp.path().join("run"));
        assert_eq!(logs_dir(), tmp.path().join("logs"));
        assert_eq!(settings_file(), tmp.path().join("settings.json"));
        assert_eq!(servers_cache_file(), tmp.path().join("servers-cache.json"));
        std::env::remove_var("CBMP_DATA_DIR");
        assert!(data_dir().ends_with(APP_DIR));
    }

    #[test]
    fn pasta_do_app_por_perfil() {
        if cfg!(feature = "local-test") {
            assert_eq!(APP_DIR, "CBMultiplayer-TesteLocal");
        } else if cfg!(feature = "online-test") {
            assert_eq!(APP_DIR, "CBMultiplayer-Teste");
        } else {
            assert_eq!(APP_DIR, "CBMultiplayer");
        }
        assert!(default_data_dir().ends_with(APP_DIR));
    }

    #[cfg(windows)]
    #[test]
    fn raiz_vem_da_pasta_conhecida_e_nao_da_variavel() {
        let known = local_app_data().expect("FOLDERID_LocalAppData");
        assert!(known.is_absolute() && known.is_dir());
        assert_eq!(default_data_dir(), known.join(APP_DIR));
        let pd = program_data().expect("FOLDERID_ProgramData");
        assert!(pd.is_absolute() && pd.is_dir());
    }
}
