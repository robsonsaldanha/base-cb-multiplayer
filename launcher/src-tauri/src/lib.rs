//! CB Multiplayer — backend do launcher. Rede, disco, cofre e processos ficam aqui; a UI só chama os comandos
//! de `commands`.

pub mod auth;
pub mod commands;
pub mod content;
pub mod error;
pub mod game;
pub mod hub;
pub mod launch;
pub mod modpkg;
pub mod paths;
pub mod ping;
pub mod play;
pub mod server_cache;
pub mod settings;
pub mod update;

use commands::AppState;
use std::sync::Arc;
use tauri::Manager;
use tauri_plugin_deep_link::DeepLinkExt;

pub fn run() {
    let state = AppState::load(paths::settings_file());
    let hub =
        hub::HubClient::new(&state.get_settings().hub_url).expect("hub_url validado em settings");
    let auth = Arc::new(auth::Auth::new(
        hub,
        Arc::new(auth::KeyringVault::default()),
    ));

    let content = commands::content_store();
    // Última lista do Hub (memória + `servers-cache.json`): modo offline e endereços do ping.
    let servers = Arc::new(server_cache::ServerDirectory::new(
        paths::servers_cache_file(),
    ));
    let hub_url = state.get_settings().hub_url;

    tauri::Builder::default()
        // Precisa ser o primeiro plugin: a 2ª instância repassa os argumentos à 1ª e sai. Com o recurso
        // `deep-link`, o plugin entrega o `cbmp://` do argv ao `on_open_url` abaixo antes deste callback.
        .plugin(tauri_plugin_single_instance::init(|app, _argv, _cwd| {
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }))
        // Esquema `cbmp://`: só `cbmp://auth` é tratado (`auth::Auth::on_deep_link`); o resto é ignorado e registrado.
        .plugin(tauri_plugin_deep_link::init())
        .plugin(
            tauri_plugin_updater::Builder::new()
                .pubkey(update::pubkey())
                .build(),
        )
        // Só o seletor de pasta do jogo, chamado pelo Rust; nenhuma permissão `dialog:` é dada à UI.
        .plugin(tauri_plugin_dialog::init())
        .manage(state)
        // Uma loja de conteúdo só (índice e pastas de download coordenados por ela); usada pelo `play`.
        .manage(content.clone())
        .manage(auth.clone())
        .manage(servers)
        // Um play por vez (também trava "limpar cache", a faxina da abertura e a instalação de update).
        .manage(play::PlayGate::default())
        .setup(move |app| {
            // Ticket de uma execução anterior que o mod não leu.
            launch::remove_stale(&paths::run_dir());
            let handle = app.handle().clone();
            tauri::async_runtime::spawn(async move {
                // Sobras de downloads e arquivos do cache fora do índice, sob a trava do play (não corre junto
                // com o primeiro download).
                {
                    let gate = handle.state::<play::PlayGate>();
                    let _run = gate.begin().await;
                    let store = content.clone();
                    let _ = tokio::task::spawn_blocking(move || store.startup()).await;
                }
                // Jogo ainda não configurado: procura na Steam/Epic (fora da trava e do executor) e grava se achar.
                let h = handle.clone();
                let _ = tokio::task::spawn_blocking(move || {
                    let state = h.state::<AppState>();
                    if state.get_settings().game_dir.is_none()
                        && commands::resolve_game(&state).is_none()
                    {
                        log::info!("Hogwarts Legacy não encontrado automaticamente");
                    }
                })
                .await;
            });
            // Atualização do launcher: ao iniciar e a cada 6 h; só avisa a UI.
            update::spawn_periodic(app.handle().clone(), hub_url.clone());

            // Em dev o instalador não registrou o esquema: registra para o executável atual.
            #[cfg(debug_assertions)]
            if let Err(e) = app.deep_link().register_all() {
                log::warn!("registro do esquema cbmp em dev falhou: {e}");
            }
            let handle = app.handle().clone();
            app.deep_link().on_open_url(move |event| {
                for url in event.urls() {
                    commands::handle_deep_link(handle.clone(), auth.clone(), url.to_string());
                }
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::get_settings,
            commands::set_settings,
            commands::app_version,
            commands::login,
            commands::logout,
            commands::me,
            commands::pick_game_dir,
            commands::play,
            commands::cancel_play,
            commands::clear_cache,
            commands::check_update,
            commands::install_update,
            commands::list_servers,
            commands::cached_servers,
            commands::ping_servers
        ])
        .run(tauri::generate_context!())
        .expect("falha ao iniciar o CB Multiplayer");
}
