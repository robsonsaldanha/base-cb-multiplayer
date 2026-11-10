//! Superfície única exposta à UI. Cada comando aqui precisa constar em `build.rs` e na capability.

use crate::auth::{self, Auth};
use crate::error::{AppError, AppResult};
use crate::settings::{validate_game_dir, Settings, SettingsUpdate};
use serde::Serialize;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, MutexGuard};
use tauri::{AppHandle, Emitter, Manager, State};

/// Evento para a UI quando o login muda (deep link tratado, logout).
pub const AUTH_CHANGED: &str = "auth://changed";

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

/// Carga de `auth://changed`: `error` só quando um login terminou mal (negado, recusado pelo Hub, cofre).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AuthChanged {
    pub logged_in: bool,
    pub error: Option<AppError>,
}

/// Perfil para o topo da UI. `avatar` é `data:` URL (baixada pelo Rust do CDN do Discord) ou `null`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Profile {
    pub name: String,
    pub discord_id: String,
    pub avatar: Option<String>,
}

/// Evento a emitir depois de um deep link: link ignorado (não é nosso / state errado) não mexe na UI.
pub fn auth_event_for(result: &AppResult<()>) -> Option<AuthChanged> {
    match result {
        Ok(()) => Some(AuthChanged {
            logged_in: true,
            error: None,
        }),
        Err(e) if matches!(e.code, "invalid_deep_link" | "login_state_invalid") => None,
        Err(e) => Some(AuthChanged {
            logged_in: false,
            error: Some(e.clone()),
        }),
    }
}

/// Deep link vindo do plugin (1ª instância, ou repassado pela 2ª): trata fora da thread do evento.
pub fn handle_deep_link(app: AppHandle, auth: Arc<Auth>, url: String) {
    tauri::async_runtime::spawn(async move {
        let r = auth.on_deep_link(&url).await;
        // Link do nosso login (sucesso ou erro): traz a janela para a frente do navegador. Link alheio não.
        if let Some(ev) = auth_event_for(&r) {
            let _ = app.emit(AUTH_CHANGED, ev);
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }
    });
}

/// Abre o navegador do sistema na página de login do Hub. O resultado chega por `auth://changed`.
#[tauri::command]
pub fn login(auth: State<'_, Arc<Auth>>) -> AppResult<()> {
    let url = auth.begin_login();
    auth::open_in_browser(&url)
}

#[tauri::command]
pub async fn logout(app: AppHandle, auth: State<'_, Arc<Auth>>) -> AppResult<()> {
    let r = auth.logout().await;
    let _ = app.emit(
        AUTH_CHANGED,
        AuthChanged {
            logged_in: false,
            error: None,
        },
    );
    r
}

/// Perfil do usuário logado (`/v1/me`), renovando a sessão se preciso.
#[tauri::command]
pub async fn me(auth: State<'_, Arc<Auth>>) -> AppResult<Profile> {
    let me = auth.me().await?;
    let avatar = auth.hub().avatar_data_url(&me).await;
    Ok(Profile {
        name: me.name,
        discord_id: me.discord_id,
        avatar,
    })
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
    fn evento_de_auth_ignora_links_que_nao_sao_nossos() {
        assert_eq!(
            auth_event_for(&Ok(())),
            Some(AuthChanged {
                logged_in: true,
                error: None
            })
        );
        for code in ["invalid_deep_link", "login_state_invalid"] {
            assert_eq!(auth_event_for(&Err(AppError::new(code, "x"))), None);
        }
        let denied = auth_event_for(&Err(AppError::new("login_denied", "x"))).unwrap();
        assert!(!denied.logged_in);
        assert_eq!(denied.error.unwrap().code, "login_denied");
        let v = serde_json::to_value(AuthChanged {
            logged_in: false,
            error: None,
        })
        .unwrap();
        assert_eq!(v, serde_json::json!({ "loggedIn": false, "error": null }));
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
