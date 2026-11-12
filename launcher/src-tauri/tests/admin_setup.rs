//! Modo elevado `--mod-admin-setup` sem elevar de verdade: argumentos, validação, o trabalho elevado numa pasta de jogo
//! falsa (`tempfile`, pacote assinado por um par gerado aqui) e o comando com o UAC trocado pelo próprio `run`. Nenhum
//! teste chama `request` (que abriria o UAC).

use cbmp_launcher_lib::admin_setup::{
    entry, exit_code, hold_path, parse_args, run, to_command_line, validate, wants_admin_setup,
    AdminArgs, FLAG,
};
use cbmp_launcher_lib::commands::mod_admin_setup_with;
use cbmp_launcher_lib::error::AppError;
use cbmp_launcher_lib::game::{GameInstall, Processes, Store};
use cbmp_launcher_lib::launch::acl;
use cbmp_launcher_lib::modact::{
    ensure_active, status, ModStatus, BACKUP_FOLDER, BACKUP_MANIFEST, LOADER_NAME, OVERRIDE_NAME,
};
use cbmp_launcher_lib::modpkg::{container_dir, mod_dir, win64_dir, ModInstaller};
use std::path::{Path, PathBuf};

#[path = "common/modzip.rs"]
mod modzip;

const LOADER: &[u8] = b"MZ-carregador-do-pacote";
const OLD_LOADER: &[u8] = b"MZ-carregador-anterior";
const OVERRIDE_OURS: &[u8] = b"cbmp\\atual\\ue4ss\r\n";
const SID: &str = "S-1-5-21-1-2-3-1001";

struct Env {
    tmp: tempfile::TempDir,
    game: GameInstall,
    mods: PathBuf,
    keys: modzip::Keys,
}

impl Env {
    fn win64(&self) -> PathBuf {
        win64_dir(&self.game)
    }
    fn put(&self, name: &str, data: &[u8]) {
        std::fs::write(self.win64().join(name), data).unwrap();
    }
    fn read(&self, name: &str) -> Vec<u8> {
        std::fs::read(self.win64().join(name)).unwrap()
    }
    fn pubkey(&self) -> String {
        modzip::pubkey(&self.keys)
    }
    /// Pacote assinado em `mods\<sub>` (`pending` ou `package`).
    fn package(&self, sub: &str, loader: &[u8], keys: &modzip::Keys) -> PathBuf {
        let dir = self.mods.join(sub);
        std::fs::create_dir_all(&dir).unwrap();
        let zip = modzip::build("0.2.0", 1, loader);
        std::fs::write(dir.join("cbmp-mod.zip"), &zip).unwrap();
        std::fs::write(
            dir.join("cbmp-mod.zip.sig"),
            modzip::sign(keys, &zip, 1, "0.2.0"),
        )
        .unwrap();
        dir
    }
    fn args(&self, package_dir: PathBuf, sid: &str) -> AdminArgs {
        AdminArgs {
            game_dir: self.game.dir.clone(),
            user_sid: sid.into(),
            package_dir,
            confirm_conflict: false,
            replace_loader: None,
        }
    }
    fn installer(&self) -> ModInstaller {
        ModInstaller::new(self.mods.clone(), self.pubkey())
    }
    /// Pasta que faz o papel de `%SystemRoot%` nos testes de `validate`.
    fn fake_windows(&self) -> PathBuf {
        let w = self.tmp.path().join("Windows");
        std::fs::create_dir_all(&w).unwrap();
        w
    }
}

fn env() -> Env {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("Epic Games").join("HogwartsLegacy");
    std::fs::create_dir_all(dir.join("Phoenix").join("Binaries").join("Win64")).unwrap();
    std::fs::write(dir.join("HogwartsLegacy.exe"), b"MZ").unwrap();
    Env {
        game: GameInstall {
            exe: dir.join("HogwartsLegacy.exe"),
            dir,
            store: Store::Manual,
        },
        mods: tmp.path().join("mods"),
        tmp,
        keys: modzip::keys(),
    }
}

fn s(v: &[&str]) -> Vec<String> {
    v.iter().map(|x| x.to_string()).collect()
}

fn me() -> String {
    acl::current_user_sid().unwrap()
}

// ----- argumentos -----

#[test]
fn modo_elevado_so_pelo_primeiro_argumento() {
    assert!(wants_admin_setup(&s(&["cbmp.exe", FLAG])));
    assert!(!wants_admin_setup(&s(&["cbmp.exe"])));
    assert!(!wants_admin_setup(&s(&[
        "cbmp.exe",
        "cbmp://auth?code=x",
        FLAG
    ])));
    assert!(!wants_admin_setup(&s(&["cbmp.exe", "--MOD-ADMIN-SETUP"])));
    // `main.rs`: fora do modo elevado o launcher segue normal; nele, argumento ruim sai com 2 sem fazer nada.
    assert_eq!(entry(["cbmp.exe".into()]), None);
    assert_eq!(
        entry(["cbmp.exe".into(), "cbmp://auth?code=x".into()]),
        None
    );
    assert_eq!(entry(["cbmp.exe".into(), FLAG.into()]), Some(2));
    assert_eq!(
        entry(["cbmp.exe".into(), FLAG.into(), "cbmp://auth?x".into()]),
        Some(2)
    );
}

const FULL: [&str; 9] = [
    "cbmp.exe",
    FLAG,
    "--game-dir",
    r"C:\Program Files\Epic Games\HogwartsLegacy",
    "--user-sid",
    SID,
    "--package-dir",
    r"C:\Users\x\AppData\Local\CBMultiplayer\mods\pending",
    "--confirm-conflict",
];

#[test]
fn parse_aceita_a_forma_completa() {
    let a = parse_args(&s(&FULL)).unwrap();
    assert_eq!(
        a,
        AdminArgs {
            game_dir: PathBuf::from(r"C:\Program Files\Epic Games\HogwartsLegacy"),
            user_sid: SID.into(),
            package_dir: PathBuf::from(r"C:\Users\x\AppData\Local\CBMultiplayer\mods\pending"),
            confirm_conflict: true,
            replace_loader: None,
        }
    );
    let mut no_confirm = s(&FULL[..8]);
    assert!(!parse_args(&no_confirm).unwrap().confirm_conflict);
    no_confirm.extend(s(&["--replace-loader", &"a".repeat(64)]));
    assert_eq!(
        parse_args(&no_confirm).unwrap().replace_loader,
        Some("a".repeat(64))
    );
}

#[test]
fn parse_recusa_argumento_a_mais_repetido_sem_valor_e_relativo() {
    let with = |extra: &[&str]| {
        let mut v = s(&FULL);
        v.extend(s(extra));
        v
    };
    let swap = |i: usize, val: &str| {
        let mut v = s(&FULL);
        v[i] = val.into();
        v
    };
    let bad: Vec<Vec<String>> = vec![
        // A mais / solto / deep link / ticket.
        with(&["extra"]),
        with(&["cbmp://auth?code=x"]),
        with(&["--ticket", "abc"]),
        // Repetido.
        with(&["--confirm-conflict"]),
        with(&["--game-dir", r"D:\HL"]),
        with(&["--user-sid", SID]),
        // Sem valor.
        s(&FULL[..3]),
        s(&["cbmp.exe", FLAG, "--game-dir", "--user-sid", SID]),
        with(&["--replace-loader"]),
        // Valor ausente obrigatório.
        s(&["cbmp.exe", FLAG, "--game-dir", r"C:\HL", "--user-sid", SID]),
        // Relativo, UNC, prefixo `\\?\`, `..`.
        swap(3, r"Epic Games\HogwartsLegacy"),
        swap(3, r"\\servidor\jogos\HL"),
        swap(3, r"\\?\C:\HL"),
        swap(3, r"C:\Jogos\..\Windows"),
        swap(7, r"mods\pending"),
        // Hash inválido.
        with(&["--replace-loader", "xyz"]),
        // Sem a flag no lugar.
        swap(1, "--game-dir"),
    ];
    for b in bad {
        assert!(parse_args(&b).is_err(), "{b:?}");
    }
}

#[cfg(windows)]
fn argv(cmdline: &str) -> Vec<String> {
    use windows_sys::Win32::Foundation::{LocalFree, HLOCAL};
    use windows_sys::Win32::UI::Shell::CommandLineToArgvW;
    let w: Vec<u16> = cmdline.encode_utf16().chain(Some(0)).collect();
    let mut n = 0i32;
    // SAFETY: string terminada em zero; o vetor devolvido é liberado com LocalFree.
    let p = unsafe { CommandLineToArgvW(w.as_ptr(), &mut n) };
    assert!(!p.is_null());
    let out = (0..n as usize)
        .map(|i| {
            // SAFETY: `n` ponteiros para strings terminadas em zero.
            let s = unsafe { *p.add(i) };
            let len = unsafe { (0..).take_while(|&j| *s.add(j) != 0).count() };
            String::from_utf16(unsafe { std::slice::from_raw_parts(s, len) }).unwrap()
        })
        .collect();
    unsafe { LocalFree(p as HLOCAL) };
    out
}

#[cfg(windows)]
#[test]
fn linha_de_comando_ida_e_volta_com_espacos_acentos_e_barra_no_fim() {
    for (game, pkg, confirm, replace) in [
        (
            r"C:\Program Files\Epic Games\HogwartsLegacy",
            r"C:\Users\João da Silva\AppData\Local\CBMultiplayer\mods\pending",
            true,
            None,
        ),
        (
            r"D:\Jogos ção\Hogwarts Legacy\",
            r"C:\Usuários\ä ö\mods\package\",
            false,
            Some("0123456789abcdef".repeat(4)),
        ),
    ] {
        let a = AdminArgs {
            game_dir: PathBuf::from(game),
            user_sid: SID.into(),
            package_dir: PathBuf::from(pkg),
            confirm_conflict: confirm,
            replace_loader: replace,
        };
        let line = format!(
            "\"C:\\Program Files\\CB Multiplayer\\cbmp.exe\" {}",
            to_command_line(&a)
        );
        let back = parse_args(&argv(&line)).unwrap();
        assert_eq!(back, a, "{line}");
        assert_eq!(back.game_dir.to_str().unwrap(), game);
    }
}

// ----- validação -----

#[test]
fn validate_aceita_pasta_de_jogo_de_verdade() {
    let e = env();
    let pkg = e.package("pending", LOADER, &e.keys);
    let g = validate(&e.args(pkg, SID), &e.fake_windows()).unwrap();
    assert_eq!(g.dir, e.game.dir);
}

#[test]
fn validate_recusa() {
    let e = env();
    let pkg = e.package("pending", LOADER, &e.keys);
    let win = e.fake_windows();
    let check = |a: AdminArgs, root: &Path, why: &str| {
        let err = validate(&a, root).unwrap_err();
        assert_eq!(err.code, "invalid_admin_args", "{why}");
    };
    // Pasta que não é do jogo.
    let mut a = e.args(pkg.clone(), SID);
    a.game_dir = e.tmp.path().join("mods");
    check(a, &win, "não é o jogo");
    // Jogo dentro de um `%SystemRoot%` falso (sem diferenciar caixa).
    let upper = PathBuf::from(e.tmp.path().to_string_lossy().to_uppercase());
    check(e.args(pkg.clone(), SID), e.tmp.path(), "dentro do Windows");
    check(
        e.args(pkg.clone(), SID),
        &upper,
        "dentro do Windows (caixa)",
    );
    // SIDs que não são de usuário.
    for sid in ["S-1-5-18", "S-1-5-21-1", "S-1-5-32-544", ""] {
        check(e.args(pkg.clone(), sid), &win, sid);
    }
    // Pacote relativo ou ausente.
    check(
        e.args(PathBuf::from(r"mods\pending"), SID),
        &win,
        "relativo",
    );
    check(e.args(e.tmp.path().join("nada"), SID), &win, "ausente");
    // Jogo relativo.
    let mut a = e.args(pkg, SID);
    a.game_dir = PathBuf::from(r"Epic Games\HogwartsLegacy");
    check(a, &win, "jogo relativo");
}

#[cfg(windows)]
fn junction(link: &Path, target: &Path) {
    let out = std::process::Command::new("cmd")
        .args(["/C", "mklink", "/J"])
        .arg(link)
        .arg(target)
        .output()
        .unwrap();
    assert!(out.status.success(), "{out:?}");
}

#[cfg(windows)]
#[test]
fn validate_recusa_juncao_ou_link_no_caminho() {
    let e = env();
    let pkg = e.package("pending", LOADER, &e.keys);
    let win = e.fake_windows();
    // Junção no meio do caminho do jogo (`<tmp>\via` → `<tmp>\Epic Games`).
    let via = e.tmp.path().join("via");
    junction(&via, &e.tmp.path().join("Epic Games"));
    let mut a = e.args(pkg.clone(), SID);
    a.game_dir = via.join("HogwartsLegacy");
    assert_eq!(validate(&a, &win).unwrap_err().code, "invalid_admin_args");
    // `Win64` trocado por junção.
    let real = e.tmp.path().join("win64-real");
    std::fs::rename(e.win64(), &real).unwrap();
    junction(&e.win64(), &real);
    assert_eq!(
        validate(&e.args(pkg.clone(), SID), &win).unwrap_err().code,
        "invalid_admin_args"
    );
    std::fs::remove_dir(e.win64()).unwrap();
    std::fs::rename(&real, e.win64()).unwrap();
    validate(&e.args(pkg.clone(), SID), &win).unwrap();
    // Pacote por junção.
    let pkg_link = e.tmp.path().join("pkg-link");
    junction(&pkg_link, &pkg);
    assert_eq!(
        validate(&e.args(pkg_link, SID), &win).unwrap_err().code,
        "invalid_admin_args"
    );
    // Link simbólico de pasta (precisa de privilégio ou modo de desenvolvedor).
    let sym = e.tmp.path().join("sym");
    match std::os::windows::fs::symlink_dir(e.tmp.path().join("Epic Games"), &sym) {
        Ok(()) => {
            let mut a = e.args(pkg, SID);
            a.game_dir = sym.join("HogwartsLegacy");
            assert_eq!(validate(&a, &win).unwrap_err().code, "invalid_admin_args");
        }
        Err(err) => eprintln!("symlink_dir sem privilégio ({err}); caso do link simbólico pulado"),
    }
}

#[cfg(windows)]
#[test]
fn pastas_presas_nao_podem_ser_renomeadas() {
    let e = env();
    let held = hold_path(&e.win64()).unwrap();
    // A própria pasta presa (sem compartilhar a exclusão) não pode ser renomeada nem trocada por junção.
    let aside = e.game.dir.join("Phoenix").join("Binaries").join("outra");
    assert!(
        std::fs::rename(e.win64(), &aside).is_err(),
        "Win64 renomeada com a pasta presa"
    );
    let moved = e.tmp.path().join("movido");
    assert!(
        std::fs::rename(&e.game.dir, &moved).is_err(),
        "jogo renomeado com a pasta presa"
    );
    assert!(std::fs::rename(e.tmp.path().join("Epic Games"), &moved).is_err());
    drop(held);
    std::fs::rename(&e.game.dir, &moved).unwrap();
    // Junção no caminho: não prende.
    let via = e.tmp.path().join("via");
    junction(&via, &e.tmp.path().join("Epic Games"));
    assert_eq!(hold_path(&via).unwrap_err().code, "invalid_admin_args");
}

#[cfg(windows)]
#[test]
fn pacote_aberto_nao_pode_ser_trocado_enquanto_e_conferido() {
    let e = env();
    let pkg = e.package("pending", LOADER, &e.keys);
    let (mut f, sig) = cbmp_launcher_lib::modpkg::open_package(&pkg).unwrap();
    let zip = pkg.join("cbmp-mod.zip");
    assert!(
        std::fs::OpenOptions::new().write(true).open(&zip).is_err(),
        "gravável"
    );
    assert!(
        std::fs::rename(&zip, pkg.join("outro.zip")).is_err(),
        "renomeável"
    );
    assert!(std::fs::remove_file(&zip).is_err(), "apagável");
    let v = cbmp_launcher_lib::modpkg::verify_package(
        &mut f,
        &sig,
        &e.pubkey(),
        &semver::Version::new(0, 2, 0),
    )
    .unwrap();
    assert_eq!(v.loader, LOADER);
    assert_eq!(v.loader_ref.sha256, modzip::sha_hex(LOADER));
    assert_eq!(v.signed.version, semver::Version::new(0, 2, 0));
    drop(f);
    std::fs::remove_file(&zip).unwrap();
}

// ----- o trabalho elevado (sem elevar) -----

fn backup_dir(e: &Env) -> PathBuf {
    e.win64().join(BACKUP_FOLDER)
}

#[cfg(windows)]
#[test]
fn run_ativa_o_carregador_libera_cbmp_e_nao_instala_o_pacote() {
    let e = env();
    let pkg = e.package("pending", LOADER, &e.keys);
    let sid = me();
    run(&e.args(pkg, &sid), &e.pubkey()).unwrap();
    assert_eq!(e.read(LOADER_NAME), LOADER);
    assert_eq!(e.read(OVERRIDE_NAME), OVERRIDE_OURS);
    let cbmp = container_dir(&e.game);
    assert!(cbmp.is_dir());
    let dacl = acl::read_dacl(&cbmp).unwrap();
    assert!(
        dacl.aces.iter().any(|a| a.sid == sid
            && a.allowed
            && a.mask & acl::MODIFY == acl::MODIFY
            && a.flags & acl::INHERIT_TO_CHILDREN == acl::INHERIT_TO_CHILDREN),
        "{dacl:?}"
    );
    // Nada em `cbmp\atual`, nada além do contêiner vazio; sem backup nem temporários.
    assert!(!mod_dir(&e.game).exists());
    assert_eq!(std::fs::read_dir(&cbmp).unwrap().count(), 0);
    assert!(!backup_dir(&e).exists());
    let mut names: Vec<String> = std::fs::read_dir(e.win64())
        .unwrap()
        .map(|d| d.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    assert_eq!(names, vec!["cbmp", "dwmapi.dll", "override.txt"]);
    // De novo: nada muda.
    run(&e.args(e.mods.join("pending"), &sid), &e.pubkey()).unwrap();
    assert_eq!(e.read(LOADER_NAME), LOADER);
}

#[test]
fn run_com_assinatura_de_outra_chave_nao_grava_nada() {
    let e = env();
    let other = modzip::keys();
    let pkg = e.package("pending", LOADER, &other);
    let r = run(&e.args(pkg, SID), &e.pubkey());
    assert_eq!(r.as_ref().unwrap_err().code, "mod_signature_invalid");
    assert_eq!(exit_code(&r), 3);
    assert_eq!(
        std::fs::read_dir(e.win64()).unwrap().count(),
        0,
        "nada gravado"
    );
}

#[test]
fn run_com_zip_alterado_depois_de_assinado_nao_grava_nada() {
    let e = env();
    let pkg = e.package("pending", LOADER, &e.keys);
    let other_zip = modzip::build("0.2.0", 1, b"MZ-outro");
    std::fs::write(pkg.join("cbmp-mod.zip"), other_zip).unwrap();
    let r = run(&e.args(pkg, SID), &e.pubkey());
    assert_eq!(r.unwrap_err().code, "mod_signature_invalid");
    assert_eq!(std::fs::read_dir(e.win64()).unwrap().count(), 0);
}

#[cfg(windows)]
#[test]
fn run_com_carregador_estranho_so_com_confirmacao() {
    let e = env();
    let pkg = e.package("pending", LOADER, &e.keys);
    e.put(LOADER_NAME, b"UE4SS do jogador");
    let sid = me();
    let r = run(&e.args(pkg.clone(), &sid), &e.pubkey());
    assert_eq!(r.as_ref().unwrap_err().code, "mod_conflict");
    assert_eq!(exit_code(&r), 4);
    assert_eq!(e.read(LOADER_NAME), b"UE4SS do jogador", "intacto");
    assert!(!container_dir(&e.game).exists(), "nada gravado no conflito");
    assert!(!e.win64().join(OVERRIDE_NAME).exists());

    let mut a = e.args(pkg, &sid);
    a.confirm_conflict = true;
    run(&a, &e.pubkey()).unwrap();
    assert_eq!(e.read(LOADER_NAME), LOADER);
    assert_eq!(
        std::fs::read(backup_dir(&e).join(LOADER_NAME)).unwrap(),
        b"UE4SS do jogador"
    );
    // Manifesto dos backups (o mesmo que o launcher normal adota), com o sha256 do arquivo do jogador.
    let m: serde_json::Value =
        serde_json::from_slice(&std::fs::read(backup_dir(&e).join(BACKUP_MANIFEST)).unwrap())
            .unwrap();
    assert_eq!(m["v"], 1);
    assert_eq!(m["entries"].as_array().unwrap().len(), 1);
    assert_eq!(m["entries"][0]["name"], "dwmapi.dll");
    assert_eq!(m["entries"][0]["stored"], "dwmapi.dll");
    assert_eq!(m["entries"][0]["state"], "done");
    assert_eq!(
        m["entries"][0]["sha256"],
        modzip::sha_hex(b"UE4SS do jogador")
    );

    // O registro do launcher não foi escrito pelo modo elevado.
    assert!(!e.mods.join("installed.json").exists());
}

#[cfg(windows)]
#[test]
fn run_substitui_o_carregador_nosso_anterior_so_pelo_hash_indicado() {
    let e = env();
    let pkg = e.package("package", LOADER, &e.keys);
    e.put(LOADER_NAME, OLD_LOADER);
    e.put(OVERRIDE_NAME, OVERRIDE_OURS);
    let sid = me();
    // Sem o hash: é estranho (conflito).
    assert_eq!(
        run(&e.args(pkg.clone(), &sid), &e.pubkey())
            .unwrap_err()
            .code,
        "mod_conflict"
    );
    let mut a = e.args(pkg.clone(), &sid);
    a.replace_loader = Some(modzip::sha_hex(b"outro hash qualquer"));
    assert_eq!(run(&a, &e.pubkey()).unwrap_err().code, "mod_conflict");
    a.replace_loader = Some(modzip::sha_hex(OLD_LOADER));
    run(&a, &e.pubkey()).unwrap();
    assert_eq!(e.read(LOADER_NAME), LOADER);
    assert!(
        !backup_dir(&e).exists(),
        "nosso anterior não vai para o backup"
    );
}

#[test]
fn run_com_ue4ss_antigo_nao_mexe() {
    let e = env();
    let pkg = e.package("pending", LOADER, &e.keys);
    e.put("xinput1_3.dll", b"proxy");
    e.put("UE4SS.dll", b"ue4ss");
    let mut a = e.args(pkg, SID);
    a.confirm_conflict = true;
    let r = run(&a, &e.pubkey());
    assert_eq!(r.as_ref().unwrap_err().code, "mod_conflict");
    assert_eq!(exit_code(&r), 4);
    assert!(!e.win64().join(LOADER_NAME).exists());
    assert!(!container_dir(&e.game).exists());
}

#[test]
fn run_recusa_argumentos_invalidos_com_codigo_2() {
    let e = env();
    let pkg = e.package("pending", LOADER, &e.keys);
    let r = run(&e.args(pkg, "S-1-5-18"), &e.pubkey());
    assert_eq!(exit_code(&r), 2);
    assert_eq!(std::fs::read_dir(e.win64()).unwrap().count(), 0);
}

#[test]
fn codigos_de_saida() {
    let err = |c: &'static str| Err(AppError::new(c, "x"));
    assert_eq!(exit_code(&Ok(())), 0);
    assert_eq!(exit_code(&err("invalid_admin_args")), 2);
    assert_eq!(exit_code(&err("mod_signature_invalid")), 3);
    assert_eq!(exit_code(&err("mod_conflict")), 4);
    for other in [
        "mod_activation_failed",
        "mod_needs_admin",
        "mod_invalid",
        "mod_incompatible",
        "permission_denied",
        "io_error",
    ] {
        assert_eq!(exit_code(&err(other)), 5, "{other}");
    }
}

// ----- o comando (UAC trocado pelo `run` direto) -----

struct Procs(&'static [&'static str]);

impl Processes for Procs {
    fn names(&self) -> Option<Vec<String>> {
        Some(self.0.iter().map(|s| s.to_string()).collect())
    }
}

const CLOSED: Procs = Procs(&["steam.exe"]);

#[cfg(windows)]
#[test]
fn comando_instala_com_o_modo_elevado_e_deixa_tudo_ativo() {
    let e = env();
    e.package("pending", LOADER, &e.keys);
    let mods = e.installer();
    let sid = me();
    let pk = e.pubkey();
    let mut called = None;
    let info = mod_admin_setup_with(
        &e.game,
        &mods,
        &CLOSED,
        &sid,
        || panic!("sem conflito não há diálogo"),
        |a| {
            called = Some(a.clone());
            run(a, &pk)
        },
    )
    .unwrap();
    let a = called.unwrap();
    assert_eq!(a.package_dir, e.mods.join("pending"));
    assert_eq!(a.user_sid, sid);
    assert!(!a.confirm_conflict);
    assert_eq!(a.replace_loader, None);
    assert_eq!(
        info.status,
        ModStatus::Installed {
            version: "0.2.0".into()
        }
    );
    // O pacote foi instalado sem elevação em `cbmp\atual` e virou o da versão registrada.
    assert!(mod_dir(&e.game).join("loader").join(LOADER_NAME).is_file());
    assert!(!e.mods.join("pending").exists());
    assert!(e.mods.join("package").join("cbmp-mod.zip").is_file());
    assert_eq!(e.read(LOADER_NAME), LOADER);
    ensure_active(&e.game, &mods, false).unwrap();
    assert_eq!(
        status(Some(&e.game), &mods),
        ModStatus::Installed {
            version: "0.2.0".into()
        }
    );
}

#[cfg(windows)]
#[test]
fn comando_com_conflito_pergunta_e_o_launcher_adota_o_backup_do_modo_elevado() {
    let e = env();
    e.package("pending", LOADER, &e.keys);
    e.put(LOADER_NAME, b"UE4SS do jogador");
    let mods = e.installer();
    let sid = me();
    let pk = e.pubkey();
    // Cancelado: não eleva, nada muda.
    let info = mod_admin_setup_with(
        &e.game,
        &mods,
        &CLOSED,
        &sid,
        || false,
        |_| panic!("cancelado não eleva"),
    )
    .unwrap();
    assert_eq!(info.status, ModStatus::NotInstalled);
    assert_eq!(e.read(LOADER_NAME), b"UE4SS do jogador");
    // Aceito: eleva com a confirmação; o backup feito pelo modo elevado entra no registro do launcher.
    let mut asked = 0;
    mod_admin_setup_with(
        &e.game,
        &mods,
        &CLOSED,
        &sid,
        || {
            asked += 1;
            true
        },
        |a| {
            assert!(a.confirm_conflict);
            run(a, &pk)
        },
    )
    .unwrap();
    assert_eq!(asked, 1);
    let rec = mods.installed().unwrap();
    assert_eq!(rec.backups.len(), 1);
    assert_eq!(rec.backups[0].stored, "dwmapi.dll");
    assert_eq!(
        rec.backups[0].sha256.as_deref(),
        Some(modzip::sha_hex(b"UE4SS do jogador").as_str())
    );
    // Remover (pasta gravável aqui) devolve o arquivo do jogador.
    cbmp_launcher_lib::modact::remove_mod(&e.game, &mods).unwrap();
    assert_eq!(e.read(LOADER_NAME), b"UE4SS do jogador");
    assert!(!backup_dir(&e).exists());
}

#[cfg(windows)]
#[test]
fn comando_passa_o_carregador_nosso_anterior_para_ser_substituido() {
    let e = env();
    e.package("pending", LOADER, &e.keys);
    let mods = e.installer();
    // Registro com o carregador anterior ativo em `Win64`.
    let mut rec: cbmp_launcher_lib::modpkg::Installed = serde_json::from_value(serde_json::json!({
        "version": "0.1.9", "protocol": 1, "gameDir": e.game.dir, "sha256": "b".repeat(64), "files": [],
        "layout": 2, "loader": { "sha256": modzip::sha_hex(OLD_LOADER), "size": OLD_LOADER.len() }
    }))
    .unwrap();
    rec.loader_history = vec![];
    mods.save_record(&rec).unwrap();
    e.put(LOADER_NAME, OLD_LOADER);
    e.put(OVERRIDE_NAME, OVERRIDE_OURS);
    let pk = e.pubkey();
    mod_admin_setup_with(
        &e.game,
        &mods,
        &CLOSED,
        &me(),
        || panic!("carregador nosso não é conflito"),
        |a| {
            assert_eq!(a.replace_loader, Some(modzip::sha_hex(OLD_LOADER)));
            assert!(!a.confirm_conflict);
            run(a, &pk)
        },
    )
    .unwrap();
    assert_eq!(e.read(LOADER_NAME), LOADER);
    assert!(!backup_dir(&e).exists());
}

#[test]
fn comando_recusa_jogo_aberto_e_sem_pacote() {
    let e = env();
    let mods = e.installer();
    let no_elevate = |_: &AdminArgs| -> Result<(), AppError> { panic!("não pode elevar") };
    e.package("pending", LOADER, &e.keys);
    let err = mod_admin_setup_with(
        &e.game,
        &mods,
        &Procs(&["HogwartsLegacy.exe"]),
        SID,
        || panic!("sem diálogo"),
        no_elevate,
    )
    .unwrap_err();
    assert_eq!(err.code, "game_running");
    std::fs::remove_dir_all(e.mods.join("pending")).unwrap();
    let err = mod_admin_setup_with(&e.game, &mods, &CLOSED, SID, || false, no_elevate).unwrap_err();
    assert_eq!(err.code, "mod_unavailable");
    assert_eq!(
        err.message,
        "Clique em Jogar uma vez para baixar o mod e tente de novo."
    );
    // Sem `pending`, usa `package`.
    e.package("package", LOADER, &e.keys);
    let mut used = None;
    let _ = mod_admin_setup_with(
        &e.game,
        &mods,
        &CLOSED,
        SID,
        || false,
        |a| {
            used = Some(a.package_dir.clone());
            Err(AppError::new("mod_needs_admin", "x"))
        },
    );
    assert_eq!(used, Some(e.mods.join("package")));
}

#[test]
fn comando_recusa_pacote_de_outra_chave_antes_de_elevar() {
    let e = env();
    e.package("pending", LOADER, &modzip::keys());
    let err = mod_admin_setup_with(
        &e.game,
        &e.installer(),
        &CLOSED,
        SID,
        || false,
        |_| panic!("não pode elevar"),
    )
    .unwrap_err();
    assert_eq!(err.code, "mod_signature_invalid");
}
