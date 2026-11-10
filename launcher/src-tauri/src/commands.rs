//! Superfície única exposta à UI. Cada comando aqui precisa constar em `build.rs` e na capability.

use crate::auth::{self, Auth};
use crate::content::{ContentStore, Limits, Progress};
use crate::error::{AppError, AppResult};
use crate::game::{self, GameInstall};
use crate::launch::{LaunchOutcome, CONNECT_TTL};
use crate::modpkg::ModInstaller;
use crate::paths;
use crate::play::{PlayContext, PlayGate, Stage};
use crate::settings::{validate_game_dir, Settings, SettingsUpdate};
use crate::update::{self, UpdateStatus};
use serde::Serialize;
use std::collections::HashSet;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, MutexGuard};
use tauri::{AppHandle, Emitter, Manager, State};

/// Evento para a UI quando o login muda (deep link tratado, logout).
pub const AUTH_CHANGED: &str = "auth://changed";
/// Progresso do download do conteúdo do servidor (`content::Progress`, no máx. 10/s).
pub const CONTENT_PROGRESS: &str = "content://progress";
/// Etapa do Jogar que começou ([`PlayStageEvent`]).
pub const PLAY_STAGE: &str = "play://stage";

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

// ----- conteúdo do servidor (orquestrado pelo `play` da Task 7) -----

/// Cache e montagens nas pastas do launcher. Criada uma vez em `lib::run` (estado gerenciado do Tauri): o índice e
/// as pastas de download são coordenados por uma instância só.
pub fn content_store() -> ContentStore {
    ContentStore::new(paths::cache_dir(), paths::servers_dir())
}

/// Limites do manifest vindos das configurações (`maxServerContentMB`).
pub fn content_limits(s: &Settings) -> Limits {
    Limits::new(s.max_server_content_mb)
}

/// `cacheLimitMB` em bytes.
pub fn cache_limit_bytes(s: &Settings) -> u64 {
    s.cache_limit_mb.saturating_mul(1024 * 1024)
}

/// Servidores cujo conteúdo montado o limite do cache nunca apaga: favoritos e recentes.
pub fn keep_servers(s: &Settings) -> HashSet<String> {
    s.favorites.iter().chain(&s.recent).cloned().collect()
}

/// Repassa o progresso do download à UI como `content://progress` (o `ContentStore` já limita a 10/s).
pub fn progress_emitter(app: AppHandle) -> impl Fn(Progress) + Send + Sync + 'static {
    move |p| {
        let _ = app.emit(CONTENT_PROGRESS, p);
    }
}

/// "Limpar cache" (Configurações): apaga cache e montagens. Não corre junto com um play.
#[tauri::command]
pub async fn clear_cache(
    content: State<'_, ContentStore>,
    gate: State<'_, PlayGate>,
) -> AppResult<()> {
    let _run = gate.try_begin()?;
    let store = content.inner().clone();
    tokio::task::spawn_blocking(move || store.clear())
        .await
        .map_err(|_| AppError::new("internal_error", "Falha interna ao limpar o cache."))?
}

// ----- jogo -----

/// Pasta do jogo: a das configurações se ainda for válida; senão procura na Steam/Epic e grava o que achar.
pub fn resolve_game(state: &AppState) -> Option<GameInstall> {
    if let Some(dir) = state.get_settings().game_dir {
        if let Ok(g) = game::validate_game_dir(&dir) {
            return Some(g);
        }
        log::warn!("pasta do jogo configurada não é mais válida; procurando de novo");
    }
    let found = game::locate(&game::SystemRegistry, &game::Roots::system())?;
    if let Err(e) = state.set_game_dir(Some(found.dir.clone())) {
        log::warn!("pasta do jogo encontrada, mas não gravada: {}", e.code);
    }
    Some(found)
}

/// Seletor de pasta (plugin dialog, chamado só daqui) → valida que é o Hogwarts Legacy → grava `gameDir`.
/// Cancelar o seletor não muda nada.
#[tauri::command]
pub async fn pick_game_dir(app: AppHandle, state: State<'_, AppState>) -> AppResult<Settings> {
    use tauri_plugin_dialog::DialogExt;
    let (tx, rx) = tokio::sync::oneshot::channel();
    let mut dialog = app.dialog().file().set_title("Pasta do Hogwarts Legacy");
    if let Some(w) = app.get_webview_window("main") {
        dialog = dialog.set_parent(&w);
    }
    dialog.pick_folder(move |p| {
        let _ = tx.send(p);
    });
    let Some(picked) = rx.await.ok().flatten() else {
        return Ok(state.get_settings());
    };
    let dir = picked.into_path().map_err(|_| {
        AppError::new(
            "game_invalid",
            "Escolha uma pasta do computador com o Hogwarts Legacy.",
        )
    })?;
    let g = game::validate_game_dir(&dir)?;
    state.set_game_dir(Some(g.dir))
}

// ----- jogar -----

/// Carga de `play://stage`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlayStageEvent {
    pub server_id: String,
    pub stage: Stage,
}

/// Jogar: ticket → manifest → download → montagem → mod (se não simular) → conexão → abrir. Etapas por
/// `play://stage`, progresso por `content://progress`. Um por vez (`busy`); `cancel_play` interrompe.
#[tauri::command]
pub async fn play(
    app: AppHandle,
    server_id: String,
    state: State<'_, AppState>,
    auth: State<'_, Arc<Auth>>,
    content: State<'_, ContentStore>,
    gate: State<'_, PlayGate>,
) -> AppResult<LaunchOutcome> {
    let run = gate.try_begin()?;
    let settings = state.get_settings();
    // Simulando, o jogo não é necessário (nem procurado). Senão: registro/disco fora do executor assíncrono.
    let game = if settings.simulate_launch {
        None
    } else {
        let h = app.clone();
        tokio::task::spawn_blocking(move || resolve_game(&h.state::<AppState>()))
            .await
            .map_err(|_| AppError::new("internal_error", "Falha interna ao procurar o jogo."))?
    };
    let ctx = PlayContext {
        auth: auth.inner().clone(),
        content: content.inner().clone(),
        mods: ModInstaller::system(),
        run_dir: paths::run_dir(),
        clock: auth::monotonic_clock(),
        simulate: settings.simulate_launch,
        game,
        limits: content_limits(&settings),
        cache_limit_bytes: cache_limit_bytes(&settings),
        keep: keep_servers(&settings),
        connect_ttl: CONNECT_TTL,
    };
    let stage_app = app.clone();
    let id = server_id.clone();
    let on_stage = move |stage: Stage| {
        let _ = stage_app.emit(
            PLAY_STAGE,
            PlayStageEvent {
                server_id: id.clone(),
                stage,
            },
        );
    };
    let r = crate::play::play(
        &ctx,
        &server_id,
        &on_stage,
        progress_emitter(app),
        run.cancel.clone(),
    )
    .await;
    if let Err(e) = &r {
        log::warn!("jogar em {server_id:?} falhou: {}", e.code);
    }
    r
}

/// Cancela o play em andamento (`false` se não houver).
#[tauri::command]
pub fn cancel_play(gate: State<'_, PlayGate>) -> bool {
    gate.cancel()
}

// ----- atualização do launcher -----

#[tauri::command]
pub async fn check_update(app: AppHandle, state: State<'_, AppState>) -> AppResult<UpdateStatus> {
    update::check(&app, &state.get_settings().hub_url).await
}

/// Baixa, confere e instala a versão nova (o instalador fecha o launcher). Não corre durante um play.
#[tauri::command]
pub async fn install_update(
    app: AppHandle,
    state: State<'_, AppState>,
    gate: State<'_, PlayGate>,
) -> AppResult<()> {
    let _run = gate.try_begin()?;
    update::install(&app, &state.get_settings().hub_url).await
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
    fn conteudo_usa_limites_e_keep_das_configuracoes() {
        let s = Settings {
            favorites: vec!["srv_fav".into()],
            recent: vec!["srv_rec".into(), "srv_fav".into()],
            cache_limit_mb: 3,
            max_server_content_mb: 5,
            ..Settings::default()
        };
        assert_eq!(content_limits(&s), Limits::new(5));
        assert_eq!(cache_limit_bytes(&s), 3 * 1024 * 1024);
        assert_eq!(
            keep_servers(&s),
            HashSet::from(["srv_fav".to_string(), "srv_rec".to_string()])
        );
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
