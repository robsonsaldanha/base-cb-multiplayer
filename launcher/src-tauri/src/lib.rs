//! CB Multiplayer — backend do launcher. Rede, disco, cofre e processos ficam aqui; a UI só chama os comandos
//! de `commands`.

pub mod commands;
pub mod error;
pub mod paths;
pub mod settings;
pub mod update;

use commands::AppState;
use settings::Settings;
use tauri::Manager;

pub fn run() {
    let settings_path = paths::settings_file();
    let settings = Settings::load(&settings_path);

    tauri::Builder::default()
        // Precisa ser o primeiro plugin: a 2ª instância repassa os argumentos (deep link) à 1ª e sai.
        .plugin(tauri_plugin_single_instance::init(|app, _argv, _cwd| {
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }))
        // Esquema `cbmp://`; o tratamento de `cbmp://auth` chega com o login (Task 5).
        .plugin(tauri_plugin_deep_link::init())
        .plugin(
            tauri_plugin_updater::Builder::new()
                .pubkey(update::pubkey())
                .build(),
        )
        // Só o seletor de pasta do jogo, chamado pelo Rust; nenhuma permissão `dialog:` é dada à UI.
        .plugin(tauri_plugin_dialog::init())
        .manage(AppState::new(settings_path, settings))
        .invoke_handler(tauri::generate_handler![
            commands::get_settings,
            commands::set_settings,
            commands::app_version
        ])
        .run(tauri::generate_context!())
        .expect("falha ao iniciar o CB Multiplayer");
}
