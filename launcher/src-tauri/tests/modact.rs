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
    // Sem fixar versão: o launcher já é 0.2.0 (o `minLauncher` do pacote).
    let mods = ModInstaller::new(mods.to_path_buf(), modzip::pubkey(&k));
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
    assert_eq!(
        files_under(&g.backup()),
        vec!["cbmp-backup.json", "dwmapi.dll", "override.txt"]
    );
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
        sha256: None,
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
        sha256: None,
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
    let res = remove_everywhere(&mods, None);
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
            sha256: None,
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

    let res = remove_everywhere(&b.installer(), None);
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
    assert!(remove_everywhere(&b.installer(), None).is_empty());
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
    assert!(remove_everywhere(&g.installer(), None).is_empty());
}

// ----- comandos das Configurações (Task 9) -----

use cbmp_launcher_lib::commands::{
    mod_info, mod_logs_dir, old_folder_warning, old_folder_warning_once, remove_mod_with,
    resolve_conflict_with,
};

fn closed() -> FakeProcesses {
    FakeProcesses(vec!["steam.exe"])
}

fn game_open() -> FakeProcesses {
    FakeProcesses(vec!["HogwartsLegacy.exe"])
}

#[test]
fn remover_recusa_com_o_jogo_aberto_e_nao_mexe_em_nada() {
    let g = fake_game();
    let mut rec = installed(&g);
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    let e = remove_mod_with(Some(&g.game), &g.installer(), &game_open()).unwrap_err();
    assert_eq!(e.code, "game_running");
    assert_eq!(g.read(LOADER_NAME), LOADER);
    assert!(!record(&g).removed);
}

#[test]
fn remover_devolve_estado_e_resultado_por_pasta() {
    let g = fake_game();
    let mut rec = installed(&g);
    g.put(LOADER_NAME, b"UE4SS do jogador");
    cbmp_launcher_lib::modact::activate_with(&g.win64(), &g.current(), &mut rec, true, &mut |r| {
        g.installer().save_record(r)
    })
    .unwrap();
    let before = mod_info(Some(&g.game), &g.installer());
    assert_eq!(
        serde_json::to_value(&before).unwrap(),
        serde_json::json!({ "state": "installed", "version": "0.2.0", "otherFolders": [], "goneFolders": [] })
    );
    let out = remove_mod_with(Some(&g.game), &g.installer(), &closed()).unwrap();
    assert_eq!(out.status.status, ModStatus::NotInstalled);
    assert_eq!(out.folders.len(), 1);
    assert!(out.folders[0].error.is_none());
    assert_eq!(g.read(LOADER_NAME), b"UE4SS do jogador");
    let v = serde_json::to_value(&out).unwrap();
    assert_eq!(v["status"]["state"], "notInstalled");
    assert_eq!(v["folders"][0]["gameDir"], g.game.dir.display().to_string());
    assert_eq!(v["folders"][0]["error"], serde_json::Value::Null);
}

#[test]
fn resolver_conflito_so_com_confirmacao() {
    let g = fake_game();
    installed(&g);
    g.put(LOADER_NAME, b"UE4SS do jogador");
    // Jogo aberto: nem pergunta.
    let e = resolve_conflict_with(&g.game, &g.installer(), &game_open(), || {
        panic!("não pode abrir o diálogo com o jogo aberto")
    })
    .unwrap_err();
    assert_eq!(e.code, "game_running");
    // Cancelado: nada muda.
    let mut asked = 0;
    let st = resolve_conflict_with(&g.game, &g.installer(), &closed(), || {
        asked += 1;
        false
    })
    .unwrap();
    assert_eq!(asked, 1);
    assert_eq!(st.status, ModStatus::Conflict);
    assert_eq!(g.read(LOADER_NAME), b"UE4SS do jogador");
    // Aceito: o do jogador vai para `cbmp-backup\` e o nosso entra.
    let st = resolve_conflict_with(&g.game, &g.installer(), &closed(), || true).unwrap();
    assert_eq!(
        st.status,
        ModStatus::Installed {
            version: "0.2.0".into()
        }
    );
    assert_eq!(g.read(LOADER_NAME), LOADER);
    assert_eq!(
        std::fs::read(g.backup().join(LOADER_NAME)).unwrap(),
        b"UE4SS do jogador"
    );
    // Sem conflito: não pergunta.
    resolve_conflict_with(&g.game, &g.installer(), &closed(), || {
        panic!("sem conflito não há diálogo")
    })
    .unwrap();
}

#[test]
fn ue4ss_antigo_nao_abre_o_dialogo() {
    let g = fake_game();
    installed(&g);
    g.put("xinput1_3.dll", b"x");
    g.put("UE4SS.dll", b"x");
    let e = resolve_conflict_with(&g.game, &g.installer(), &closed(), || {
        panic!("UE4SS antigo não tem saída automática")
    })
    .unwrap_err();
    assert_eq!(e.code, "mod_conflict");
    assert!(e.message.contains("xinput1_3.dll"));
}

#[test]
fn pasta_de_logs_do_mod() {
    let g = fake_game();
    assert_eq!(mod_logs_dir(None).unwrap_err().code, "mod_logs_missing");
    let e = mod_logs_dir(Some(&g.game)).unwrap_err();
    assert_eq!(e.code, "mod_logs_missing");
    assert_eq!(e.message, "A pasta de logs do mod ainda não existe.");
    let logs = container_dir(&g.game).join("logs");
    std::fs::create_dir_all(&logs).unwrap();
    assert_eq!(mod_logs_dir(Some(&g.game)).unwrap(), logs);
}

#[test]
fn aviso_da_pasta_antiga_cita_as_pastas() {
    assert!(old_folder_warning(&[]).is_none());
    let w = old_folder_warning(&[PathBuf::from("D:\\Jogos\\HL")]).unwrap();
    assert_eq!(w.code, "mod_old_folder");
    assert!(w.message.contains("D:\\Jogos\\HL"), "{}", w.message);
    assert!(w.message.contains("Remover mod do jogo"));
}

// ----- revisão da Task 9: pasta do jogo movida (F1) — manifesto dentro da pasta do jogo -----

use cbmp_launcher_lib::modact::{adopt_folder, read_manifest, BACKUP_MANIFEST};

fn manifest_json(g: &Game) -> serde_json::Value {
    serde_json::from_slice(&std::fs::read(g.backup().join(BACKUP_MANIFEST)).unwrap()).unwrap()
}

/// A mesma árvore em outro lugar (Steam "mover pasta de instalação", renomear).
fn moved(a: Game, name: &str) -> Game {
    let to = a.game.dir.parent().unwrap().join(name);
    std::fs::rename(&a.game.dir, &to).unwrap();
    Game {
        game: GameInstall {
            exe: to.join("HogwartsLegacy.exe"),
            dir: to,
            store: Store::Manual,
        },
        mods: a.mods,
        _tmp: a._tmp,
    }
}

#[test]
fn manifesto_e_gravado_antes_de_mover_com_sha_e_estado() {
    let g = fake_game();
    installed(&g);
    g.put(LOADER_NAME, b"UE4SS do jogador");
    ensure_active(&g.game, &g.installer(), true).unwrap();
    let m = manifest_json(&g);
    assert_eq!(m["v"], 1);
    assert_eq!(m["entries"][0]["name"], "dwmapi.dll");
    assert_eq!(m["entries"][0]["stored"], "dwmapi.dll");
    assert_eq!(
        m["entries"][0]["sha256"],
        modzip::sha_hex(b"UE4SS do jogador")
    );
    assert_eq!(m["entries"][0]["state"], "done");
    assert_eq!(
        record(&g).backups[0].sha256.as_deref(),
        Some(modzip::sha_hex(b"UE4SS do jogador").as_str())
    );
    // Remover devolve o arquivo e apaga o manifesto (e a pasta vazia).
    remove_mod(&g.game, &g.installer()).unwrap();
    assert_eq!(g.read(LOADER_NAME), b"UE4SS do jogador");
    assert!(!g.backup().exists());
}

#[test]
fn manifesto_que_nao_grava_nao_move_o_arquivo_do_jogador() {
    let g = fake_game();
    installed(&g);
    g.put(LOADER_NAME, b"UE4SS do jogador");
    // `cbmp-backup` ocupado por um arquivo: o manifesto não tem onde ser gravado.
    std::fs::write(g.backup(), b"arquivo no lugar da pasta").unwrap();
    assert!(ensure_active(&g.game, &g.installer(), true).is_err());
    assert_eq!(
        g.read(LOADER_NAME),
        b"UE4SS do jogador",
        "nada saiu de Win64"
    );
    assert!(record(&g).backups.is_empty());
}

#[tokio::test]
async fn pasta_movida_depois_do_jogar_remover_devolve_o_arquivo_do_jogador() {
    let a = fake_game();
    let (hub, mods) = hub_with_mod(&a.mods).await;
    a.put(LOADER_NAME, b"UE4SS do jogador");
    let before = files_under(&a.win64());
    mods.ensure(&hub, 1, &a.game, CancellationToken::new())
        .await
        .unwrap();
    ensure_active(&a.game, &mods, true).unwrap();
    let a_dir = a.game.dir.clone();
    let b = moved(a, "HL movido");

    // Jogar na pasta nova: a antiga sumiu, mas os backups foram junto e são adotados.
    mods.ensure(&hub, 1, &b.game, CancellationToken::new())
        .await
        .unwrap();
    ensure_active(&b.game, &mods, false).unwrap();
    let r = mods.installed().unwrap();
    assert_eq!(r.game_dir, b.game.dir);
    assert_eq!(r.backups.len(), 1);
    assert_eq!(r.backups[0].name, LOADER_NAME);
    assert!(r.other_folders.iter().all(|f| f.game_dir != a_dir));

    let res = remove_everywhere(&mods, Some(&b.game.dir));
    assert!(res.iter().all(|f| f.error.is_none()), "{res:?}");
    assert_eq!(b.read(LOADER_NAME), b"UE4SS do jogador");
    assert!(!b.backup().exists());
    assert!(!b.current().exists());
    assert_eq!(files_under(&b.win64()), before);
    assert!(leftover_folders(None, &mods).is_empty());
}

#[tokio::test]
async fn pasta_movida_sem_jogar_estado_mostra_os_backups_e_remover_devolve() {
    let a = fake_game();
    let (hub, mods) = hub_with_mod(&a.mods).await;
    a.put(LOADER_NAME, b"UE4SS do jogador");
    a.put(OVERRIDE_NAME, b"ue4ss");
    let before = files_under(&a.win64());
    mods.ensure(&hub, 1, &a.game, CancellationToken::new())
        .await
        .unwrap();
    ensure_active(&a.game, &mods, true).unwrap();
    let b = moved(a, "HL renomeado");

    // Configurações abertas com a pasta nova: o manifesto de B é adotado.
    adopt_folder(&b.game.dir, &mods);
    let r = mods.installed().unwrap();
    let fb = r
        .other_folders
        .iter()
        .find(|f| f.game_dir == b.game.dir)
        .expect("pasta nova registrada com os backups");
    assert_eq!(fb.backups.len(), 2);
    assert!(
        r.backups.is_empty(),
        "a pasta antiga (sumiu) não guarda mais esses backups"
    );
    assert!(leftover_folders(None, &mods).contains(&b.game.dir));

    // "Remover mod do jogo" com a pasta nova: o do jogador volta, nada nosso fica.
    let out = remove_mod_with(Some(&b.game), &mods, &closed()).unwrap();
    assert!(
        out.folders.iter().all(|f| f.error.is_none()),
        "{:?}",
        out.folders
    );
    assert_eq!(b.read(LOADER_NAME), b"UE4SS do jogador");
    assert_eq!(b.read(OVERRIDE_NAME), b"ue4ss");
    assert!(!b.backup().exists());
    assert!(!b.current().exists());
    assert_eq!(files_under(&b.win64()), before);
    assert!(leftover_folders(None, &mods).is_empty());
}

#[test]
fn pasta_sumida_com_backups_fica_registrada() {
    let g = fake_game();
    let mut rec = installed(&g);
    let gone = g.game.dir.parent().unwrap().join("apagada");
    rec.game_dir = gone.clone();
    rec.backups = vec![BackupEntry {
        name: LOADER_NAME.into(),
        stored: LOADER_NAME.into(),
        sha256: Some(modzip::sha_hex(b"x")),
        state: BackupState::Done,
    }];
    g.installer().save_record(&rec).unwrap();
    let res = remove_everywhere(&g.installer(), None);
    assert_eq!(res.len(), 1);
    assert_eq!(
        res[0].error.as_ref().unwrap().code,
        "mod_folder_unreachable"
    );
    let r = record(&g);
    assert_eq!(
        r.backups.len(),
        1,
        "não descarta: os arquivos podem estar em outro lugar"
    );
    assert!(!r.removed);
}

/// Grava um manifesto à mão em `cbmp-backup\`.
fn plant_manifest(g: &Game, entries: serde_json::Value) {
    std::fs::create_dir_all(g.backup()).unwrap();
    std::fs::write(
        g.backup().join(BACKUP_MANIFEST),
        serde_json::json!({ "v": 1, "entries": entries }).to_string(),
    )
    .unwrap();
}

#[test]
fn manifesto_adulterado_e_recusado() {
    let g = fake_game();
    let mut rec = installed(&g);
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    g.installer().save_record(&rec).unwrap();
    let outside = g.game.dir.parent().unwrap().join("fora.dll");
    std::fs::write(&outside, b"fora").unwrap();
    std::fs::write(g.win64().join("kernel32.dll"), b"do jogo").unwrap();
    std::fs::create_dir_all(g.backup()).unwrap();
    std::fs::write(g.backup().join("dwmapi.dll"), b"backup").unwrap();
    std::fs::write(g.backup().join("x.dll"), b"x").unwrap();
    let sha = |d: &[u8]| modzip::sha_hex(d);
    plant_manifest(
        &g,
        serde_json::json!([
            // Nome fora da lista fixa, caminhos, travessia, sha errado, sufixo inválido.
            { "name": "kernel32.dll", "stored": "x.dll", "sha256": sha(b"x"), "state": "done" },
            { "name": "dwmapi.dll", "stored": "..\\fora.dll", "sha256": sha(b"fora"), "state": "done" },
            { "name": "dwmapi.dll", "stored": "../fora.dll", "sha256": sha(b"fora"), "state": "done" },
            { "name": "..\\..\\fora.dll", "stored": "dwmapi.dll", "sha256": sha(b"backup"), "state": "done" },
            { "name": "dwmapi.dll", "stored": "dwmapi.dll", "sha256": sha(b"outro"), "state": "done" },
            { "name": "dwmapi.dll", "stored": "dwmapi.dll.x", "sha256": sha(b"backup"), "state": "done" },
            { "name": "override.txt", "stored": "x.dll", "sha256": sha(b"x"), "state": "done" },
            { "name": "dwmapi.dll", "stored": "C:x", "sha256": sha(b"x"), "state": "done" }
        ]),
    );
    assert!(read_manifest(&g.win64()).is_empty());
    adopt_folder(&g.game.dir, &g.installer());
    assert!(record(&g).backups.is_empty());
    remove_mod(&g.game, &g.installer()).unwrap();
    assert_eq!(std::fs::read(&outside).unwrap(), b"fora");
    assert_eq!(g.read("kernel32.dll"), b"do jogo");
    assert!(!g.win64().join(LOADER_NAME).exists(), "nada devolvido");
    assert_eq!(
        std::fs::read(g.backup().join("dwmapi.dll")).unwrap(),
        b"backup"
    );

    // Manifesto com uma entrada boa e uma ruim: só a boa vale.
    plant_manifest(
        &g,
        serde_json::json!([
            { "name": "dwmapi.dll", "stored": "dwmapi.dll", "sha256": sha(b"backup"), "state": "done" },
            { "name": "override.txt", "stored": "../../x", "sha256": sha(b"x"), "state": "done" }
        ]),
    );
    let got = read_manifest(&g.win64());
    assert_eq!(got.len(), 1);
    assert_eq!(got[0].stored, "dwmapi.dll");
    // Campos a mais, versão errada ou lixo: recusado inteiro.
    for bad in [
        r#"{"v":2,"entries":[]}"#,
        r#"{"v":1,"entries":[],"x":1}"#,
        "lixo",
    ] {
        std::fs::write(g.backup().join(BACKUP_MANIFEST), bad.as_bytes()).unwrap();
        assert!(read_manifest(&g.win64()).is_empty(), "{bad}");
    }
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
fn manifesto_ou_backup_por_juncao_e_recusado() {
    let g = fake_game();
    let elsewhere = g.game.dir.parent().unwrap().join("fora");
    std::fs::create_dir_all(&elsewhere).unwrap();
    std::fs::write(elsewhere.join("dwmapi.dll"), b"fora").unwrap();
    std::fs::write(
        elsewhere.join(BACKUP_MANIFEST),
        serde_json::json!({ "v": 1, "entries": [
            { "name": "dwmapi.dll", "stored": "dwmapi.dll", "sha256": modzip::sha_hex(b"fora"), "state": "done" }
        ]})
        .to_string(),
    )
    .unwrap();
    // `cbmp-backup` é uma junção para fora da pasta do jogo.
    junction(&g.backup(), &elsewhere);
    assert!(read_manifest(&g.win64()).is_empty());
    let mut rec = installed(&g);
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    g.installer().save_record(&rec).unwrap();
    remove_mod(&g.game, &g.installer()).unwrap();
    assert!(
        !g.win64().join(LOADER_NAME).exists(),
        "nada devolvido de fora"
    );
    assert_eq!(
        std::fs::read(elsewhere.join("dwmapi.dll")).unwrap(),
        b"fora"
    );
    std::fs::remove_dir(g.backup()).unwrap();

    // Backup que é uma junção (pasta) para fora.
    std::fs::create_dir_all(g.backup()).unwrap();
    junction(&g.backup().join("dwmapi.dll"), &elsewhere);
    plant_manifest(
        &g,
        serde_json::json!([{ "name": "dwmapi.dll", "stored": "dwmapi.dll", "state": "done" }]),
    );
    assert!(read_manifest(&g.win64()).is_empty());
    std::fs::remove_dir(g.backup().join("dwmapi.dll")).unwrap();
}

// ----- Task 10 revisada: pasta protegida, junções e "Esquecer pasta" -----

#[cfg(windows)]
#[path = "common/deny.rs"]
mod deny;

use cbmp_launcher_lib::game::resolve_final;
use cbmp_launcher_lib::modact::{forget_folder, gone_folders, GoneFolder};

#[cfg(windows)]
fn protected_message(g: &Game) -> String {
    format!(
        "O launcher não tem permissão para gravar na pasta do jogo ({}). Nesta versão de testes, instale o jogo numa \
         pasta em que o seu usuário possa gravar (por exemplo, outra biblioteca da Steam ou outra pasta na Epic Games) \
         e tente de novo.",
        g.game.dir.display()
    )
}

#[cfg(windows)]
#[test]
fn ativar_sem_permissao_em_win64_da_mod_folder_protected() {
    let g = fake_game();
    installed(&g);
    let _deny = deny::Deny::new(&g.win64(), "(W)");
    let e = ensure_active(&g.game, &g.installer(), false).unwrap_err();
    assert_eq!(e.code, "mod_folder_protected");
    assert_eq!(e.message, protected_message(&g));
    assert!(!g.win64().join(LOADER_NAME).exists());
}

#[cfg(windows)]
#[test]
fn remover_sem_permissao_da_mod_folder_protected_e_nao_marca_removido() {
    let g = fake_game();
    let mut rec = installed(&g);
    activate(&g.win64(), &g.current(), &mut rec, false).unwrap();
    g.installer().save_record(&rec).unwrap();
    let e = {
        let _deny_file = deny::Deny::new(&g.win64().join(LOADER_NAME), "(D)");
        let _deny_dir = deny::Deny::new(&g.win64(), "(DC)");
        remove_mod(&g.game, &g.installer()).unwrap_err()
    };
    // (O `icacls /deny` também nega SYNCHRONIZE: as leituras de conferência vêm depois de tirar a negação.)
    assert_eq!(e.code, "mod_folder_protected");
    assert_eq!(e.message, protected_message(&g));
    assert_eq!(g.read(LOADER_NAME), LOADER);
    assert!(!record(&g).removed);
}

#[cfg(windows)]
#[test]
fn pasta_do_jogo_por_juncao_e_resolvida_para_o_caminho_final() {
    let g = fake_game();
    let real_parent = g.game.dir.parent().unwrap().to_path_buf();
    // Junção numa pasta temporária própria (nunca direto no %TEMP%): o `drop` apaga só o link, não o alvo.
    let links = tempfile::tempdir().unwrap();
    let via = links.path().join("via-juncao");
    junction(&via, &real_parent);
    let through = GameInstall {
        dir: via.join("Hogwarts Legacy"),
        exe: via.join("Hogwarts Legacy").join("HogwartsLegacy.exe"),
        store: Store::Manual,
    };
    let r = resolve_final(&through);
    assert_eq!(
        r.dir.canonicalize().unwrap(),
        g.game.dir.canonicalize().unwrap()
    );
    assert!(!r.dir.starts_with(&via), "{}", r.dir.display());
    assert!(
        !r.dir.to_string_lossy().starts_with(r"\\?\"),
        "{}",
        r.dir.display()
    );
    assert_eq!(r.exe, r.dir.join("HogwartsLegacy.exe"));
    // Ativar e remover pelo caminho resolvido funcionam normalmente (junção só no caminho escolhido).
    let gr = Game {
        game: r.clone(),
        mods: g.mods.clone(),
        _tmp: tempfile::tempdir().unwrap(),
    };
    let mut rec = installed(&gr);
    assert_eq!(rec.game_dir, r.dir);
    activate(&gr.win64(), &gr.current(), &mut rec, false).unwrap();
    gr.installer().save_record(&rec).unwrap();
    remove_mod(&gr.game, &gr.installer()).unwrap();
    assert!(!g.win64().join(LOADER_NAME).exists());
    std::fs::remove_dir(&via).unwrap();
}

#[cfg(windows)]
#[test]
fn registro_gravado_pela_juncao_e_a_mesma_pasta_do_jogo() {
    use cbmp_launcher_lib::modact::{leave_other_folders, rebase_record};
    let g = fake_game();
    let real_parent = g.game.dir.parent().unwrap().to_path_buf();
    // Junção numa pasta temporária própria (nunca direto no %TEMP%): o `drop` apaga só o link, não o alvo.
    let links = tempfile::tempdir().unwrap();
    let via = links.path().join("via-juncao-2");
    junction(&via, &real_parent);
    let through = GameInstall {
        dir: via.join("Hogwarts Legacy"),
        exe: via.join("Hogwarts Legacy").join("HogwartsLegacy.exe"),
        store: Store::Manual,
    };
    let real = resolve_final(&g.game);
    // Registro antigo pelo caminho da junção (antes de o launcher resolver o caminho final).
    let gj = Game {
        game: through.clone(),
        mods: g.mods.clone(),
        _tmp: tempfile::tempdir().unwrap(),
    };
    let mut rec = installed(&gj);
    activate(&gj.win64(), &gj.current(), &mut rec, false).unwrap();
    gj.installer().save_record(&rec).unwrap();
    // Não aparece como pasta anterior, nem como sumida.
    assert!(leftover_folders(Some(&real.dir), &g.installer()).is_empty());
    assert!(gone_folders(Some(&real.dir), &g.installer()).is_empty());
    // Antes de instalar: o registro passa ao caminho final e nada sai da pasta.
    assert!(leave_other_folders(&real, &g.installer()).is_empty());
    assert_eq!(record(&g).game_dir, real.dir);
    assert!(!record(&g).removed);
    assert_eq!(g.read(LOADER_NAME), LOADER);
    rebase_record(&real.dir, &g.installer());
    assert_eq!(record(&g).game_dir, real.dir);
    std::fs::remove_dir(&via).unwrap();
}

#[cfg(windows)]
#[test]
fn cbmp_backup_por_juncao_nao_recebe_o_arquivo_do_jogador() {
    let g = fake_game();
    installed(&g);
    g.put(LOADER_NAME, b"UE4SS do jogador");
    let elsewhere = g.game.dir.parent().unwrap().join("fora");
    std::fs::create_dir_all(&elsewhere).unwrap();
    junction(&g.backup(), &elsewhere);
    assert!(ensure_active(&g.game, &g.installer(), true).is_err());
    assert_eq!(
        g.read(LOADER_NAME),
        b"UE4SS do jogador",
        "não saiu de Win64"
    );
    assert_eq!(
        std::fs::read_dir(&elsewhere).unwrap().count(),
        0,
        "nada fora"
    );
    std::fs::remove_dir(g.backup()).unwrap();
}

#[test]
fn esquecer_pasta_que_nao_existe_mais() {
    let g = fake_game();
    let mut rec = installed(&g);
    let gone = g.game.dir.parent().unwrap().join("desinstalada");
    rec.other_folders = vec![FolderRecord {
        game_dir: gone.clone(),
        version: "0.2.0".into(),
        loader: None,
        backups: vec![
            BackupEntry {
                name: LOADER_NAME.into(),
                stored: LOADER_NAME.into(),
                sha256: Some(modzip::sha_hex(b"x")),
                state: BackupState::Done,
            },
            BackupEntry {
                name: OVERRIDE_NAME.into(),
                stored: OVERRIDE_NAME.into(),
                sha256: Some(modzip::sha_hex(b"y")),
                state: BackupState::Done,
            },
        ],
        removed: false,
    }];
    g.installer().save_record(&rec).unwrap();
    assert_eq!(
        gone_folders(Some(&g.game.dir), &g.installer()),
        vec![GoneFolder {
            game_dir: gone.clone(),
            backups: vec!["dwmapi.dll".into(), "override.txt".into()],
        }]
    );
    let info = serde_json::to_value(mod_info(Some(&g.game), &g.installer())).unwrap();
    assert_eq!(info["goneFolders"][0]["backups"][1], "override.txt");
    // A pasta que existe não pode ser esquecida.
    assert_eq!(
        forget_folder(&g.game.dir, &g.installer()).unwrap_err().code,
        "mod_folder_present"
    );
    forget_folder(&gone, &g.installer()).unwrap();
    let r = record(&g);
    assert!(r.other_folders.is_empty());
    assert!(!r.removed, "a pasta atual continua");
    assert!(gone_folders(None, &g.installer()).is_empty());
    assert!(leftover_folders(Some(&g.game.dir), &g.installer()).is_empty());
}

#[cfg(windows)]
#[test]
fn pasta_inacessivel_ou_registro_atual_sumido() {
    let g = fake_game();
    let mut rec = installed(&g);
    // Disco ausente: não é "sumida", não pode ser esquecida.
    let offline = missing_drive().join("Jogos").join("HL");
    rec.game_dir = offline.clone();
    g.installer().save_record(&rec).unwrap();
    assert!(gone_folders(None, &g.installer()).is_empty());
    assert_eq!(
        forget_folder(&offline, &g.installer()).unwrap_err().code,
        "mod_folder_present"
    );
    // Registro atual numa pasta apagada: esquecer marca removido e limpa os backups.
    let gone = g.game.dir.parent().unwrap().join("apagada");
    rec.game_dir = gone.clone();
    rec.backups = vec![BackupEntry {
        name: LOADER_NAME.into(),
        stored: LOADER_NAME.into(),
        sha256: None,
        state: BackupState::Done,
    }];
    g.installer().save_record(&rec).unwrap();
    assert_eq!(gone_folders(None, &g.installer())[0].game_dir, gone);
    forget_folder(&gone, &g.installer()).unwrap();
    let r = record(&g);
    assert!(r.removed && r.backups.is_empty() && r.loader.is_none());
    assert!(leftover_folders(None, &g.installer()).is_empty());
}

#[test]
fn aviso_de_pasta_anterior_sai_uma_vez_por_execucao() {
    let flag = std::sync::atomic::AtomicBool::new(false);
    assert!(
        old_folder_warning_once(&flag, &[]).is_none(),
        "sem pasta não gasta o aviso"
    );
    let left = [PathBuf::from("D:\\HL")];
    assert_eq!(
        old_folder_warning_once(&flag, &left).unwrap().code,
        "mod_old_folder"
    );
    assert!(old_folder_warning_once(&flag, &left).is_none());
}
