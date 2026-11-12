// Sem console no build de release.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    cbmp_launcher_lib::run();
}
