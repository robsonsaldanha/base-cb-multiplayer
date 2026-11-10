//! CB Multiplayer — backend do launcher. Rede, disco, cofre e processos ficam aqui; a UI só chama os comandos
//! de `commands`.

pub mod auth;
pub mod commands;
pub mod content;
pub mod error;
pub mod hub;
pub mod paths;
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
    {
        // Sobras de downloads e arquivos do cache fora do índice (queda no meio de uma gravação).
        let content = content.clone();
        std::thread::spawn(move || content.startup());
    }

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
        // Uma loja de conteúdo só (índice e pastas de download coordenados por ela); usada pelo `play` (Task 7).
        .manage(content.clone())
        .manage(auth.clone())
        .setup(move |app| {
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
            commands::me
        ])
        .run(tauri::generate_context!())
        .expect("falha ao iniciar o CB Multiplayer");
}
