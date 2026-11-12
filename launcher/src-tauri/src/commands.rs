//! Superfície única exposta à UI. Cada comando aqui precisa constar em `build.rs` e na capability.

use crate::auth::{self, Auth};
use crate::content::{ContentStore, Limits, Progress};
use crate::error::{AppError, AppResult};
use crate::game::{
    self, game_not_found, game_running_error, game_running_with, Detection, GameCandidate,
    GameInstall, Processes, SystemProcesses,
};
use crate::hub::ServerListing;
use crate::launch::{LaunchOutcome, CONNECT_TTL};
use crate::modact::{self, Activation, FolderRemoval, ModStatus};
use crate::modpkg::{self, ModInstaller};
use crate::paths;
use crate::ping::{PingResult, Pinger};
use crate::play::{PlayContext, PlayGate, Stage};
use crate::server_cache::{CachedServers, ServerDirectory};
use crate::settings::{validate_game_dir, Settings, SettingsUpdate};
use crate::update::{self, UpdateStatus};
use serde::Serialize;
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use tauri::{AppHandle, Emitter, Manager, State};

/// Evento para a UI quando o login muda (deep link tratado, logout).
pub const AUTH_CHANGED: &str = "auth://changed";
/// Progresso do download do conteúdo do servidor (`content::Progress`, no máx. 10/s).
pub const CONTENT_PROGRESS: &str = "content://progress";
/// Etapa do Jogar que começou ([`PlayStageEvent`]).
pub const PLAY_STAGE: &str = "play://stage";
/// Aviso que não para nada (carga `AppError`): o mod continua numa pasta anterior do jogo.
pub const MOD_WARNING: &str = "mod://warning";

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

/// Procura real (registro e pastas do sistema, dono/permissões pelo Windows).
pub fn system_detect() -> Detection {
    game::detect(&game::SystemRegistry, &game::Roots::system())
}

/// Pasta do jogo: a das configurações se ainda for válida; senão procura na Steam/Epic e grava o que achar **de
/// confiável**. Uma candidata não confiável é devolvida, mas nunca gravada nem usada (só com [`adopt_detected`]).
pub fn resolve_game_with(state: &AppState, detect: impl FnOnce() -> Detection) -> Detection {
    if let Some(dir) = state.get_settings().game_dir {
        if let Ok(g) = game::validate_game_dir(&dir) {
            return Detection::Trusted(g);
        }
        log::warn!("pasta do jogo configurada não é mais válida; procurando de novo");
    }
    let found = detect();
    if let Detection::Trusted(g) = &found {
        if let Err(e) = state.set_game_dir(Some(g.dir.clone())) {
            log::warn!("pasta do jogo encontrada, mas não gravada: {}", e.code);
        }
    }
    found
}

/// [`resolve_game_with`] com a procura real; só a instalação utilizável.
pub fn resolve_game(state: &AppState) -> Option<GameInstall> {
    match resolve_game_with(state, system_detect) {
        // Junções/links no caminho escolhido (biblioteca da Steam movida) são resolvidos aqui, uma vez; dentro de
        // `cbmp\` e `cbmp-backup\` pontos de reanálise continuam recusados.
        Detection::Trusted(g) => Some(game::resolve_final(&g)),
        Detection::Candidate(_) | Detection::Ambiguous { .. } | Detection::NotFound => None,
    }
}

/// Resposta de [`detect_game`]: configurações atuais (com `gameDir` gravado se uma instalação confiável foi achada
/// agora) e, sem pasta utilizável, a **única** pasta não confiável achada para a UI sugerir. `ambiguous`: mais de uma
/// (nenhuma é sugerida; a UI pede o seletor).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GameDetection {
    pub settings: Settings,
    pub candidate: Option<GameCandidate>,
    pub ambiguous: bool,
}

/// Pasta do jogo atual ou procura de novo (como no Jogar). Nunca grava uma candidata não confiável.
pub fn detect_game_with(state: &AppState, detect: impl FnOnce() -> Detection) -> GameDetection {
    let (candidate, ambiguous) = match resolve_game_with(state, detect) {
        Detection::Candidate(c) => (Some(c), false),
        Detection::Ambiguous { .. } => (None, true),
        Detection::Trusted(_) | Detection::NotFound => (None, false),
    };
    GameDetection {
        settings: state.get_settings(),
        candidate,
        ambiguous,
    }
}

fn candidate_changed() -> AppError {
    AppError::new(
        "game_candidate_changed",
        "A pasta sugerida não foi encontrada de novo. Procure outra vez ou escolha a pasta do jogo.",
    )
}

fn game_ambiguous() -> AppError {
    AppError::new(
        "game_ambiguous",
        "Mais de uma pasta parece ser do Hogwarts Legacy. Escolha a pasta do jogo.",
    )
}

/// Confirmação da sugestão: `shown` é a pasta que a UI mostrou; `detection` é uma procura **nova** feita pelo Rust.
/// Só aceita se a procura achou de novo exatamente essa pasta como **única** candidata não confiável (a UI não escolhe
/// caminho nenhum; instalação confiável o Rust já grava sozinho, então não passa por aqui). Depois `confirm` (diálogo
/// nativo, fora do alcance da UI); recusado → nada muda. Por fim, o mesmo caminho do seletor de pasta:
/// [`game::validate_game_dir`] e [`AppState::set_game_dir`].
pub fn adopt_detected(
    state: &AppState,
    shown: &Path,
    detection: Detection,
    confirm: impl FnOnce(&GameCandidate) -> bool,
) -> AppResult<Settings> {
    let c = match detection {
        Detection::Candidate(c) if game::same_path(&c.dir, shown) => c,
        Detection::Ambiguous { .. } => return Err(game_ambiguous()),
        _ => return Err(candidate_changed()),
    };
    if !confirm(&c) {
        return Ok(state.get_settings());
    }
    log::info!(
        "pasta do jogo sugerida ({:?}) confirmada pelo usuário",
        c.store
    );
    let g = game::validate_game_dir(&c.dir)?;
    state.set_game_dir(Some(g.dir))
}

/// Uma confirmação nativa por vez: a UI não consegue empilhar diálogos para cansar o usuário.
#[derive(Debug, Default)]
pub struct ConfirmGate(std::sync::atomic::AtomicBool);

/// Confirmação aberta; solta a trava ao sair de escopo.
pub struct ConfirmTicket<'a>(&'a ConfirmGate);

impl ConfirmGate {
    pub const fn new() -> Self {
        Self(std::sync::atomic::AtomicBool::new(false))
    }

    /// `game_confirm_open` se já houver uma confirmação aberta.
    pub fn try_begin(&self) -> AppResult<ConfirmTicket<'_>> {
        use std::sync::atomic::Ordering;
        if self.0.swap(true, Ordering::AcqRel) {
            return Err(AppError::new(
                "game_confirm_open",
                "Já existe uma confirmação da pasta do jogo aberta.",
            ));
        }
        Ok(ConfirmTicket(self))
    }
}

impl Drop for ConfirmTicket<'_> {
    fn drop(&mut self) {
        self.0 .0.store(false, std::sync::atomic::Ordering::Release);
    }
}

static GAME_CONFIRM: ConfirmGate = ConfirmGate::new();

/// Pasta do jogo atual ou a sugestão (ver [`GameDetection`]). Registro/disco fora do executor assíncrono.
#[tauri::command]
pub async fn detect_game(app: AppHandle) -> AppResult<GameDetection> {
    tokio::task::spawn_blocking(move || detect_game_with(&app.state::<AppState>(), system_detect))
        .await
        .map_err(|_| AppError::new("internal_error", "Falha interna ao procurar o jogo."))
}

/// "Usar esta pasta" na sugestão: procura de novo, confere que achou só `dir`, pede confirmação no diálogo nativo e
/// grava como o seletor de pasta. Recusar no diálogo devolve as configurações sem mudança.
#[tauri::command]
pub async fn use_detected_game(app: AppHandle, dir: String) -> AppResult<Settings> {
    let _open = GAME_CONFIRM.try_begin()?;
    tokio::task::spawn_blocking(move || {
        let state = app.state::<AppState>();
        adopt_detected(&state, Path::new(&dir), system_detect(), |c| {
            confirm_candidate_dialog(&app, c)
        })
    })
    .await
    .map_err(|_| AppError::new("internal_error", "Falha interna ao gravar a pasta do jogo."))?
}

/// Texto do diálogo nativo: não atribui a pasta a ninguém (o `.item` que a indicou pode ter sido plantado).
pub fn confirm_text(c: &GameCandidate) -> String {
    format!(
        "O launcher encontrou uma pasta que parece ser do Hogwarts Legacy:\n\n{}\n\nO launcher não conseguiu verificar \
         quem controla esta pasta: outra conta deste computador pode tê-la criado ou alterado.\n\nUse esta pasta só se \
         foi nela que você instalou o jogo. Se não tiver certeza, cancele e use \"Escolher pasta do jogo\".",
        c.dir.display()
    )
}

/// Diálogo nativo (bloqueia esta thread, nunca a principal): a UI não consegue responder por ele.
fn confirm_candidate_dialog(app: &AppHandle, c: &GameCandidate) -> bool {
    use tauri_plugin_dialog::{DialogExt, MessageDialogButtons, MessageDialogKind};
    let mut dialog = app
        .dialog()
        .message(confirm_text(c))
        .title("Pasta do Hogwarts Legacy")
        .kind(MessageDialogKind::Warning)
        .buttons(MessageDialogButtons::OkCancelCustom(
            "Usar esta pasta".into(),
            "Cancelar".into(),
        ));
    if let Some(w) = app.get_webview_window("main") {
        dialog = dialog.parent(&w);
    }
    dialog.blocking_show()
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
        processes: Arc::new(SystemProcesses),
        launcher_version: app.package_info().version.to_string(),
        // Conflito só se resolve pela confirmação nativa (`resolve_mod_conflict`), nunca pelo Jogar.
        confirm_conflict: false,
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
        progress_emitter(app.clone()),
        run.cancel.clone(),
    )
    .await;
    if let Err(e) = &r {
        log::warn!("jogar em {server_id:?} falhou: {}", e.code);
    }
    if let Some(g) = ctx.game.as_ref().filter(|_| !ctx.simulate) {
        let left = modact::leftover_folders(Some(&g.dir), &ctx.mods);
        if let Some(w) = old_folder_warning_once(&OLD_FOLDER_WARNED, &left) {
            let _ = app.emit(MOD_WARNING, w);
        }
    }
    r
}

/// Aviso de [`MOD_WARNING`] quando o mod ficou em pastas anteriores do jogo (inacessíveis ou com falha ao remover).
/// O aviso de pasta anterior sai no máximo uma vez por execução do launcher.
static OLD_FOLDER_WARNED: AtomicBool = AtomicBool::new(false);

/// [`old_folder_warning`] só na primeira vez em que há pastas a avisar (`flag` marca que já avisou).
pub fn old_folder_warning_once(flag: &AtomicBool, left: &[PathBuf]) -> Option<AppError> {
    let w = old_folder_warning(left)?;
    (!flag.swap(true, Ordering::SeqCst)).then_some(w)
}

pub fn old_folder_warning(left: &[PathBuf]) -> Option<AppError> {
    if left.is_empty() {
        return None;
    }
    let dirs: Vec<String> = left.iter().map(|d| d.display().to_string()).collect();
    Some(AppError::new(
        "mod_old_folder",
        format!(
            "O mod do CB Multiplayer continua na pasta anterior do jogo: {}. Ele sai de lá no próximo Jogar em que a \
             pasta estiver acessível, ou em Configurações → Remover mod do jogo.",
            dirs.join("; ")
        ),
    ))
}

// ----- mod do jogo (Configurações) -----

/// Estado do mod na pasta do jogo atual + pastas anteriores do jogo onde ele ainda está.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ModInfo {
    #[serde(flatten)]
    pub status: ModStatus,
    pub other_folders: Vec<PathBuf>,
    /// Das `other_folders`, as que não existem mais ("Esquecer pasta"), com os nomes dos arquivos do jogador.
    pub gone_folders: Vec<modact::GoneFolder>,
}

/// Resposta de "Remover mod do jogo": estado depois e resultado por pasta.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoveModResult {
    pub status: ModInfo,
    pub folders: Vec<FolderRemoval>,
}

pub fn mod_info(game: Option<&GameInstall>, mods: &ModInstaller) -> ModInfo {
    ModInfo {
        status: modact::status(game, mods),
        other_folders: modact::leftover_folders(game.map(|g| g.dir.as_path()), mods),
        gone_folders: modact::gone_folders(game.map(|g| g.dir.as_path()), mods),
    }
}

/// "Remover mod do jogo": recusa com o jogo aberto; senão remove de todas as pastas registradas.
pub fn remove_mod_with(
    game: Option<&GameInstall>,
    mods: &ModInstaller,
    processes: &dyn Processes,
) -> AppResult<RemoveModResult> {
    if game_running_with(processes) {
        return Err(game_running_error());
    }
    let folders = modact::remove_everywhere(mods, game.map(|g| g.dir.as_path()));
    for f in &folders {
        if let Some(e) = &f.error {
            log::warn!("mod não removido de {}: {}", f.game_dir.display(), e.code);
        }
    }
    Ok(RemoveModResult {
        status: mod_info(game, mods),
        folders,
    })
}

/// "Usar o carregador do CB Multiplayer": recusa com o jogo aberto; sem conflito só ativa; com conflito comum pede
/// `confirm` (diálogo nativo com [`modact::CONFLICT_DIALOG`]) e, aceito, ativa movendo os arquivos do jogador para
/// `cbmp-backup\`. UE4SS antigo (`xinput1_3.dll`) não tem saída automática: o erro volta sem diálogo.
pub fn resolve_conflict_with(
    game: &GameInstall,
    mods: &ModInstaller,
    processes: &dyn Processes,
    confirm: impl FnOnce() -> bool,
) -> AppResult<ModInfo> {
    if game_running_with(processes) {
        return Err(game_running_error());
    }
    match modact::ensure_active(game, mods, false) {
        Ok(()) => return Ok(mod_info(Some(game), mods)),
        Err(e) if e.code != "mod_conflict" => return Err(e),
        Err(e) => {
            let legacy = mods
                .installed()
                .filter(|r| r.game_dir == game.dir)
                .and_then(|r| modact::inspect(&modpkg::win64_dir(game), &r).ok())
                == Some(Activation::LegacyUe4ss);
            if legacy {
                return Err(e);
            }
        }
    }
    if !confirm() {
        log::info!("conflito do mod: jogador cancelou");
        return Ok(mod_info(Some(game), mods));
    }
    log::info!("conflito do mod: jogador confirmou usar o carregador do CB Multiplayer");
    modact::ensure_active(game, mods, true)?;
    Ok(mod_info(Some(game), mods))
}

fn mod_logs_missing() -> AppError {
    AppError::new(
        "mod_logs_missing",
        "A pasta de logs do mod ainda não existe.",
    )
}

/// `Win64\cbmp\logs` da pasta do jogo, se for uma pasta de verdade (não link).
pub fn mod_logs_dir(game: Option<&GameInstall>) -> AppResult<PathBuf> {
    let g = game.ok_or_else(mod_logs_missing)?;
    let dir = modpkg::container_dir(g).join("logs");
    match std::fs::symlink_metadata(&dir) {
        Ok(m) if m.is_dir() && !m.file_type().is_symlink() => Ok(dir),
        _ => Err(mod_logs_missing()),
    }
}

fn internal(what: &str) -> AppError {
    AppError::new("internal_error", format!("Falha interna {what}."))
}

/// Estado do mod para as Configurações (procura o jogo como o Jogar, fora do executor assíncrono). Só lê: não pega a
/// trava do play (um Jogar clicado ao mesmo tempo não recebe `busy`) nem grava o registro; o manifesto de backups da
/// pasta é adotado no Jogar, na remoção e na abertura do launcher.
#[tauri::command]
pub async fn mod_status(app: AppHandle) -> AppResult<ModInfo> {
    tokio::task::spawn_blocking(move || {
        let game = resolve_game(&app.state::<AppState>());
        mod_info(game.as_ref(), &ModInstaller::system())
    })
    .await
    .map_err(|_| internal("ao conferir o mod"))
}

/// "Esquecer pasta": tira do registro uma pasta anterior do jogo que não existe mais (ver
/// [`modact::forget_folder`]). A confirmação, com os nomes dos arquivos do jogador, é da UI. Sob a trava do play.
#[tauri::command]
pub async fn forget_mod_folder(
    app: AppHandle,
    gate: State<'_, PlayGate>,
    dir: String,
) -> AppResult<ModInfo> {
    let _run = gate.try_begin()?;
    tokio::task::spawn_blocking(move || {
        let game = resolve_game(&app.state::<AppState>());
        let mods = ModInstaller::system();
        modact::forget_folder(Path::new(&dir), &mods)?;
        Ok(mod_info(game.as_ref(), &mods))
    })
    .await
    .map_err(|_| internal("ao esquecer a pasta do jogo"))?
}

/// "Remover mod do jogo" (todas as pastas registradas). Não corre junto com um play; recusa com o jogo aberto.
#[tauri::command]
pub async fn remove_mod(app: AppHandle, gate: State<'_, PlayGate>) -> AppResult<RemoveModResult> {
    let _run = gate.try_begin()?;
    tokio::task::spawn_blocking(move || {
        let game = resolve_game(&app.state::<AppState>());
        remove_mod_with(game.as_ref(), &ModInstaller::system(), &SystemProcesses)
    })
    .await
    .map_err(|_| internal("ao remover o mod"))?
}

/// Conflito com outro carregador: diálogo nativo e, aceito, ativação com backup. Sob a trava do play.
#[tauri::command]
pub async fn resolve_mod_conflict(app: AppHandle, gate: State<'_, PlayGate>) -> AppResult<ModInfo> {
    let _run = gate.try_begin()?;
    tokio::task::spawn_blocking(move || {
        let game = resolve_game(&app.state::<AppState>()).ok_or_else(game_not_found)?;
        resolve_conflict_with(&game, &ModInstaller::system(), &SystemProcesses, || {
            conflict_dialog(&app)
        })
    })
    .await
    .map_err(|_| internal("ao resolver o conflito do mod"))?
}

/// Diálogo nativo de conflito (bloqueia esta thread, nunca a principal): a UI não consegue responder por ele.
fn conflict_dialog(app: &AppHandle) -> bool {
    use tauri_plugin_dialog::{DialogExt, MessageDialogButtons, MessageDialogKind};
    let mut dialog = app
        .dialog()
        .message(modact::CONFLICT_DIALOG)
        .title("Outro carregador de mods")
        .kind(MessageDialogKind::Warning)
        .buttons(MessageDialogButtons::OkCancelCustom(
            "Usar o carregador do CB Multiplayer".into(),
            "Cancelar".into(),
        ));
    if let Some(w) = app.get_webview_window("main") {
        dialog = dialog.parent(&w);
    }
    dialog.blocking_show()
}

/// Abre `Win64\cbmp\logs` no Explorer (só pelo Rust). Pasta ausente → `mod_logs_missing`.
#[tauri::command]
pub async fn open_mod_logs(app: AppHandle) -> AppResult<()> {
    tokio::task::spawn_blocking(move || {
        let game = resolve_game(&app.state::<AppState>());
        let dir = mod_logs_dir(game.as_ref())?;
        open_folder_os(&dir)
    })
    .await
    .map_err(|_| internal("ao abrir a pasta de logs"))?
}

#[cfg(windows)]
fn open_folder_os(dir: &Path) -> AppResult<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::UI::Shell::ShellExecuteW;
    use windows_sys::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;
    let verb: Vec<u16> = "open".encode_utf16().chain(Some(0)).collect();
    let target: Vec<u16> = dir.as_os_str().encode_wide().chain(Some(0)).collect();
    // SAFETY: strings UTF-16 terminadas em zero que vivem até o fim da chamada; demais ponteiros nulos.
    let r = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            verb.as_ptr(),
            target.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            SW_SHOWNORMAL,
        )
    };
    // ShellExecuteW: valor > 32 = sucesso.
    if r as isize > 32 {
        Ok(())
    } else {
        Err(AppError::new(
            "open_folder_failed",
            "Não foi possível abrir a pasta.",
        ))
    }
}

#[cfg(not(windows))]
fn open_folder_os(_dir: &Path) -> AppResult<()> {
    Err(AppError::new(
        "open_folder_failed",
        "Não foi possível abrir a pasta.",
    ))
}

/// Cancela o play em andamento (`false` se não houver).
#[tauri::command]
pub fn cancel_play(gate: State<'_, PlayGate>) -> bool {
    gate.cancel()
}

// ----- lista de servidores e ping -----

fn unix_now() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| i64::try_from(d.as_secs()).unwrap_or(i64::MAX))
        .unwrap_or(0)
}

/// `GET /v1/servers`. Em sucesso a lista vira a conhecida (ping) e é gravada em `servers-cache.json`. Em erro a
/// UI pede [`cached_servers`] e mostra a lista como "desatualizada".
#[tauri::command]
pub async fn list_servers(
    auth: State<'_, Arc<Auth>>,
    dir: State<'_, Arc<ServerDirectory>>,
) -> AppResult<Vec<ServerListing>> {
    let list = auth.hub().servers().await?;
    let d = dir.inner().clone();
    // Sem id repetido nem lixo (a UI usa o id como chave); gravar fora do executor assíncrono.
    tokio::task::spawn_blocking(move || d.store_fresh(list, unix_now()))
        .await
        .map_err(|_| {
            log::warn!("falha interna ao guardar a lista de servidores");
            AppError::new(
                "internal_error",
                "Falha interna ao guardar a lista de servidores.",
            )
        })
}

/// Última lista gravada (`null` se não houver). Os endereços dela passam a valer para o ping.
#[tauri::command]
pub async fn cached_servers(
    dir: State<'_, Arc<ServerDirectory>>,
) -> AppResult<Option<CachedServers>> {
    let d = dir.inner().clone();
    tokio::task::spawn_blocking(move || d.cached())
        .await
        .map_err(|_| AppError::new("internal_error", "Falha interna ao ler a lista guardada."))
}

/// Ping por id (`GET /info`, 2 s, ≤ 16 simultâneos no launcher, um pedido por endereço a cada 25 s). Host/porta só
/// da lista conhecida; id desconhecido → `ms: null`.
#[tauri::command]
pub async fn ping_servers(
    ids: Vec<String>,
    dir: State<'_, Arc<ServerDirectory>>,
    pinger: State<'_, Pinger>,
) -> AppResult<Vec<PingResult>> {
    Ok(pinger.ping_ids(&dir, ids).await)
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
