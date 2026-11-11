fn main() {
    // Lista explícita dos comandos do app: cada um vira a permissão `allow-<comando>`, liberada só em
    // `capabilities/default.json`. Comando novo sem permissão na capability fica inacessível à UI.
    let manifest = tauri_build::AppManifest::new().commands(&[
        "get_settings",
        "set_settings",
        "app_version",
        "login",
        "logout",
        "me",
        "pick_game_dir",
        "detect_game",
        "use_detected_game",
        "play",
        "cancel_play",
        "clear_cache",
        "check_update",
        "install_update",
        "list_servers",
        "cached_servers",
        "ping_servers",
    ]);
    tauri_build::try_build(tauri_build::Attributes::new().app_manifest(manifest))
        .expect("falha no tauri-build");
}
