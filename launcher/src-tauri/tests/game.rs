//! Localizar o Hogwarts Legacy: árvores falsas de Steam/Epic e registro simulado (trait `Registry`).

use cbmp_launcher_lib::game::{
    self, locate, parse_vdf, steam_libraries, validate_game_dir, Hive, Registry, Roots, Store,
};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[derive(Default)]
struct FakeRegistry(HashMap<(Hive, String, String), String>);

impl FakeRegistry {
    fn with(mut self, hive: Hive, key: &str, value: &str, data: &str) -> Self {
        self.0
            .insert((hive, key.to_string(), value.to_string()), data.to_string());
        self
    }
}

impl Registry for FakeRegistry {
    fn read_string(&self, hive: Hive, key: &str, value: &str) -> Option<String> {
        self.0
            .get(&(hive, key.to_string(), value.to_string()))
            .cloned()
    }
}

/// Pasta com a estrutura que o validador espera.
fn fake_game(dir: &Path) {
    std::fs::create_dir_all(dir.join("Phoenix").join("Binaries").join("Win64")).unwrap();
    std::fs::write(dir.join("HogwartsLegacy.exe"), b"MZ").unwrap();
}

/// Caminho como o Steam grava no `.vdf` (barras invertidas escapadas).
fn vdf_path(p: &Path) -> String {
    p.display().to_string().replace('\\', "\\\\")
}

fn write_library_folders(steam: &Path, libs: &[&Path]) {
    let mut s = String::from("\"libraryfolders\"\n{\n");
    for (i, lib) in libs.iter().enumerate() {
        s.push_str(&format!(
            "\t\"{i}\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t\t\"label\"\t\t\"\"\n\t\t\"apps\"\n\t\t{{\n\t\t\t\"228980\"\t\t\"0\"\n\t\t}}\n\t}}\n",
            vdf_path(lib)
        ));
    }
    s.push_str("}\n");
    let dir = steam.join("steamapps");
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join("libraryfolders.vdf"), s).unwrap();
}

fn write_app_manifest(lib: &Path, installdir: &str) {
    let dir = lib.join("steamapps");
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(
        dir.join("appmanifest_990080.acf"),
        format!(
            "\"AppState\"\n{{\n\t\"appid\"\t\t\"990080\"\n\t\"name\"\t\t\"Hogwarts Legacy\"\n\t\"installdir\"\t\t\"{installdir}\"\n}}\n"
        ),
    )
    .unwrap();
}

fn no_epic(tmp: &Path) -> Roots {
    Roots {
        epic_manifests: tmp.join("sem-epic"),
    }
}

#[test]
fn steam_com_duas_bibliotecas_acha_o_jogo_na_segunda() {
    let tmp = tempfile::tempdir().unwrap();
    let steam = tmp.path().join("Steam");
    let lib2 = tmp.path().join("SteamLibrary");
    write_library_folders(&steam, &[&steam, &lib2]);
    // Outro jogo na primeira biblioteca; o Hogwarts Legacy na segunda.
    let game_dir = lib2
        .join("steamapps")
        .join("common")
        .join("Hogwarts Legacy");
    fake_game(&game_dir);
    write_app_manifest(&lib2, "Hogwarts Legacy");

    // O Steam grava `SteamPath` com barras normais e minúsculas.
    let reg = FakeRegistry::default().with(
        Hive::CurrentUser,
        r"Software\Valve\Steam",
        "SteamPath",
        &steam.display().to_string().replace('\\', "/"),
    );
    let found = locate(&reg, &no_epic(tmp.path())).expect("jogo da Steam");
    assert_eq!(found.store, Store::Steam);
    assert_eq!(found.dir, game_dir);
    assert_eq!(found.exe, game_dir.join("HogwartsLegacy.exe"));
}

#[test]
fn steam_pelo_install_path_do_hklm() {
    let tmp = tempfile::tempdir().unwrap();
    let steam = tmp.path().join("Steam");
    write_library_folders(&steam, &[&steam]);
    let game_dir = steam.join("steamapps").join("common").join("HL");
    fake_game(&game_dir);
    write_app_manifest(&steam, "HL");
    let reg = FakeRegistry::default().with(
        Hive::LocalMachine,
        r"SOFTWARE\WOW6432Node\Valve\Steam",
        "InstallPath",
        &steam.display().to_string(),
    );
    let found = locate(&reg, &no_epic(tmp.path())).unwrap();
    assert_eq!((found.store, found.dir), (Store::Steam, game_dir));
}

#[test]
fn steam_com_installdir_malicioso_ou_pasta_incompleta_e_ignorado() {
    let tmp = tempfile::tempdir().unwrap();
    let steam = tmp.path().join("Steam");
    write_library_folders(&steam, &[&steam]);
    // `installdir` que sai de `common`: nunca seguido, mesmo que o destino pareça o jogo.
    fake_game(&steam.join("steamapps").join("fora"));
    write_app_manifest(&steam, "..\\\\fora");
    let reg = FakeRegistry::default().with(
        Hive::CurrentUser,
        r"Software\Valve\Steam",
        "SteamPath",
        &steam.display().to_string(),
    );
    assert_eq!(locate(&reg, &no_epic(tmp.path())), None);

    // Manifest certo, mas a pasta não tem a estrutura do jogo.
    write_app_manifest(&steam, "Hogwarts Legacy");
    std::fs::create_dir_all(
        steam
            .join("steamapps")
            .join("common")
            .join("Hogwarts Legacy"),
    )
    .unwrap();
    assert_eq!(locate(&reg, &no_epic(tmp.path())), None);
}

fn write_item(dir: &Path, name: &str, json: serde_json::Value) {
    std::fs::create_dir_all(dir).unwrap();
    std::fs::write(dir.join(name), serde_json::to_vec_pretty(&json).unwrap()).unwrap();
}

#[test]
fn epic_pelos_manifests_item() {
    let tmp = tempfile::tempdir().unwrap();
    let manifests = tmp.path().join("Manifests");
    let game_dir = tmp.path().join("Epic Games").join("HogwartsLegacy");
    fake_game(&game_dir);
    write_item(
        &manifests,
        "AAA.item",
        serde_json::json!({ "DisplayName": "Fortnite", "AppName": "Fortnite", "InstallLocation": tmp.path().join("fn") }),
    );
    std::fs::write(manifests.join("quebrado.item"), b"{ nao e json").unwrap();
    std::fs::write(manifests.join("outro.txt"), b"ignorado").unwrap();
    write_item(
        &manifests,
        "BBB.item",
        serde_json::json!({ "DisplayName": "Hogwarts Legacy", "AppName": "fa1234", "InstallLocation": game_dir }),
    );
    let roots = Roots {
        epic_manifests: manifests,
    };
    let found = locate(&FakeRegistry::default(), &roots).expect("jogo da Epic");
    assert_eq!(found.store, Store::Epic);
    assert_eq!(found.dir, game_dir);
}

#[test]
fn epic_com_pasta_que_nao_e_o_jogo_e_ignorado() {
    let tmp = tempfile::tempdir().unwrap();
    let manifests = tmp.path().join("Manifests");
    write_item(
        &manifests,
        "x.item",
        serde_json::json!({ "DisplayName": "Hogwarts Legacy", "InstallLocation": tmp.path().join("vazio") }),
    );
    let roots = Roots {
        epic_manifests: manifests,
    };
    assert_eq!(locate(&FakeRegistry::default(), &roots), None);
}

#[test]
fn nenhum_lugar_devolve_none() {
    let tmp = tempfile::tempdir().unwrap();
    assert_eq!(locate(&FakeRegistry::default(), &no_epic(tmp.path())), None);
    // Steam registrado mas sem `libraryfolders.vdf` nem o app.
    let reg = FakeRegistry::default().with(
        Hive::CurrentUser,
        r"Software\Valve\Steam",
        "SteamPath",
        &tmp.path().join("nada").display().to_string(),
    );
    assert_eq!(locate(&reg, &no_epic(tmp.path())), None);
}

#[test]
fn pasta_manual_valida_e_invalida() {
    let tmp = tempfile::tempdir().unwrap();
    let ok = tmp.path().join("HL");
    fake_game(&ok);
    let g = validate_game_dir(&ok).unwrap();
    assert_eq!(g.store, Store::Manual);
    assert_eq!(g.exe, ok.join("HogwartsLegacy.exe"));

    let sem_exe = tmp.path().join("sem_exe");
    std::fs::create_dir_all(sem_exe.join("Phoenix").join("Binaries").join("Win64")).unwrap();
    let sem_phoenix = tmp.path().join("sem_phoenix");
    std::fs::create_dir_all(&sem_phoenix).unwrap();
    std::fs::write(sem_phoenix.join("HogwartsLegacy.exe"), b"MZ").unwrap();
    // Executável que é pasta.
    let exe_pasta = tmp.path().join("exe_pasta");
    std::fs::create_dir_all(exe_pasta.join("HogwartsLegacy.exe")).unwrap();
    std::fs::create_dir_all(exe_pasta.join("Phoenix").join("Binaries").join("Win64")).unwrap();
    for bad in [
        sem_exe,
        sem_phoenix,
        exe_pasta,
        tmp.path().join("nao_existe"),
        PathBuf::from("relativo"),
    ] {
        let e = validate_game_dir(&bad).unwrap_err();
        assert_eq!(e.code, "game_invalid", "{}", bad.display());
        assert!(!e.message.is_empty());
    }
}

#[test]
fn vdf_com_escapes_e_formato_antigo() {
    let v = parse_vdf(
        "\"libraryfolders\"\n{\n\t\"contentstatsid\"\t\"123\"\n\t\"1\"\t\t\"D:\\\\Games\\\\Steam\"\n\t\"2\"\n\t{\n\t\t\"path\"\t\t\"E:\\\\Lib \\\"x\\\"\"\n\t}\n}\n",
    )
    .expect("vdf válido");
    let tmp = tempfile::tempdir().unwrap();
    let steam = tmp.path().join("Steam");
    std::fs::create_dir_all(steam.join("steamapps")).unwrap();
    std::fs::write(
        steam.join("steamapps").join("libraryfolders.vdf"),
        "\"libraryfolders\"\n{\n\t\"1\"\t\t\"D:\\\\Games\\\\Steam\"\n\t\"2\"\n\t{\n\t\t\"path\"\t\t\"E:\\\\Lib\"\n\t}\n}\n",
    )
    .unwrap();
    let libs = steam_libraries(&steam);
    assert_eq!(
        libs,
        vec![
            steam.clone(),
            PathBuf::from("D:\\Games\\Steam"),
            PathBuf::from("E:\\Lib")
        ]
    );
    assert!(
        game::vdf_get(&v, &["libraryfolders", "2", "path"]).is_some_and(|p| p == "E:\\Lib \"x\"")
    );
    assert_eq!(
        game::vdf_get(&v, &["libraryfolders", "1"]).as_deref(),
        Some("D:\\Games\\Steam")
    );
    // Texto quebrado não entra em pânico.
    for bad in ["\"a\" {", "\"a\" \"b", "}", "{", "\"a\""] {
        let _ = parse_vdf(bad);
    }
}
