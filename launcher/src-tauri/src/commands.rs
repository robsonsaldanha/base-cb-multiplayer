//! Superfície única exposta à UI. Cada comando aqui precisa constar em `build.rs` e na capability.

use crate::error::{AppError, AppResult};
use crate::settings::{validate_game_dir, Settings, SettingsUpdate};
use std::path::PathBuf;
use std::sync::{Mutex, MutexGuard};
use tauri::{AppHandle, State};

pub struct AppState {
    settings_path: PathBuf,
    settings: Mutex<Settings>,
    /// Arquivo existe mas não pôde ser lido: gravar por cima apagaria as configurações reais do usuário.
    save_blocked: Option<AppError>,
}

impl AppState {
    /// Carrega `settings.json`; se o arquivo estiver ilegível, segue com os padrões e bloqueia gravações.
    pub fn load(settings_path: PathBuf) -> Self {
        let (settings, save_blocked) = match Settings::load(&settings_path) {
            Ok(s) => (s, None),
            Err(e) => (Settings::default(), Some(e)),
        };
        Self {
            settings_path,
            settings: Mutex::new(settings),
            save_blocked,
        }
    }

    fn settings(&self) -> MutexGuard<'_, Settings> {
        // Um pânico com o lock na mão não deixa as configurações inconsistentes (só trocamos após gravar).
        self.settings.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn get_settings(&self) -> Settings {
        self.settings().clone()
    }

    /// Aplica a mudança, valida, grava e só então troca a cópia em memória. O lock serializa gravações.
    fn store(&self, change: impl FnOnce(&Settings) -> Settings) -> AppResult<Settings> {
        if let Some(e) = &self.save_blocked {
            return Err(e.clone());
        }
        let mut cur = self.settings();
        let new = change(&cur);
        new.save(&self.settings_path)?;
        *cur = new.clone();
        Ok(new)
    }

    /// Campos editáveis pela UI; `hub_url` e `game_dir` não mudam por aqui.
    pub fn set_settings(&self, update: SettingsUpdate) -> AppResult<Settings> {
        self.store(|cur| cur.with_update(update))
    }

    /// Só para o Rust (seletor de pasta / localizador do jogo); não é comando da UI.
    pub fn set_game_dir(&self, dir: Option<PathBuf>) -> AppResult<Settings> {
        validate_game_dir(dir.as_deref())?;
        self.store(|cur| Settings {
            game_dir: dir,
            ..cur.clone()
        })
    }
}

#[tauri::command]
pub fn get_settings(state: State<'_, AppState>) -> AppResult<Settings> {
    Ok(state.get_settings())
}

#[tauri::command]
pub fn set_settings(state: State<'_, AppState>, settings: SettingsUpdate) -> AppResult<Settings> {
    state.set_settings(settings)
}

#[tauri::command]
pub fn app_version(app: AppHandle) -> String {
    app.package_info().version.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn update(favorites: &[&str], cache_limit_mb: u64) -> SettingsUpdate {
        SettingsUpdate {
            favorites: favorites.iter().map(|s| s.to_string()).collect(),
            recent: vec![],
            cache_limit_mb,
            max_server_content_mb: 2048,
            simulate_launch: true,
        }
    }

    #[test]
    fn set_settings_valida_grava_e_atualiza_memoria() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        let state = AppState::load(path.clone());

        let ok = state.set_settings(update(&["srv"], 4096)).unwrap();
        assert_eq!(ok.favorites, vec!["srv".to_string()]);
        assert_eq!(state.get_settings(), ok);
        assert_eq!(Settings::load(&path).unwrap(), ok);

        let err = state
            .set_settings(update(&["srv", "outro"], 0))
            .unwrap_err();
        assert_eq!(err.code, "invalid_settings");
        assert_eq!(
            state.get_settings(),
            ok,
            "valor recusado não pode chegar à memória"
        );
        assert_eq!(Settings::load(&path).unwrap(), ok);
    }

    #[test]
    fn set_settings_mantem_pasta_do_jogo_e_hub() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        let state = AppState::load(path.clone());
        let game = std::env::temp_dir().join("Hogwarts Legacy");
        state.set_game_dir(Some(game.clone())).unwrap();

        let after = state.set_settings(update(&["a"], 1024)).unwrap();
        assert_eq!(after.game_dir, Some(game));
        assert_eq!(after.hub_url, Settings::default().hub_url);
    }

    #[test]
    fn set_game_dir_recusa_relativo() {
        let dir = tempfile::tempdir().unwrap();
        let state = AppState::load(dir.path().join("settings.json"));
        let err = state.set_game_dir(Some(PathBuf::from("jogo"))).unwrap_err();
        assert_eq!(err.code, "invalid_settings");
        assert_eq!(state.get_settings().game_dir, None);
    }

    #[test]
    fn arquivo_ilegivel_bloqueia_gravacao() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::create_dir(&path).unwrap();
        let state = AppState::load(path.clone());
        assert_eq!(state.get_settings(), Settings::default());
        let err = state.set_settings(update(&["a"], 1024)).unwrap_err();
        assert_eq!(err.code, "settings_unreadable");
        assert!(path.is_dir(), "nada foi gravado por cima");
    }
}
