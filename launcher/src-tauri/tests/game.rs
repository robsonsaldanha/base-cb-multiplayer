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
    // Confiança por dono/DACL é testada à parte (a pasta temporária pode ter ACL de outras contas).
    let found = game::locate_with(&FakeRegistry::default(), &roots, &Distrust(vec![]))
        .expect("jogo da Epic");
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

// ----- confiança (dono/permissões) dos caminhos da Epic -----

/// Recusa os caminhos listados (dono "outro usuário" simulado).
struct Distrust(Vec<PathBuf>);

impl game::Trust for Distrust {
    fn trusted(&self, path: &Path) -> bool {
        !self.0.iter().any(|p| p == path)
    }
}

fn epic_setup(tmp: &Path) -> (Roots, PathBuf, PathBuf) {
    let manifests = tmp.join("Manifests");
    let game_dir = tmp.join("Epic Games").join("HogwartsLegacy");
    fake_game(&game_dir);
    write_item(
        &manifests,
        "HL.item",
        serde_json::json!({ "DisplayName": "Hogwarts Legacy", "InstallLocation": game_dir }),
    );
    let item = manifests.join("HL.item");
    (
        Roots {
            epic_manifests: manifests,
        },
        item,
        game_dir,
    )
}

#[test]
fn epic_plantado_por_outro_usuario_e_ignorado() {
    let tmp = tempfile::tempdir().unwrap();
    let (roots, item, game_dir) = epic_setup(tmp.path());
    let reg = FakeRegistry::default();
    let ok = game::locate_with(&reg, &roots, &Distrust(vec![])).unwrap();
    assert_eq!((ok.store, &ok.dir), (Store::Epic, &game_dir));
    let bins = game_dir.join("Phoenix").join("Binaries").join("Win64");
    for bad in [
        item,
        game_dir.clone(),
        game_dir.join("HogwartsLegacy.exe"),
        bins,
    ] {
        assert_eq!(
            game::locate_with(&reg, &roots, &Distrust(vec![bad.clone()])),
            None,
            "{}",
            bad.display()
        );
    }
}

#[cfg(windows)]
fn icacls(path: &Path, args: &[&str]) {
    let st = std::process::Command::new("icacls")
        .arg(path)
        .args(args)
        .stdout(std::process::Stdio::null())
        .status()
        .unwrap();
    assert!(st.success());
}

#[cfg(windows)]
#[test]
fn confianca_real_pelo_dono_e_pela_dacl() {
    use game::Trust;
    let tmp = tempfile::tempdir().unwrap();
    // Pasta só deste usuário (a temporária pode herdar entradas de outras contas, como num PC compartilhado).
    cbmp_launcher_lib::launch::acl::restrict_dir_to_current_user(tmp.path()).unwrap();
    let (roots, item, game_dir) = epic_setup(tmp.path());
    // Criados por este usuário numa pasta só dele: confiáveis.
    assert!(game::SystemTrust.trusted(&item));
    assert_eq!(
        locate(&FakeRegistry::default(), &roots).map(|g| g.dir),
        Some(game_dir.clone())
    );
    // "Todos" com escrita na pasta do jogo: qualquer conta poderia trocar o executável → ignorado.
    icacls(&game_dir, &["/grant", "*S-1-1-0:(W)"]);
    assert!(!game::SystemTrust.trusted(&game_dir));
    assert_eq!(locate(&FakeRegistry::default(), &roots), None);
    icacls(&game_dir, &["/remove:g", "*S-1-1-0"]);
    assert!(game::SystemTrust.trusted(&game_dir));
    // Idem no próprio `.item`.
    icacls(&item, &["/grant", "*S-1-5-32-545:(M)"]);
    assert_eq!(locate(&FakeRegistry::default(), &roots), None);
}
