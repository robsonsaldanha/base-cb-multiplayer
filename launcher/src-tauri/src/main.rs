// Sem console no build de release.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    // Modo elevado (`--mod-admin-setup` como primeiro argumento, pedido pelo próprio launcher pelo UAC): faz só a
    // instalação do carregador na pasta do jogo e sai, antes de qualquer coisa do Tauri (sem janela, sem instância
    // única, sem deep link, sem rede, sem log em arquivo).
    if let Some(code) = cbmp_launcher_lib::admin_setup::entry(std::env::args_os()) {
        std::process::exit(code);
    }
    cbmp_launcher_lib::run();
}
