//! Superfície única exposta à UI. Cada comando aqui precisa constar em `build.rs` e na capability.

use crate::error::AppResult;
use crate::settings::Settings;
use std::path::PathBuf;
use std::sync::{Mutex, MutexGuard};
use tauri::{AppHandle, State};

pub struct AppState {
    settings_path: PathBuf,
    settings: Mutex<Settings>,
}

impl AppState {
    pub fn new(settings_path: PathBuf, settings: Settings) -> Self {
        Self {
            settings_path,
            settings: Mutex::new(settings),
        }
    }

    fn settings(&self) -> MutexGuard<'_, Settings> {
        // Um pânico com o lock na mão não deixa as configurações inconsistentes (só trocamos após gravar).
        self.settings.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn get_settings(&self) -> Settings {
        self.settings().clone()
    }

    /// Valida, grava e só então troca a cópia em memória. O lock serializa gravações concorrentes.
    pub fn set_settings(&self, new: Settings) -> AppResult<Settings> {
        let mut cur = self.settings();
        new.save(&self.settings_path)?;
        *cur = new.clone();
        Ok(new)
    }
}

#[tauri::command]
pub fn get_settings(state: State<'_, AppState>) -> AppResult<Settings> {
    Ok(state.get_settings())
}

#[tauri::command]
pub fn set_settings(state: State<'_, AppState>, settings: Settings) -> AppResult<Settings> {
    state.set_settings(settings)
}

#[tauri::command]
pub fn app_version(app: AppHandle) -> String {
    app.package_info().version.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn set_settings_valida_grava_e_atualiza_memoria() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        let state = AppState::new(path.clone(), Settings::default());

        let ok = Settings {
            favorites: vec!["srv".into()],
            ..Settings::default()
        };
        assert_eq!(state.set_settings(ok.clone()).unwrap(), ok);
        assert_eq!(state.get_settings(), ok);
        assert_eq!(Settings::load(&path), ok);

        let bad = Settings {
            hub_url: "http://evil.example.com".into(),
            ..Settings::default()
        };
        assert_eq!(
            state.set_settings(bad).unwrap_err().code,
            "invalid_settings"
        );
        assert_eq!(
            state.get_settings(),
            ok,
            "valor recusado não pode chegar à memória"
        );
        assert_eq!(Settings::load(&path), ok);
    }
}
