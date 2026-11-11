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
    // ... mas vira sugestão para o usuário confirmar, dizendo o que falhou.
    assert_eq!(
        game::detect(&FakeRegistry::default(), &roots),
        game::Detection::Candidate(game::GameCandidate {
            dir: game_dir.clone(),
            store: Store::Epic,
            failed: vec![game::TrustCheck::GameDir],
        })
    );
    icacls(&game_dir, &["/remove:g", "*S-1-1-0"]);
    assert!(game::SystemTrust.trusted(&game_dir));
    // Idem no próprio `.item`.
    icacls(&item, &["/grant", "*S-1-5-32-545:(M)"]);
    assert_eq!(locate(&FakeRegistry::default(), &roots), None);
    assert!(matches!(
        game::detect(&FakeRegistry::default(), &roots),
        game::Detection::Candidate(c) if c.failed == vec![game::TrustCheck::Manifest] && c.dir == game_dir
    ));
}

// ----- Epic não confiável vira sugestão (nunca usada sozinha) -----

use cbmp_launcher_lib::commands::{adopt_detected, detect_game_with, AppState};
use game::{Detection, GameCandidate, TrustCheck};
use std::cell::Cell;

fn candidate(dir: &Path, failed: Vec<TrustCheck>) -> Detection {
    Detection::Candidate(GameCandidate {
        dir: dir.to_path_buf(),
        store: Store::Epic,
        failed,
    })
}

#[test]
fn deteccao_epic_confiavel_e_usada_direto() {
    let tmp = tempfile::tempdir().unwrap();
    let (roots, _, game_dir) = epic_setup(tmp.path());
    match game::detect_with(&FakeRegistry::default(), &roots, &Distrust(vec![])) {
        Detection::Trusted(g) => assert_eq!((g.store, g.dir), (Store::Epic, game_dir)),
        other => panic!("esperava instalação confiável: {other:?}"),
    }
}

#[test]
fn deteccao_epic_manifest_nao_confiavel_vira_candidata() {
    let tmp = tempfile::tempdir().unwrap();
    let (roots, item, game_dir) = epic_setup(tmp.path());
    let reg = FakeRegistry::default();
    let d = game::detect_with(&reg, &roots, &Distrust(vec![item.clone()]));
    assert_eq!(d, candidate(&game_dir, vec![TrustCheck::Manifest]));
    // E nunca vira instalação utilizável sozinha.
    assert_eq!(game::locate_with(&reg, &roots, &Distrust(vec![item])), None);
}

#[test]
fn deteccao_epic_pasta_nao_confiavel_vira_candidata_com_as_verificacoes_que_falharam() {
    let tmp = tempfile::tempdir().unwrap();
    let (roots, item, game_dir) = epic_setup(tmp.path());
    let exe = game_dir.join("HogwartsLegacy.exe");
    let bins = game_dir.join("Phoenix").join("Binaries").join("Win64");
    let reg = FakeRegistry::default();
    for (distrust, failed) in [
        (vec![game_dir.clone()], vec![TrustCheck::GameDir]),
        (vec![exe.clone()], vec![TrustCheck::Exe]),
        (vec![bins.clone()], vec![TrustCheck::Binaries]),
        // Caso real provável da Epic: tudo gravável pelo grupo Usuários.
        (
            vec![item, game_dir.clone(), exe, bins],
            vec![
                TrustCheck::Manifest,
                TrustCheck::GameDir,
                TrustCheck::Exe,
                TrustCheck::Binaries,
            ],
        ),
    ] {
        assert_eq!(
            game::detect_with(&reg, &roots, &Distrust(distrust.clone())),
            candidate(&game_dir, failed)
        );
        assert_eq!(game::locate_with(&reg, &roots, &Distrust(distrust)), None);
    }
}

#[test]
fn deteccao_epic_confiavel_vence_candidata_anterior() {
    let tmp = tempfile::tempdir().unwrap();
    let manifests = tmp.path().join("Manifests");
    let plantado = tmp.path().join("plantado").join("HL");
    let real = tmp.path().join("Epic Games").join("HogwartsLegacy");
    fake_game(&plantado);
    fake_game(&real);
    // `A.item` vem antes na ordem, mas não é confiável.
    write_item(
        &manifests,
        "A.item",
        serde_json::json!({ "DisplayName": "Hogwarts Legacy", "InstallLocation": plantado }),
    );
    write_item(
        &manifests,
        "B.item",
        serde_json::json!({ "DisplayName": "Hogwarts Legacy", "InstallLocation": real }),
    );
    let roots = Roots {
        epic_manifests: manifests.clone(),
    };
    let d = game::detect_with(
        &FakeRegistry::default(),
        &roots,
        &Distrust(vec![manifests.join("A.item")]),
    );
    assert!(
        matches!(&d, Detection::Trusted(g) if g.dir == real),
        "{d:?}"
    );
}

#[test]
fn deteccao_sem_instalacao_e_not_found() {
    let tmp = tempfile::tempdir().unwrap();
    let reg = FakeRegistry::default();
    assert_eq!(
        game::detect_with(&reg, &no_epic(tmp.path()), &Distrust(vec![])),
        Detection::NotFound
    );
    // Manifest não confiável de outro jogo, e um do Hogwarts Legacy apontando para pasta incompleta: nada.
    let manifests = tmp.path().join("Manifests");
    write_item(
        &manifests,
        "outro.item",
        serde_json::json!({ "DisplayName": "Outro Jogo", "InstallLocation": tmp.path() }),
    );
    write_item(
        &manifests,
        "hl.item",
        serde_json::json!({ "DisplayName": "Hogwarts Legacy", "InstallLocation": tmp.path().join("vazio") }),
    );
    let roots = Roots {
        epic_manifests: manifests.clone(),
    };
    let all = Distrust(vec![
        manifests.join("outro.item"),
        manifests.join("hl.item"),
    ]);
    assert_eq!(game::detect_with(&reg, &roots, &all), Detection::NotFound);
}

#[test]
fn candidata_para_a_ui_so_tem_pasta_e_loja() {
    let v = serde_json::to_value(GameCandidate {
        dir: PathBuf::from("D:\\Epic Games\\HogwartsLegacy"),
        store: Store::Epic,
        failed: vec![TrustCheck::Manifest],
    })
    .unwrap();
    assert_eq!(
        v,
        serde_json::json!({ "dir": "D:\\Epic Games\\HogwartsLegacy", "store": "epic" })
    );
}

// ----- comandos: detectar e confirmar a sugestão -----

fn state(tmp: &Path) -> AppState {
    AppState::load(tmp.join("dados").join("settings.json"))
}

#[test]
fn detect_game_sugere_candidata_sem_gravar_e_grava_a_confiavel() {
    let tmp = tempfile::tempdir().unwrap();
    let (roots, item, game_dir) = epic_setup(tmp.path());
    let st = state(tmp.path());
    let reg = FakeRegistry::default();

    let r = detect_game_with(&st, || {
        game::detect_with(&reg, &roots, &Distrust(vec![item.clone()]))
    });
    assert_eq!(r.candidate.map(|c| c.dir), Some(game_dir.clone()));
    assert_eq!(
        r.settings.game_dir, None,
        "candidata nunca é gravada sozinha"
    );
    assert_eq!(st.get_settings().game_dir, None);

    let r = detect_game_with(&st, || game::detect_with(&reg, &roots, &Distrust(vec![])));
    assert_eq!(r.candidate, None);
    assert_eq!(r.settings.game_dir, Some(game_dir.clone()));
    // Com pasta válida gravada, não procura de novo.
    let r = detect_game_with(&st, || panic!("não devia procurar"));
    assert_eq!(r.settings.game_dir, Some(game_dir));
}

#[test]
fn confirmar_candidata_grava_como_o_seletor() {
    let tmp = tempfile::tempdir().unwrap();
    let (_, _, game_dir) = epic_setup(tmp.path());
    let st = state(tmp.path());
    let asked = Cell::new(0);
    let s = adopt_detected(
        &st,
        &game_dir,
        candidate(&game_dir, vec![TrustCheck::Manifest]),
        |c| {
            asked.set(asked.get() + 1);
            assert_eq!(c.dir, game_dir);
            true
        },
    )
    .unwrap();
    assert_eq!(
        asked.get(),
        1,
        "diálogo nativo sempre aparece para a candidata"
    );
    assert_eq!(s.game_dir, Some(game_dir.clone()));
    assert_eq!(st.get_settings().game_dir, Some(game_dir.clone()));
    // Gravado no arquivo, e o Jogar usa a pasta como uma escolhida à mão.
    let saved = cbmp_launcher_lib::settings::Settings::load(
        &tmp.path().join("dados").join("settings.json"),
    )
    .unwrap();
    assert_eq!(saved.game_dir, Some(game_dir.clone()));
    let g = cbmp_launcher_lib::commands::resolve_game(&st).unwrap();
    assert_eq!((g.store, g.dir), (Store::Manual, game_dir));
}

#[test]
fn recusar_no_dialogo_nao_muda_nada() {
    let tmp = tempfile::tempdir().unwrap();
    let (_, _, game_dir) = epic_setup(tmp.path());
    let st = state(tmp.path());
    let s = adopt_detected(&st, &game_dir, candidate(&game_dir, vec![]), |_| false).unwrap();
    assert_eq!(s.game_dir, None);
    assert_eq!(st.get_settings().game_dir, None);
}

#[test]
fn confirmacao_so_aceita_a_pasta_que_a_procura_achou() {
    let tmp = tempfile::tempdir().unwrap();
    let (_, _, game_dir) = epic_setup(tmp.path());
    // Outra pasta válida do jogo, que a UI tentaria impor.
    let outra = tmp.path().join("Outra").join("HL");
    fake_game(&outra);
    let st = state(tmp.path());
    let never = |_: &GameCandidate| -> bool { panic!("diálogo não pode abrir") };
    for (shown, detection) in [
        // UI pede uma pasta diferente da achada.
        (
            outra.clone(),
            candidate(&game_dir, vec![TrustCheck::Manifest]),
        ),
        // A procura nova já não acha nada.
        (game_dir.clone(), Detection::NotFound),
        // A procura nova acha outra pasta (instalação confiável diferente).
        (
            game_dir.clone(),
            Detection::Trusted(validate_game_dir(&outra).unwrap()),
        ),
        // Caminho relativo / vazio.
        (PathBuf::from(""), candidate(&game_dir, vec![])),
        (
            PathBuf::from("HogwartsLegacy"),
            candidate(&game_dir, vec![]),
        ),
    ] {
        let e = adopt_detected(&st, &shown, detection, never).unwrap_err();
        assert_eq!(e.code, "game_candidate_changed", "{}", shown.display());
        assert_eq!(st.get_settings().game_dir, None);
    }
}

#[test]
fn confirmacao_revalida_a_pasta() {
    let tmp = tempfile::tempdir().unwrap();
    let (_, _, game_dir) = epic_setup(tmp.path());
    let st = state(tmp.path());
    // A pasta deixou de ser o jogo entre a procura e a gravação.
    std::fs::remove_file(game_dir.join("HogwartsLegacy.exe")).unwrap();
    let e = adopt_detected(&st, &game_dir, candidate(&game_dir, vec![]), |_| true).unwrap_err();
    assert_eq!(e.code, "game_invalid");
    assert_eq!(st.get_settings().game_dir, None);
}

#[test]
fn confirmacao_recusa_instalacao_confiavel() {
    // Instalação confiável o Rust grava sozinho (`detect_game`/Jogar); por aqui a UI não troca a pasta configurada.
    let tmp = tempfile::tempdir().unwrap();
    let (_, _, game_dir) = epic_setup(tmp.path());
    let st = state(tmp.path());
    let g = validate_game_dir(&game_dir).unwrap();
    let e = adopt_detected(&st, &game_dir, Detection::Trusted(g), |_| {
        panic!("diálogo não pode abrir")
    })
    .unwrap_err();
    assert_eq!(e.code, "game_candidate_changed");
    assert_eq!(st.get_settings().game_dir, None);
}

#[cfg(windows)]
#[test]
fn confirmacao_compara_caminho_sem_diferenciar_maiusculas() {
    let tmp = tempfile::tempdir().unwrap();
    let (_, _, game_dir) = epic_setup(tmp.path());
    let st = state(tmp.path());
    let shown = PathBuf::from(format!(
        "{}\\",
        game_dir.display().to_string().to_uppercase()
    ));
    let s = adopt_detected(&st, &shown, candidate(&game_dir, vec![]), |_| true).unwrap();
    // Grava o caminho achado pelo Rust, não o texto da UI.
    assert_eq!(s.game_dir, Some(game_dir));
}

// ----- várias pastas não confiáveis: nenhuma sugestão (fix round 1) -----

/// Manifests da Epic apontando para `dirs` (cada um com a estrutura do jogo), todos não confiáveis.
fn epic_untrusted(tmp: &Path, dirs: &[&str]) -> (Roots, Vec<PathBuf>, Distrust) {
    let manifests = tmp.join("Manifests");
    let mut games = Vec::new();
    let mut items = Vec::new();
    for (i, d) in dirs.iter().enumerate() {
        let dir = tmp.join(d);
        fake_game(&dir);
        let name = format!("{i:02}.item");
        write_item(
            &manifests,
            &name,
            serde_json::json!({ "DisplayName": "Hogwarts Legacy", "InstallLocation": dir }),
        );
        items.push(manifests.join(name));
        games.push(dir);
    }
    (
        Roots {
            epic_manifests: manifests,
        },
        games,
        Distrust(items),
    )
}

#[test]
fn uma_pasta_nao_confiavel_vira_sugestao() {
    let tmp = tempfile::tempdir().unwrap();
    // Dois manifests para a MESMA pasta continuam sendo uma pasta só.
    let (roots, games, distrust) = epic_untrusted(tmp.path(), &["Epic/HL", "Epic/HL"]);
    let d = game::detect_with(&FakeRegistry::default(), &roots, &distrust);
    assert!(
        matches!(&d, Detection::Candidate(c) if c.dir == games[0]),
        "{d:?}"
    );
    let st = state(tmp.path());
    let r = detect_game_with(&st, || d.clone());
    assert_eq!(r.candidate.map(|c| c.dir), Some(games[0].clone()));
    assert!(!r.ambiguous);
}

#[test]
fn duas_pastas_nao_confiaveis_nao_viram_sugestao() {
    let tmp = tempfile::tempdir().unwrap();
    // Um `.item` plantado (nome que vem antes) e o verdadeiro: nenhum é sugerido.
    let (roots, _, distrust) =
        epic_untrusted(tmp.path(), &["Plantado/HL", "Epic Games/HogwartsLegacy"]);
    let d = game::detect_with(&FakeRegistry::default(), &roots, &distrust);
    assert_eq!(
        d,
        Detection::Ambiguous {
            candidates: 2,
            flooded: false
        }
    );
    assert_eq!(
        game::locate_with(&FakeRegistry::default(), &roots, &distrust),
        None
    );
    let st = state(tmp.path());
    let r = detect_game_with(&st, || d.clone());
    assert_eq!(r.candidate, None);
    assert!(r.ambiguous);
    assert_eq!(r.settings.game_dir, None);
    assert_eq!(
        serde_json::to_value(&r).unwrap()["ambiguous"],
        serde_json::json!(true)
    );
}

#[test]
fn instalacao_confiavel_vence_varias_plantadas() {
    let tmp = tempfile::tempdir().unwrap();
    let (roots, games, mut distrust) =
        epic_untrusted(tmp.path(), &["A/HL", "B/HL", "Epic Games/HogwartsLegacy"]);
    // O terceiro manifest (e a pasta dele) é confiável.
    distrust.0.pop();
    let d = game::detect_with(&FakeRegistry::default(), &roots, &distrust);
    assert!(
        matches!(&d, Detection::Trusted(g) if g.dir == games[2]),
        "{d:?}"
    );
}

#[test]
fn manifests_demais_nao_viram_sugestao() {
    let tmp = tempfile::tempdir().unwrap();
    let (roots, _, _) = epic_untrusted(tmp.path(), &["Epic Games/HogwartsLegacy"]);
    // Enche a pasta de manifests (de outro jogo) além do teto: o verdadeiro pode ter ficado de fora.
    for i in 0..game::MAX_EPIC_ITEMS {
        std::fs::write(roots.epic_manifests.join(format!("lixo{i:05}.item")), b"{}").unwrap();
    }
    let all_untrusted = AllUntrusted;
    let d = game::detect_with(&FakeRegistry::default(), &roots, &all_untrusted);
    assert!(
        matches!(d, Detection::Ambiguous { flooded: true, .. }),
        "{d:?}"
    );
    let st = state(tmp.path());
    let r = detect_game_with(&st, || d.clone());
    assert_eq!((r.candidate, r.ambiguous), (None, true));
}

/// Nada é confiável (como numa Epic real com tudo gravável por Usuários).
struct AllUntrusted;

impl game::Trust for AllUntrusted {
    fn trusted(&self, _path: &Path) -> bool {
        false
    }
}

#[test]
fn confirmacao_recusa_se_ficou_ambiguo_depois_da_sugestao() {
    let tmp = tempfile::tempdir().unwrap();
    let (roots, games, distrust) = epic_untrusted(tmp.path(), &["Epic Games/HogwartsLegacy"]);
    let st = state(tmp.path());
    // Sugerida quando havia uma pasta só...
    let r = detect_game_with(&st, || {
        game::detect_with(&FakeRegistry::default(), &roots, &distrust)
    });
    let shown = r.candidate.expect("sugestão").dir;
    assert_eq!(shown, games[0]);
    // ... e, antes do clique, outra conta planta um segundo `.item`.
    let planted = tmp.path().join("Plantado").join("HL");
    fake_game(&planted);
    write_item(
        &roots.epic_manifests,
        "00a.item",
        serde_json::json!({ "DisplayName": "Hogwarts Legacy", "InstallLocation": planted }),
    );
    let mut distrust = distrust;
    distrust.0.push(roots.epic_manifests.join("00a.item"));
    let fresh = game::detect_with(&FakeRegistry::default(), &roots, &distrust);
    let e = adopt_detected(&st, &shown, fresh, |_| panic!("diálogo não pode abrir")).unwrap_err();
    assert_eq!(e.code, "game_ambiguous");
    assert_eq!(st.get_settings().game_dir, None);
}

#[test]
fn caminho_com_caracteres_de_disfarce_e_ignorado() {
    let tmp = tempfile::tempdir().unwrap();
    let manifests = tmp.path().join("Manifests");
    // U+202E (direita para a esquerda) num nome de pasta que o NTFS aceita.
    let dir = tmp.path().join("Epic Games\u{202E}exe.HL");
    fake_game(&dir);
    write_item(
        &manifests,
        "x.item",
        serde_json::json!({ "DisplayName": "Hogwarts Legacy", "InstallLocation": dir }),
    );
    let roots = Roots {
        epic_manifests: manifests,
    };
    assert_eq!(
        game::detect_with(&FakeRegistry::default(), &roots, &AllUntrusted),
        Detection::NotFound
    );
}

#[test]
fn item_grande_demais_e_ignorado() {
    let tmp = tempfile::tempdir().unwrap();
    let (roots, item, _) = epic_setup(tmp.path());
    // Manifest válido, mas acima do teto de leitura (1 MB): nem é interpretado.
    let mut big = std::fs::read(&item).unwrap();
    big.extend(std::iter::repeat_n(b' ', 1024 * 1024));
    std::fs::write(&item, big).unwrap();
    assert_eq!(
        game::detect_with(&FakeRegistry::default(), &roots, &AllUntrusted),
        Detection::NotFound
    );
}

#[test]
fn texto_do_dialogo_nao_atribui_a_pasta_a_ninguem() {
    let c = GameCandidate {
        dir: PathBuf::from("D:\\Jogos\\HL"),
        store: Store::Epic,
        failed: vec![TrustCheck::Manifest],
    };
    let t = cbmp_launcher_lib::commands::confirm_text(&c);
    assert!(t.contains("D:\\Jogos\\HL"), "{t}");
    assert!(t.contains("parece ser do Hogwarts Legacy"), "{t}");
    assert!(t.contains("não conseguiu verificar"), "{t}");
    assert!(t.contains("só se foi nela que você instalou o jogo"), "{t}");
    assert!(!t.contains("Epic"), "não pode dizer de onde veio: {t}");
}

#[test]
fn uma_confirmacao_por_vez() {
    let gate = cbmp_launcher_lib::commands::ConfirmGate::new();
    let open = gate.try_begin().unwrap();
    assert_eq!(
        gate.try_begin().err().map(|e| e.code),
        Some("game_confirm_open")
    );
    drop(open);
    assert!(gate.try_begin().is_ok());
}
