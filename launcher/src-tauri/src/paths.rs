//! Pastas do launcher em `%LOCALAPPDATA%\CBMultiplayer`.
//!
//! `CBMP_DATA_DIR` troca a raiz, mas só em builds de debug (testes/dev); o release ignora a variável.

use std::path::PathBuf;

const APP_DIR: &str = "CBMultiplayer";

pub fn data_dir() -> PathBuf {
    #[cfg(debug_assertions)]
    if let Some(dir) = std::env::var_os("CBMP_DATA_DIR").filter(|d| !d.is_empty()) {
        return PathBuf::from(dir);
    }
    default_data_dir()
}

fn default_data_dir() -> PathBuf {
    if let Some(local) = std::env::var_os("LOCALAPPDATA").filter(|d| !d.is_empty()) {
        return PathBuf::from(local).join(APP_DIR);
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
        std::env::remove_var("CBMP_DATA_DIR");
        assert!(data_dir().ends_with(APP_DIR));
    }
}
