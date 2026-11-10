//! Orquestração do "Jogar": detalhe → ticket → manifest → download → montagem → mod → conexão → abrir.
//!
//! - Um por vez ([`PlayGate`]): downloads simultâneos do mesmo hash colidiriam, e o limite do cache não pode correr
//!   junto com uma montagem. "Limpar cache" usa a mesma trava.
//! - O manifest sempre passa por `validate_manifest` antes de `missing`/`download`/`mount`.
//! - Métodos de disco do `ContentStore` (missing, mount, enforce_limit) rodam em `spawn_blocking`.
//! - O ticket (TTL 300 s no Hub) é pedido de novo se passaram mais de [`TICKET_RENEW_AFTER_SECS`] desde a emissão.
//! - Cancelamento: conferido entre as etapas e repassado ao download do conteúdo e do mod.

use crate::auth::{Auth, Clock};
use crate::content::{self, ContentStore, Limits, Progress};
use crate::error::{AppError, AppResult};
use crate::game::{game_not_found, GameInstall};
use crate::launch::{self, ConnectInfo, LaunchOutcome};
use crate::modpkg::ModInstaller;
use serde::Serialize;
use std::collections::HashSet;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio_util::sync::CancellationToken;

/// Ticket com mais tempo que isso desde a emissão é renovado antes de gravar a conexão.
pub const TICKET_RENEW_AFTER_SECS: i64 = 240;

/// Etapas para a UI (`play://stage`), na ordem em que acontecem.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Stage {
    Ticket,
    Manifest,
    Download,
    Mount,
    Mod,
    Connect,
    Launch,
}

/// Tudo de que um play precisa (montado pelo comando a partir do estado do app; injetável nos testes).
pub struct PlayContext {
    pub auth: Arc<Auth>,
    pub content: ContentStore,
    pub mods: ModInstaller,
    pub run_dir: PathBuf,
    /// Relógio monotônico em segundos (o mesmo tipo do `Auth`).
    pub clock: Clock,
    pub simulate: bool,
    /// Instalação do jogo; obrigatória quando não simula.
    pub game: Option<GameInstall>,
    pub limits: Limits,
    pub cache_limit_bytes: u64,
    /// Servidores cujo conteúdo o limite do cache não apaga (favoritos + recentes); o atual é somado.
    pub keep: HashSet<String>,
    pub connect_ttl: Duration,
}

/// Trava "um play por vez" + cancelamento do play em andamento.
#[derive(Default)]
pub struct PlayGate {
    busy: tokio::sync::Mutex<()>,
    current: Arc<Mutex<Option<CancellationToken>>>,
}

/// Play (ou limpeza de cache) em andamento; libera a trava ao sair do escopo.
pub struct PlayRun<'a> {
    pub cancel: CancellationToken,
    current: Arc<Mutex<Option<CancellationToken>>>,
    _guard: tokio::sync::MutexGuard<'a, ()>,
}

impl std::fmt::Debug for PlayRun<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PlayRun")
            .field("cancelled", &self.cancel.is_cancelled())
            .finish()
    }
}

impl Drop for PlayRun<'_> {
    fn drop(&mut self) {
        *self.current.lock().unwrap_or_else(|e| e.into_inner()) = None;
    }
}

fn busy() -> AppError {
    AppError::new(
        "busy",
        "Já existe uma entrada em servidor em andamento. Aguarde ou cancele.",
    )
}

fn cancelled() -> AppError {
    AppError::new("cancelled", "Entrada no servidor cancelada.")
}

impl PlayGate {
    /// Começa um play; se já houver um, `busy`.
    pub fn try_begin(&self) -> AppResult<PlayRun<'_>> {
        let guard = self.busy.try_lock().map_err(|_| busy())?;
        let cancel = CancellationToken::new();
        *self.current.lock().unwrap_or_else(|e| e.into_inner()) = Some(cancel.clone());
        Ok(PlayRun {
            cancel,
            current: self.current.clone(),
            _guard: guard,
        })
    }

    /// Espera a trava (tarefas internas, como a faxina do cache na abertura).
    pub async fn begin(&self) -> PlayRun<'_> {
        let guard = self.busy.lock().await;
        let cancel = CancellationToken::new();
        *self.current.lock().unwrap_or_else(|e| e.into_inner()) = Some(cancel.clone());
        PlayRun {
            cancel,
            current: self.current.clone(),
            _guard: guard,
        }
    }

    /// Cancela o play em andamento; `false` se não houver nenhum.
    pub fn cancel(&self) -> bool {
        match self
            .current
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
        {
            Some(c) => {
                c.cancel();
                true
            }
            None => false,
        }
    }
}

fn unavailable(msg: &str) -> AppError {
    AppError::new(
        "server_unavailable",
        format!("Servidor indisponível: {msg}."),
    )
}

async fn blocking<T: Send + 'static>(
    f: impl FnOnce() -> AppResult<T> + Send + 'static,
) -> AppResult<T> {
    tokio::task::spawn_blocking(f)
        .await
        .map_err(|_| AppError::new("internal_error", "Falha interna na entrada no servidor."))?
}

/// Executa o play de `server_id`. `on_stage` recebe cada etapa ao começar; `progress` o progresso do download.
pub async fn play(
    ctx: &PlayContext,
    server_id: &str,
    on_stage: &(dyn Fn(Stage) + Send + Sync),
    progress: impl Fn(Progress) + Send + Sync + 'static,
    cancel: CancellationToken,
) -> AppResult<LaunchOutcome> {
    let check = || {
        if cancel.is_cancelled() {
            Err(cancelled())
        } else {
            Ok(())
        }
    };
    check()?;
    // Sem simular, o jogo precisa existir antes de baixar qualquer coisa.
    if !ctx.simulate && ctx.game.is_none() {
        return Err(game_not_found());
    }

    let hub = ctx.auth.hub();
    let detail = hub.server(server_id).await?;
    if !detail.online {
        return Err(unavailable("offline"));
    }
    let (Some(host), Some(game_port), Some(http_port), Some(protocol)) = (
        detail.host.clone(),
        detail.game_port,
        detail.http_port,
        detail.protocol,
    ) else {
        return Err(unavailable("dados de conexão ausentes"));
    };
    let protocol = u32::try_from(protocol).map_err(|_| unavailable("protocolo inválido"))?;
    if game_port == 0 {
        return Err(unavailable("porta inválida"));
    }
    check()?;

    on_stage(Stage::Ticket);
    let mut ticket = ctx.auth.ticket(server_id).await?;
    let issued_at = (ctx.clock)();
    check()?;

    on_stage(Stage::Manifest);
    let base = content::server_base_url(&host, http_port)?;
    let manifest = content::fetch_manifest(&base).await?;
    content::validate_manifest(&manifest, server_id, protocol, &ctx.limits)?;
    check()?;

    on_stage(Stage::Download);
    let missing = {
        let (store, m) = (ctx.content.clone(), manifest.clone());
        blocking(move || Ok(store.missing(&m))).await?
    };
    ctx.content
        .download(&base, &missing, progress, cancel.clone())
        .await?;
    check()?;

    on_stage(Stage::Mount);
    let content_dir = {
        let (store, m, id) = (ctx.content.clone(), manifest.clone(), server_id.to_string());
        blocking(move || store.mount(&id, &m)).await?
    };
    {
        // Sob a mesma trava do play; o servidor atual nunca é desmontado. Falha aqui não impede de jogar.
        let mut keep = ctx.keep.clone();
        keep.insert(server_id.to_string());
        let (store, limit) = (ctx.content.clone(), ctx.cache_limit_bytes);
        if let Err(e) = blocking(move || store.enforce_limit(limit, &keep)).await {
            log::warn!("limite do cache não aplicado: {}", e.code);
        }
    }
    check()?;

    on_stage(Stage::Mod);
    if !ctx.simulate {
        let game = ctx.game.as_ref().ok_or_else(game_not_found)?;
        ctx.mods.ensure(hub, protocol, game, cancel.clone()).await?;
    }
    check()?;

    on_stage(Stage::Connect);
    if (ctx.clock)() - issued_at > TICKET_RENEW_AFTER_SECS {
        log::info!("ticket renovado antes de abrir");
        ticket = ctx.auth.ticket(server_id).await?;
    }
    let info = ConnectInfo {
        host,
        port: game_port,
        ticket,
        server_id: server_id.to_string(),
        content_dir,
        protocol,
    };
    let path = launch::write_connect_file(&ctx.run_dir, &info)?;
    tokio::spawn(launch::remove_after(
        path,
        launch::connect_json(&info),
        ctx.connect_ttl,
    ));

    on_stage(Stage::Launch);
    launch::launch(ctx.game.as_ref(), ctx.simulate, &info)
}
