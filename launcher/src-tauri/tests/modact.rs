//! Ativação do mod, conflito com backup, remoção e estado, numa pasta de jogo falsa (`tempfile`). O fluxo completo
//! instala de verdade por um Hub falso (axum em 127.0.0.1) com um par minisign gerado aqui.

use axum::extract::State;
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::{Json, Router};
use cbmp_launcher_lib::game::{game_running_with, GameInstall, Processes, Store};
use cbmp_launcher_lib::hub::HubClient;
use cbmp_launcher_lib::modact::{
    activate, ensure_active, inspect, leftover_folders, reconcile_all, remove_everywhere,
    remove_mod, status, Activation, ModStatus, BACKUP_FOLDER, LOADER_NAME, OVERRIDE_NAME,
};
use cbmp_launcher_lib::modpkg::{
    container_dir, mod_dir, win64_dir, BackupEntry, BackupState, FolderRecord, Installed,
    InstalledFile, LoaderRef, ModInstaller,
};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use tokio_util::sync::CancellationToken;

#[path = "common/modzip.rs"]
mod modzip;

const LOADER: &[u8] = b"MZ-carregador-novo";
const OLD_LOADER: &[u8] = b"MZ-carregador-antigo";
const OVERRIDE_OURS: &[u8] = b"cbmp\\atual\\ue4ss\r\n";

struct Game {
    _tmp: tempfile::TempDir,
    game: GameInstall,
    mods: PathBuf,
}

impl Game {
    fn win64(&self) -> PathBuf {
        win64_dir(&self.game)
    }
    fn current(&self) -> PathBuf {
        mod_dir(&self.game)
    }
    fn backup(&self) -> PathBuf {
        self.win64().join(BACKUP_FOLDER)
    }
    fn installer(&self) -> ModInstaller {
        ModInstaller::new(self.mods.clone(), "sem-chave".into())
    }
    fn read(&self, name: &str) -> Vec<u8> {
        std::fs::read(self.win64().join(name)).unwrap()
    }
    fn put(&self, name: &str, data: &[u8]) {
        std::fs::write(self.win64().join(name), data).unwrap();
    }
}

fn fake_game() -> Game {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("Hogwarts Legacy");
    std::fs::create_dir_all(dir.join("Phoenix").join("Binaries").join("Win64")).unwrap();
    std::fs::write(dir.join("HogwartsLegacy.exe"), b"MZ").unwrap();
    let game = GameInstall {
        exe: dir.join("HogwartsLegacy.exe"),
        dir,
        store: Store::Manual,
    };
    Game {
        mods: tmp.path().join("mods"),
        _tmp: tmp,
        game,
    }
}

/// `cbmp\atual` com o carregador `LOADER` e o registro dele (sem passar pelo Hub).
fn installed(g: &Game) -> Installed {
    let loader_dir = g.current().join("loader");
    std::fs::create_dir_all(&loader_dir).unwrap();
    std::fs::write(loader_dir.join(LOADER_NAME), LOADER).unwrap();
    let sha = modzip::sha_hex(LOADER);
    let rec = Installed {
        version: "0.2.0".into(),
        protocol: 1,
        game_dir: g.game.dir.clone(),
        sha256: "b".repeat(64),
        files: vec![InstalledFile {
            path: "loader/dwmapi.dll".into(),
            sha256: sha.clone(),
            size: LOADER.len() as u64,
        }],
        layout: 2,
        loader: Some(LoaderRef {
            sha256: sha,
            size: LOADER.len() as u64,
        }),
        loader_history: vec![modzip::sha_hex(OLD_LOADER)],
        backups: vec![],
        removed: false,
        other_folders: vec![],
    };
    g.installer().save_record(&rec).unwrap();
    rec
}

fn record(g: &Game) -> Installed {
    g.installer().installed().unwrap()
}

fn missing(loader: bool, override_file: bool) -> Activation {
    Activation::Missing {
        loader,
        override_file,
    }
}

fn conflict(loader: bool, override_file: bool) -> Activation {
    Activation::Conflict {
        loader,
        override_file,
    }
}

#[test]
fn tudo_ausente_e_depois_de_ativar_fica_ativo() {
    let g = fake_game();
    let mut rec = installed(&g);
    assert_eq!(inspect(&g.win64(), &rec).unwrap(), missing(true, true));
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    assert_eq!(inspect(&g.win64(), &rec).unwrap(), Activation::Active);
    assert_eq!(g.read(LOADER_NAME), LOADER);
    assert_eq!(g.read(OVERRIDE_NAME), OVERRIDE_OURS);
    assert!(rec.backups.is_empty());
    assert!(!g.backup().exists());
    // Ativo: nada a fazer.
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    // `override.txt` com espaços/quebras nas pontas ainda é o nosso.
    g.put(OVERRIDE_NAME, b"  cbmp\\atual\\ue4ss \n\n");
    assert_eq!(inspect(&g.win64(), &rec).unwrap(), Activation::Active);
}

#[test]
fn carregador_do_historico_e_nosso_e_e_atualizado() {
    let g = fake_game();
    let mut rec = installed(&g);
    g.put(LOADER_NAME, OLD_LOADER);
    g.put(OVERRIDE_NAME, OVERRIDE_OURS);
    // Nem estranho (conflito) nem atual: precisa ser trocado.
    assert_eq!(inspect(&g.win64(), &rec).unwrap(), missing(true, false));
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    assert_eq!(g.read(LOADER_NAME), LOADER);
    assert!(rec.backups.is_empty());
    assert_eq!(inspect(&g.win64(), &rec).unwrap(), Activation::Active);
}

#[test]
fn carregador_estranho_so_sai_com_confirmacao_e_vai_para_o_backup() {
    let g = fake_game();
    let mut rec = installed(&g);
    g.put(LOADER_NAME, b"UE4SS do jogador");
    assert_eq!(inspect(&g.win64(), &rec).unwrap(), conflict(true, false));
    let e = activate(&g.win64(), &g.current(), &mut rec, false).unwrap_err();
    assert_eq!(e.code, "mod_conflict");
    assert_eq!(
        e.message,
        "Já existe outro carregador de mods (UE4SS) na pasta do jogo."
    );
    assert_eq!(g.read(LOADER_NAME), b"UE4SS do jogador", "intacto");
    assert!(!g.win64().join(OVERRIDE_NAME).exists());

    activate(&g.win64(), &g.current(), &mut rec, true).unwrap();
    assert_eq!(
        std::fs::read(g.backup().join("dwmapi.dll")).unwrap(),
        b"UE4SS do jogador"
    );
    assert_eq!(g.read(LOADER_NAME), LOADER);
    assert_eq!(g.read(OVERRIDE_NAME), OVERRIDE_OURS);
    assert_eq!(rec.backups.len(), 1);
    assert_eq!(
        (rec.backups[0].name.as_str(), rec.backups[0].stored.as_str()),
        ("dwmapi.dll", "dwmapi.dll")
    );

    // Segundo conflito com o mesmo nome: `.1`.
    g.put(LOADER_NAME, b"outro UE4SS");
    activate(&g.win64(), &g.current(), &mut rec, true).unwrap();
    assert_eq!(
        std::fs::read(g.backup().join("dwmapi.dll.1")).unwrap(),
        b"outro UE4SS"
    );
    assert_eq!(rec.backups[1].stored, "dwmapi.dll.1");
    assert_eq!(
        std::fs::read(g.backup().join("dwmapi.dll")).unwrap(),
        b"UE4SS do jogador"
    );
}

#[test]
fn override_estranho_idem() {
    let g = fake_game();
    let mut rec = installed(&g);
    g.put(LOADER_NAME, LOADER);
    g.put(OVERRIDE_NAME, b"ue4ss\\meus-mods");
    assert_eq!(inspect(&g.win64(), &rec).unwrap(), conflict(false, true));
    assert_eq!(
        activate(&g.win64(), &g.current(), &mut rec, false)
            .unwrap_err()
            .code,
        "mod_conflict"
    );
    assert_eq!(g.read(OVERRIDE_NAME), b"ue4ss\\meus-mods");
    activate(&g.win64(), &g.current(), &mut rec, true).unwrap();
    assert_eq!(
        std::fs::read(g.backup().join("override.txt")).unwrap(),
        b"ue4ss\\meus-mods"
    );
    assert_eq!(g.read(OVERRIDE_NAME), OVERRIDE_OURS);
    assert_eq!(rec.backups.len(), 1);
    assert_eq!(rec.backups[0].name, "override.txt");
}

#[test]
fn ue4ss_antigo_por_xinput_nao_e_tocado() {
    let g = fake_game();
    let mut rec = installed(&g);
    g.put("xinput1_3.dll", b"proxy");
    g.put("UE4SS.dll", b"ue4ss");
    g.put(LOADER_NAME, b"estranho");
    assert_eq!(inspect(&g.win64(), &rec).unwrap(), Activation::LegacyUe4ss);
    let e = activate(&g.win64(), &g.current(), &mut rec, true).unwrap_err();
    assert_eq!(e.code, "mod_conflict");
    assert!(e.message.contains("xinput1_3.dll"), "{}", e.message);
    assert!(!g.backup().exists(), "nada movido");
    assert_eq!(g.read(LOADER_NAME), b"estranho");
    assert_eq!(g.read("xinput1_3.dll"), b"proxy");
    assert!(rec.backups.is_empty());
    // Só um dos dois não é o UE4SS antigo.
    std::fs::remove_file(g.win64().join("UE4SS.dll")).unwrap();
    assert_eq!(inspect(&g.win64(), &rec).unwrap(), conflict(true, false));
}

#[test]
fn ensure_active_grava_o_backup_no_registro() {
    let g = fake_game();
    installed(&g);
    g.put(LOADER_NAME, b"estranho");
    let mods = g.installer();
    assert_eq!(
        ensure_active(&g.game, &mods, false).unwrap_err().code,
        "mod_conflict"
    );
    assert!(record(&g).backups.is_empty());
    ensure_active(&g.game, &mods, true).unwrap();
    let r = record(&g);
    assert_eq!(r.backups.len(), 1);
    assert_eq!(r.backups[0].stored, "dwmapi.dll");
    assert_eq!(g.read(LOADER_NAME), LOADER);
    // Ativo: segunda chamada não muda nada.
    ensure_active(&g.game, &mods, false).unwrap();
    assert_eq!(record(&g), r);
}

#[test]
fn remover_apaga_os_nossos_devolve_o_backup_e_mantem_logs_e_pacote() {
    let g = fake_game();
    let mut rec = installed(&g);
    let c = container_dir(&g.game);
    std::fs::create_dir_all(c.join("logs")).unwrap();
    std::fs::write(c.join("logs").join("cb-mp.log"), b"log").unwrap();
    std::fs::create_dir_all(c.join("crash")).unwrap();
    std::fs::write(c.join("cb-mp.json"), b"{}").unwrap();
    std::fs::write(c.join("cbmp-run.json"), b"{}").unwrap();
    std::fs::create_dir_all(c.join("atual.old")).unwrap();
    std::fs::create_dir_all(c.join("atual.staging")).unwrap();
    std::fs::create_dir_all(g.mods.join("package")).unwrap();
    std::fs::write(g.mods.join("package").join("cbmp-mod.zip"), b"zip").unwrap();
    g.put(LOADER_NAME, b"do jogador");
    activate(&g.win64(), &g.current(), &mut rec, true).unwrap();
    g.installer().save_record(&rec).unwrap();

    remove_mod(&g.game, &g.installer()).unwrap();
    assert_eq!(g.read(LOADER_NAME), b"do jogador", "backup de volta");
    assert!(!g.win64().join(OVERRIDE_NAME).exists());
    assert!(!g.backup().exists(), "cbmp-backup vazio sai");
    for gone in ["atual", "atual.old", "atual.staging", "cbmp-run.json"] {
        assert!(!c.join(gone).exists(), "{gone}");
    }
    assert!(c.join("logs").join("cb-mp.log").is_file());
    assert!(c.join("crash").is_dir());
    assert!(c.join("cb-mp.json").is_file());
    assert!(g.mods.join("package").join("cbmp-mod.zip").is_file());
    let r = record(&g);
    assert!(r.removed);
    assert_eq!(r.version, "0.2.0");
    assert!(r.files.is_empty());
    assert!(r.backups.is_empty());
    assert!(r.loader.is_none());
    assert!(r.loader_history.contains(&modzip::sha_hex(LOADER)));
    assert_eq!(
        status(Some(&g.game), &g.installer()),
        ModStatus::NotInstalled
    );
}

#[test]
fn remover_nao_apaga_estranho_que_entrou_depois_da_ativacao() {
    let g = fake_game();
    let mut rec = installed(&g);
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    g.put(LOADER_NAME, b"trocado pelo jogador");
    g.put(OVERRIDE_NAME, b"outra coisa");
    remove_mod(&g.game, &g.installer()).unwrap();
    assert_eq!(g.read(LOADER_NAME), b"trocado pelo jogador");
    assert_eq!(g.read(OVERRIDE_NAME), b"outra coisa");
    assert!(!g.current().exists());
}

#[test]
fn backup_com_destino_ocupado_fica_em_cbmp_backup() {
    let g = fake_game();
    let mut rec = installed(&g);
    g.put(OVERRIDE_NAME, b"do jogador");
    activate(&g.win64(), &g.current(), &mut rec, true).unwrap();
    g.installer().save_record(&rec).unwrap();
    // Nosso `override.txt` some e o jogador põe outro no lugar: o backup não pode sobrescrever.
    g.put(OVERRIDE_NAME, b"novo do jogador");
    remove_mod(&g.game, &g.installer()).unwrap();
    assert_eq!(g.read(OVERRIDE_NAME), b"novo do jogador");
    assert_eq!(
        std::fs::read(g.backup().join("override.txt")).unwrap(),
        b"do jogador"
    );
    let r = record(&g);
    assert_eq!(r.backups.len(), 1, "continua registrado");
    assert_eq!(r.backups[0].name, "override.txt");
    assert!(r.removed);
}

#[test]
fn estado_nos_tres_casos() {
    let g = fake_game();
    assert_eq!(status(None, &g.installer()), ModStatus::NotInstalled);
    assert_eq!(
        status(Some(&g.game), &g.installer()),
        ModStatus::NotInstalled
    );
    let mut rec = installed(&g);
    assert_eq!(
        status(Some(&g.game), &g.installer()),
        ModStatus::Installed {
            version: "0.2.0".into()
        }
    );
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    assert_eq!(
        status(Some(&g.game), &g.installer()),
        ModStatus::Installed {
            version: "0.2.0".into()
        }
    );
    g.put(LOADER_NAME, b"estranho");
    assert_eq!(status(Some(&g.game), &g.installer()), ModStatus::Conflict);
    std::fs::remove_file(g.win64().join(LOADER_NAME)).unwrap();
    g.put("xinput1_3.dll", b"x");
    g.put("UE4SS.dll", b"x");
    assert_eq!(status(Some(&g.game), &g.installer()), ModStatus::Conflict);
    // Registro de outra pasta do jogo: não instalado nesta.
    let other = GameInstall {
        dir: g.game.dir.join("outra"),
        ..g.game.clone()
    };
    assert_eq!(
        status(Some(&other), &g.installer()),
        ModStatus::NotInstalled
    );
}

struct FakeProcesses(Vec<&'static str>);

impl Processes for FakeProcesses {
    fn names(&self) -> Option<Vec<String>> {
        Some(self.0.iter().map(|s| s.to_string()).collect())
    }
}

/// Lista de processos que não pôde ser lida.
struct NoProcessList;

impl Processes for NoProcessList {
    fn names(&self) -> Option<Vec<String>> {
        None
    }
}

#[test]
fn jogo_aberto_pelo_nome_do_processo() {
    assert!(game_running_with(&FakeProcesses(vec![
        "explorer.exe",
        "hogwartslegacy.exe"
    ])));
    assert!(game_running_with(&FakeProcesses(vec![
        "HogwartsLegacy.exe"
    ])));
    assert!(!game_running_with(&FakeProcesses(vec![
        "HogwartsLegacy.exe.bak",
        "HogwartsLegacy",
        "steam.exe"
    ])));
    assert!(!game_running_with(&FakeProcesses(vec![])));
    // Lista ilegível: falha fechada (conta como jogo aberto).
    assert!(game_running_with(&NoProcessList));
    let e = cbmp_launcher_lib::game::game_running_error();
    assert_eq!(e.code, "game_running");
    assert_eq!(
        e.message,
        "O Hogwarts Legacy está aberto. Feche o jogo e tente de novo."
    );
}

#[cfg(windows)]
#[test]
fn processos_do_sistema_incluem_este_teste() {
    use cbmp_launcher_lib::game::SystemProcesses;
    let me = std::env::current_exe().unwrap();
    let me = me.file_name().unwrap().to_string_lossy().to_string();
    let names = SystemProcesses.names().expect("lista de processos");
    assert!(
        names.iter().any(|n| n.eq_ignore_ascii_case(&me)),
        "{me} fora da lista ({} processos)",
        names.len()
    );
}

// ----- fluxo completo (critério de pronto 4, sem o ponteiro) -----

#[derive(Clone)]
struct Pkg {
    zip: Vec<u8>,
    sig: String,
    base: String,
}

async fn latest(State(p): State<Arc<Mutex<Pkg>>>) -> Response {
    let p = p.lock().unwrap().clone();
    Json(serde_json::json!({
        "version": "0.2.0",
        "protocol": 1,
        "url": format!("{}/v1/dl/mod-p1-v0.2.0/cbmp-mod-p1-v0.2.0.zip", p.base),
        "sha256": modzip::sha_hex(&p.zip),
        "size": p.zip.len(),
        "signature": p.sig,
    }))
    .into_response()
}

async fn dl(State(p): State<Arc<Mutex<Pkg>>>) -> Response {
    p.lock().unwrap().zip.clone().into_response()
}

fn files_under(dir: &Path) -> Vec<String> {
    let mut out = Vec::new();
    fn walk(base: &Path, d: &Path, out: &mut Vec<String>) {
        let Ok(rd) = std::fs::read_dir(d) else { return };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                walk(base, &p, out);
            } else {
                out.push(
                    p.strip_prefix(base)
                        .unwrap()
                        .to_string_lossy()
                        .replace('\\', "/"),
                );
            }
        }
    }
    walk(dir, dir, &mut out);
    out.sort();
    out
}

/// Hub falso publicando o mod 0.2.0 (protocolo 1, carregador `LOADER`) assinado por um par gerado aqui; devolve o
/// cliente e o instalador em `mods`.
async fn hub_with_mod(mods: &Path) -> (HubClient, ModInstaller) {
    let k = modzip::keys();
    let zip = modzip::build("0.2.0", 1, LOADER);
    let pkg = Arc::new(Mutex::new(Pkg {
        sig: modzip::sign(&k, &zip, 1, "0.2.0"),
        zip,
        base: String::new(),
    }));
    let app = Router::new()
        .route("/v1/mod/latest", get(latest))
        .route("/v1/dl/{tag}/{asset}", get(dl))
        .with_state(pkg.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let base = format!("http://{}", listener.local_addr().unwrap());
    pkg.lock().unwrap().base = base.clone();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    let hub = HubClient::new(&base).unwrap();
    let mods = ModInstaller::new(mods.to_path_buf(), modzip::pubkey(&k))
        .with_launcher_version(semver::Version::new(0, 2, 0));
    (hub, mods)
}

#[tokio::test]
async fn fluxo_completo_em_pasta_falsa() {
    let g = fake_game();
    let (hub, mods) = hub_with_mod(&g.mods).await;

    // O jogador já tinha um UE4SS por `dwmapi.dll` e um `override.txt` seu.
    g.put(LOADER_NAME, b"UE4SS do jogador");
    g.put(OVERRIDE_NAME, b"ue4ss");
    let before_win64 = files_under(&g.win64());

    // Instalar.
    let st = mods
        .ensure(&hub, 1, &g.game, CancellationToken::new())
        .await
        .unwrap();
    assert_eq!((st.version.as_str(), st.changed), ("0.2.0", true));
    assert_eq!(
        std::fs::read(g.current().join("loader").join("dwmapi.dll")).unwrap(),
        LOADER
    );
    assert!(g
        .current()
        .join("ue4ss")
        .join("Mods")
        .join("cb-mp")
        .join("dlls")
        .join("main.dll")
        .is_file());

    // Ativar: conflito detectado, nada tocado sem confirmação.
    assert_eq!(status(Some(&g.game), &mods), ModStatus::Conflict);
    let e = ensure_active(&g.game, &mods, false).unwrap_err();
    assert_eq!(e.code, "mod_conflict");
    assert_eq!(g.read(LOADER_NAME), b"UE4SS do jogador");
    assert_eq!(g.read(OVERRIDE_NAME), b"ue4ss");

    // Confirmar: os dois do jogador vão para `cbmp-backup\`, os nossos entram.
    ensure_active(&g.game, &mods, true).unwrap();
    assert_eq!(g.read(LOADER_NAME), LOADER);
    assert_eq!(g.read(OVERRIDE_NAME), OVERRIDE_OURS);
    assert_eq!(files_under(&g.backup()), vec!["dwmapi.dll", "override.txt"]);
    assert_eq!(record(&g).backups.len(), 2);
    assert_eq!(
        status(Some(&g.game), &mods),
        ModStatus::Installed {
            version: "0.2.0".into()
        }
    );
    // Outro Jogar: já ativo.
    ensure_active(&g.game, &mods, false).unwrap();

    // Remover: tudo do jogador de volta, nada nosso em `Win64` além do contêiner com o que é do jogador.
    remove_mod(&g.game, &mods).unwrap();
    assert_eq!(g.read(LOADER_NAME), b"UE4SS do jogador");
    assert_eq!(g.read(OVERRIDE_NAME), b"ue4ss");
    assert!(!g.backup().exists());
    assert!(!g.current().exists());
    assert_eq!(files_under(&g.win64()), before_win64);
    assert_eq!(status(Some(&g.game), &mods), ModStatus::NotInstalled);
    assert!(mods.package_zip().is_file(), "pacote fica para reinstalar");
}

// ----- revisão da Task 8: backup com registro antes (I1) -----

fn pending(name: &str) -> BackupEntry {
    BackupEntry {
        name: name.into(),
        stored: name.into(),
        state: BackupState::Pending,
    }
}

#[test]
fn falha_ao_gravar_o_registro_nao_move_o_arquivo_do_jogador() {
    let g = fake_game();
    installed(&g);
    g.put(LOADER_NAME, b"UE4SS do jogador");
    // `installed.json.tmp` ocupado por uma pasta: toda gravação do registro falha (a leitura continua boa).
    std::fs::create_dir_all(g.mods.join("installed.json.tmp")).unwrap();
    let e = ensure_active(&g.game, &g.installer(), true).unwrap_err();
    assert!(
        matches!(e.code, "io_error" | "permission_denied"),
        "{}",
        e.code
    );
    assert_eq!(g.read(LOADER_NAME), b"UE4SS do jogador");
    assert!(!g.backup().exists(), "nada pode ter saído de Win64");
    assert!(record(&g).backups.is_empty());
}

#[test]
fn backup_e_gravado_antes_de_mover_e_marcado_done_depois() {
    let g = fake_game();
    installed(&g);
    g.put(LOADER_NAME, b"UE4SS do jogador");
    ensure_active(&g.game, &g.installer(), true).unwrap();
    let r = record(&g);
    assert_eq!(r.backups.len(), 1);
    assert_eq!(r.backups[0].state, BackupState::Done);
    assert_eq!(
        std::fs::read(g.backup().join(LOADER_NAME)).unwrap(),
        b"UE4SS do jogador"
    );
    // O registro em disco tem o estado explícito.
    let json: serde_json::Value =
        serde_json::from_slice(&std::fs::read(g.mods.join("installed.json")).unwrap()).unwrap();
    assert_eq!(json["backups"][0]["state"], "done");
}

#[test]
fn queda_depois_de_mover_backup_pendente_e_devolvido_na_remocao() {
    let g = fake_game();
    let mut rec = installed(&g);
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    // Queda entre mover e marcar `done`: o arquivo já está em `cbmp-backup\` e o registro diz `pending`.
    std::fs::create_dir_all(g.backup()).unwrap();
    std::fs::write(g.backup().join(LOADER_NAME), b"UE4SS do jogador").unwrap();
    rec.backups = vec![pending(LOADER_NAME)];
    g.installer().save_record(&rec).unwrap();
    remove_mod(&g.game, &g.installer()).unwrap();
    assert_eq!(g.read(LOADER_NAME), b"UE4SS do jogador");
    assert!(!g.backup().exists());
    assert!(record(&g).backups.is_empty());
}

#[test]
fn queda_antes_de_mover_entrada_pendente_sai_e_arquivo_fica() {
    let g = fake_game();
    let mut rec = installed(&g);
    g.put(LOADER_NAME, b"UE4SS do jogador");
    rec.backups = vec![pending(LOADER_NAME)];
    g.installer().save_record(&rec).unwrap();
    assert_eq!(
        ensure_active(&g.game, &g.installer(), false)
            .unwrap_err()
            .code,
        "mod_conflict"
    );
    assert_eq!(g.read(LOADER_NAME), b"UE4SS do jogador");
    assert!(record(&g).backups.is_empty(), "entrada pendente descartada");
}

#[test]
fn abertura_do_launcher_resolve_backup_pendente() {
    let g = fake_game();
    let mut rec = installed(&g);
    std::fs::create_dir_all(g.backup()).unwrap();
    std::fs::write(g.backup().join(OVERRIDE_NAME), b"ue4ss").unwrap();
    rec.backups = vec![pending(OVERRIDE_NAME), pending(LOADER_NAME)];
    g.installer().save_record(&rec).unwrap();
    reconcile_all(&g.installer());
    let r = record(&g);
    assert_eq!(r.backups.len(), 1);
    assert_eq!(r.backups[0].name, OVERRIDE_NAME);
    assert_eq!(r.backups[0].state, BackupState::Done);
}

// ----- revisão da Task 8: várias pastas do jogo (I2) -----

#[test]
fn registro_antigo_de_uma_pasta_so_carrega() {
    let g = fake_game();
    std::fs::create_dir_all(&g.mods).unwrap();
    let old = serde_json::json!({
        "version": "0.2.0", "protocol": 1, "gameDir": g.game.dir, "sha256": "b".repeat(64), "files": [],
        "layout": 2, "loader": { "sha256": modzip::sha_hex(LOADER), "size": LOADER.len() },
        "backups": [{ "name": "dwmapi.dll", "stored": "dwmapi.dll" }]
    });
    std::fs::write(g.mods.join("installed.json"), old.to_string()).unwrap();
    let r = record(&g);
    assert!(r.other_folders.is_empty());
    assert_eq!(r.backups[0].state, BackupState::Done);
    assert_eq!(
        leftover_folders(None, &g.installer()),
        vec![g.game.dir.clone()]
    );
}

/// Pasta B usando o mesmo `mods\` da pasta A.
fn second_game(a: &Game) -> Game {
    let mut b = fake_game();
    b.mods = a.mods.clone();
    b
}

#[tokio::test]
async fn trocar_de_pasta_tira_o_mod_da_antiga_e_devolve_o_arquivo_do_jogador() {
    let a = fake_game();
    let b = second_game(&a);
    let (hub, mods) = hub_with_mod(&a.mods).await;
    a.put(LOADER_NAME, b"UE4SS do jogador");
    let before_a = files_under(&a.win64());
    mods.ensure(&hub, 1, &a.game, CancellationToken::new())
        .await
        .unwrap();
    ensure_active(&a.game, &mods, true).unwrap();
    assert_eq!(a.read(LOADER_NAME), LOADER);

    // O jogador troca a pasta do jogo: antes de instalar em B, o mod sai de A.
    mods.ensure(&hub, 1, &b.game, CancellationToken::new())
        .await
        .unwrap();
    assert_eq!(a.read(LOADER_NAME), b"UE4SS do jogador");
    assert!(!a.win64().join(OVERRIDE_NAME).exists());
    assert!(!a.current().exists());
    assert_eq!(files_under(&a.win64()), before_a);
    let r = mods.installed().unwrap();
    assert_eq!(r.game_dir, b.game.dir);
    assert!(r.other_folders.is_empty());
    assert!(b.current().join("loader").join(LOADER_NAME).is_file());
    ensure_active(&b.game, &mods, false).unwrap();
    assert!(leftover_folders(Some(&b.game.dir), &mods).is_empty());
}

/// Letra de unidade que não existe nesta máquina.
#[cfg(windows)]
fn missing_drive() -> PathBuf {
    (b'G'..=b'Z')
        .rev()
        .map(|c| PathBuf::from(format!("{}:\\", c as char)))
        .find(|r| !r.exists())
        .expect("alguma letra de unidade livre")
}

#[cfg(windows)]
#[tokio::test]
async fn pasta_antiga_inacessivel_fica_registrada_e_nao_impede_a_nova() {
    let b = fake_game();
    let (hub, mods) = hub_with_mod(&b.mods).await;
    let gone_drive = missing_drive().join("Jogos").join("Hogwarts Legacy");
    let mut old = installed(&b);
    old.game_dir = gone_drive.clone();
    old.backups = vec![BackupEntry {
        name: LOADER_NAME.into(),
        stored: LOADER_NAME.into(),
        state: BackupState::Done,
    }];
    mods.save_record(&old).unwrap();

    mods.ensure(&hub, 1, &b.game, CancellationToken::new())
        .await
        .unwrap();
    ensure_active(&b.game, &mods, false).unwrap();
    let r = mods.installed().unwrap();
    assert_eq!(r.game_dir, b.game.dir);
    assert_eq!(r.other_folders.len(), 1);
    assert_eq!(r.other_folders[0].game_dir, gone_drive);
    assert_eq!(r.other_folders[0].backups, old.backups);
    assert_eq!(
        leftover_folders(Some(&b.game.dir), &mods),
        vec![gone_drive.clone()]
    );

    // "Remover mod do jogo": B sai, A continua inacessível e registrada.
    let res = remove_everywhere(&mods);
    assert_eq!(res.len(), 2);
    assert_eq!(res[0].game_dir, b.game.dir);
    assert!(res[0].error.is_none());
    assert_eq!(res[1].game_dir, gone_drive);
    assert_eq!(
        res[1].error.as_ref().unwrap().code,
        "mod_folder_unreachable"
    );
    let r = mods.installed().unwrap();
    assert!(r.removed);
    assert_eq!(r.other_folders.len(), 1);
}

#[test]
fn remover_limpa_todas_as_pastas_registradas() {
    let a = fake_game();
    let b = second_game(&a);
    // B é a pasta atual, ativa.
    let mut rec = installed(&b);
    activate(&b.win64(), &b.current(), &mut rec, false).unwrap();
    // A é uma pasta anterior: nosso carregador e `override.txt` ativos, o do jogador em `cbmp-backup\`.
    a.put(LOADER_NAME, LOADER);
    a.put(OVERRIDE_NAME, OVERRIDE_OURS);
    std::fs::create_dir_all(a.backup()).unwrap();
    std::fs::write(a.backup().join(LOADER_NAME), b"UE4SS do jogador").unwrap();
    std::fs::create_dir_all(a.current().join("ue4ss")).unwrap();
    rec.other_folders = vec![FolderRecord {
        game_dir: a.game.dir.clone(),
        version: "0.2.0".into(),
        loader: rec.loader.clone(),
        backups: vec![BackupEntry {
            name: LOADER_NAME.into(),
            stored: LOADER_NAME.into(),
            state: BackupState::Done,
        }],
        removed: false,
    }];
    // Uma terceira pasta que foi apagada do disco: sai do registro sem erro.
    let deleted = a.game.dir.parent().unwrap().join("apagada");
    rec.other_folders.push(FolderRecord {
        game_dir: deleted.clone(),
        version: "0.2.0".into(),
        loader: None,
        backups: vec![],
        removed: false,
    });
    b.installer().save_record(&rec).unwrap();

    let res = remove_everywhere(&b.installer());
    let dirs: Vec<_> = res.iter().map(|r| r.game_dir.clone()).collect();
    assert_eq!(dirs, vec![b.game.dir.clone(), a.game.dir.clone(), deleted]);
    assert!(res.iter().all(|r| r.error.is_none()), "{res:?}");
    assert_eq!(a.read(LOADER_NAME), b"UE4SS do jogador");
    assert!(!a.win64().join(OVERRIDE_NAME).exists());
    assert!(!a.backup().exists());
    assert!(!a.current().exists());
    assert!(!b.win64().join(LOADER_NAME).exists());
    let r = record(&b);
    assert!(r.removed && r.other_folders.is_empty());
    assert!(leftover_folders(None, &b.installer()).is_empty());
    // De novo: nada a fazer.
    assert!(remove_everywhere(&b.installer()).is_empty());
}

// ----- revisão da Task 8: lacunas de teste (M8) -----

#[test]
fn pasta_no_lugar_do_carregador_e_override_grande_sao_estranhos() {
    let g = fake_game();
    let mut rec = installed(&g);
    std::fs::create_dir_all(g.win64().join(LOADER_NAME).join("dentro")).unwrap();
    g.put(OVERRIDE_NAME, &vec![b'x'; 5000]);
    assert_eq!(inspect(&g.win64(), &rec).unwrap(), conflict(true, true));
    activate(&g.win64(), &g.current(), &mut rec, true).unwrap();
    assert_eq!(rec.backups.len(), 2);
    assert!(g.backup().join(LOADER_NAME).join("dentro").is_dir());
    assert_eq!(g.read(LOADER_NAME), LOADER);
}

#[test]
fn remover_de_novo_depois_de_remover_nao_faz_nada() {
    let g = fake_game();
    let mut rec = installed(&g);
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    remove_mod(&g.game, &g.installer()).unwrap();
    let after = record(&g);
    remove_mod(&g.game, &g.installer()).unwrap();
    assert_eq!(record(&g), after);
    assert!(remove_everywhere(&g.installer()).is_empty());
}
