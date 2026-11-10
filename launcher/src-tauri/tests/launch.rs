//! Arquivo de conexão (ACL só do usuário, remoção em 60 s), abertura/simulação e a orquestração do `play` contra um
//! Hub + `cb-server` falsos (axum em 127.0.0.1).

use axum::extract::{Path as AxPath, State};
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use cbmp_launcher_lib::auth::{Auth, Clock, MemoryVault, TokenVault};
use cbmp_launcher_lib::content::{ContentStore, Limits};
use cbmp_launcher_lib::game::{GameInstall, Store};
use cbmp_launcher_lib::hub::HubClient;
use cbmp_launcher_lib::launch::{
    self, acl, connect_json, launch, remove_after, write_connect_file, ConnectInfo, LaunchOutcome,
    CONNECT_TTL,
};
use cbmp_launcher_lib::modpkg::ModInstaller;
use cbmp_launcher_lib::play::{play, PlayContext, PlayGate, Stage, TICKET_RENEW_AFTER_SECS};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio_util::sync::CancellationToken;

const TICKET_MARK: &str = "TICKETSECRETO";

fn info(content_dir: &Path) -> ConnectInfo {
    ConnectInfo {
        host: "127.0.0.1".into(),
        port: 7777,
        ticket: format!("{TICKET_MARK}_1"),
        server_id: "srv_A".into(),
        content_dir: content_dir.to_path_buf(),
        protocol: 3,
    }
}

// ----- connect.json -----

#[test]
fn connect_json_tem_os_campos_do_contrato() {
    let tmp = tempfile::tempdir().unwrap();
    let i = info(tmp.path());
    let v: serde_json::Value = serde_json::from_slice(&connect_json(&i)).unwrap();
    assert_eq!(v["host"], "127.0.0.1");
    assert_eq!(v["port"], 7777);
    assert_eq!(v["ticket"], format!("{TICKET_MARK}_1"));
    assert_eq!(v["serverId"], "srv_A");
    assert_eq!(v["protocol"], 3);
    assert_eq!(v["contentDir"], tmp.path().display().to_string());
    // A pasta de conteúdo compartilha arquivos com o cache (hard links): o mod só lê.
    assert_eq!(v["contentReadOnly"], true);
    // `Debug` nunca mostra o ticket (logs).
    assert!(!format!("{i:?}").contains(TICKET_MARK));
}

#[cfg(windows)]
#[test]
fn connect_json_tem_dacl_so_do_usuario_atual() {
    let tmp = tempfile::tempdir().unwrap();
    let run = tmp.path().join("run");
    let path = write_connect_file(&run, &info(tmp.path())).unwrap();
    assert_eq!(path, run.join("connect.json"));
    assert_eq!(
        std::fs::read(&path).unwrap(),
        connect_json(&info(tmp.path()))
    );
    let me = acl::current_user_sid().unwrap();
    assert!(me.starts_with("S-1-5-"), "{me}");
    let dacl = acl::read_dacl(&path).unwrap();
    assert!(dacl.protected, "DACL não pode herdar da pasta");
    assert_eq!(dacl.aces.len(), 1, "{:?}", dacl.aces);
    assert!(dacl.aces[0].allowed);
    assert_eq!(dacl.aces[0].sid, me);
    // GENERIC_ALL vira FILE_ALL_ACCESS no arquivo.
    assert_eq!(dacl.aces[0].mask, 0x001F_01FF);
    // Contraprova: um arquivo comum da mesma pasta herda outras entradas (o teste distingue os dois casos).
    let plain = run.join("comum.txt");
    std::fs::write(&plain, b"x").unwrap();
    let inherited = acl::read_dacl(&plain).unwrap();
    assert!(!inherited.protected);
    assert!(
        inherited.aces.iter().any(|a| a.sid != me),
        "{:?}",
        inherited.aces
    );
    std::fs::remove_file(&plain).unwrap();
    // Sem `.tmp` para trás.
    let names: Vec<_> = std::fs::read_dir(&run)
        .unwrap()
        .map(|e| e.unwrap().file_name())
        .collect();
    assert_eq!(names, vec![std::ffi::OsString::from("connect.json")]);

    // Regravar (outro play) mantém a ACL restrita.
    let mut i2 = info(tmp.path());
    i2.ticket = format!("{TICKET_MARK}_2");
    write_connect_file(&run, &i2).unwrap();
    let dacl = acl::read_dacl(&path).unwrap();
    assert!(dacl.protected && dacl.aces.len() == 1 && dacl.aces[0].sid == me);
}

#[tokio::test(start_paused = true)]
async fn connect_json_e_apagado_depois_de_60_s() {
    assert_eq!(CONNECT_TTL, Duration::from_secs(60));
    let tmp = tempfile::tempdir().unwrap();
    let i = info(tmp.path());
    let path = write_connect_file(tmp.path(), &i).unwrap();
    let task = tokio::spawn(remove_after(path.clone(), connect_json(&i), CONNECT_TTL));
    tokio::time::sleep(Duration::from_secs(59)).await;
    assert!(path.exists(), "ainda dentro dos 60 s");
    tokio::time::sleep(Duration::from_secs(2)).await;
    task.await.unwrap();
    assert!(!path.exists(), "apagado depois de 60 s");
}

#[tokio::test(start_paused = true)]
async fn remocao_nao_apaga_o_arquivo_de_um_play_mais_novo() {
    let tmp = tempfile::tempdir().unwrap();
    let old = info(tmp.path());
    let path = write_connect_file(tmp.path(), &old).unwrap();
    let task = tokio::spawn(remove_after(path.clone(), connect_json(&old), CONNECT_TTL));
    tokio::time::sleep(Duration::from_secs(30)).await;
    let mut new = info(tmp.path());
    new.ticket = format!("{TICKET_MARK}_novo");
    write_connect_file(tmp.path(), &new).unwrap();
    tokio::time::sleep(Duration::from_secs(31)).await;
    task.await.unwrap();
    assert_eq!(std::fs::read(&path).unwrap(), connect_json(&new));
}

#[test]
fn sobra_de_connect_json_sai_na_abertura() {
    let tmp = tempfile::tempdir().unwrap();
    let path = write_connect_file(tmp.path(), &info(tmp.path())).unwrap();
    launch::remove_stale(tmp.path());
    assert!(!path.exists());
    launch::remove_stale(&tmp.path().join("nao_existe"));
}

// ----- abrir / simular -----

#[test]
fn simular_devolve_preview_sem_ticket() {
    let tmp = tempfile::tempdir().unwrap();
    let i = info(tmp.path());
    let out = launch(None, true, &i).unwrap();
    let LaunchOutcome::Simulated(preview) = &out else {
        panic!("esperado Simulated: {out:?}");
    };
    assert_eq!(preview.server_id, "srv_A");
    assert_eq!(preview.port, 7777);
    let v = serde_json::to_value(&out).unwrap();
    assert_eq!(v["kind"], "simulated");
    assert_eq!(v["preview"]["serverId"], "srv_A");
    assert_eq!(v["preview"]["contentDir"], tmp.path().display().to_string());
    let text = v.to_string();
    assert!(
        !text.contains(TICKET_MARK) && !text.contains("ticket"),
        "{text}"
    );
}

#[test]
fn abrir_sem_jogo_e_game_not_found() {
    let tmp = tempfile::tempdir().unwrap();
    assert_eq!(
        launch(None, false, &info(tmp.path())).unwrap_err().code,
        "game_not_found"
    );
    // Instalação que sumiu do disco depois de localizada.
    let g = GameInstall {
        dir: tmp.path().join("sumiu"),
        exe: tmp.path().join("sumiu").join("HogwartsLegacy.exe"),
        store: Store::Manual,
    };
    assert_eq!(
        launch(Some(&g), false, &info(tmp.path())).unwrap_err().code,
        "game_not_found"
    );
}

/// Abre um executável inofensivo (cópia do `whoami.exe`) no lugar do jogo: nunca o jogo real.
#[cfg(windows)]
#[test]
fn abrir_inicia_o_executavel_sem_ticket_na_linha_de_comando() {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("HL");
    std::fs::create_dir_all(dir.join("Phoenix").join("Binaries").join("Win64")).unwrap();
    let sys = std::env::var_os("SystemRoot").unwrap();
    let exe = dir.join("HogwartsLegacy.exe");
    std::fs::copy(Path::new(&sys).join("System32").join("whoami.exe"), &exe).unwrap();
    let g = GameInstall {
        dir: dir.clone(),
        exe: exe.clone(),
        store: Store::Manual,
    };
    let cmd = launch::command_for(&g);
    assert_eq!(cmd.get_program(), exe.as_os_str());
    assert_eq!(cmd.get_args().count(), 0, "nada na linha de comando");
    assert_eq!(cmd.get_current_dir(), Some(dir.as_path()));
    assert!(cmd
        .get_envs()
        .all(|(k, v)| !k.to_string_lossy().contains(TICKET_MARK)
            && !v.is_some_and(|v| v.to_string_lossy().contains(TICKET_MARK))));
    assert!(matches!(
        launch(Some(&g), false, &info(tmp.path())).unwrap(),
        LaunchOutcome::Started
    ));
}

// ----- play: Hub + cb-server falsos -----

const PROTOCOL: u32 = 3;
const SERVER: &str = "srv_A";

#[derive(Default)]
struct Fake {
    tickets: u32,
    /// Segundos somados ao relógio do launcher a cada arquivo servido (download demorado).
    advance_per_file: i64,
    files: std::collections::HashMap<String, Vec<u8>>,
    manifest: serde_json::Value,
    online: bool,
    port: u16,
    /// Relógio do launcher (o mesmo do `Auth` e do `play`).
    clock: Arc<AtomicI64>,
}

type Shared = Arc<Mutex<Fake>>;

struct Env {
    _tmp: tempfile::TempDir,
    root: PathBuf,
    fake: Shared,
    clock: Arc<AtomicI64>,
    auth: Arc<Auth>,
}

async fn refresh() -> Response {
    Json(serde_json::json!({
        "accessToken": "at_1", "accessExpiresAt": 4_000_000_000i64,
        "refreshToken": "rt_2", "refreshExpiresAt": 4_000_000_000i64
    }))
    .into_response()
}

async fn ticket(State(s): State<Shared>) -> Response {
    let mut f = s.lock().unwrap();
    f.tickets += 1;
    Json(serde_json::json!({ "ticket": format!("{TICKET_MARK}_{}", f.tickets) })).into_response()
}

async fn server(State(s): State<Shared>, AxPath(id): AxPath<String>) -> Response {
    let f = s.lock().unwrap();
    if id != SERVER {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({ "error": "not_found", "message": "x" })),
        )
            .into_response();
    }
    let mut v = serde_json::json!({
        "id": SERVER, "name": "Castelo", "description": "", "tags": [], "locale": "pt-BR",
        "featured": true, "online": f.online,
    });
    if f.online {
        v["players"] = 1.into();
        v["maxPlayers"] = 10.into();
        v["host"] = "127.0.0.1".into();
        v["gamePort"] = 7777.into();
        v["httpPort"] = f.port.into();
        v["version"] = "1.0.0".into();
        v["protocol"] = PROTOCOL.into();
    }
    Json(v).into_response()
}

async fn manifest(State(s): State<Shared>) -> Response {
    Json(s.lock().unwrap().manifest.clone()).into_response()
}

fn shared_clock(env_clock: &Arc<AtomicI64>) -> Clock {
    let c = env_clock.clone();
    Arc::new(move || c.load(Ordering::SeqCst))
}

async fn file(State(s): State<Shared>, AxPath(sha): AxPath<String>) -> Response {
    let data = {
        let f = s.lock().unwrap();
        f.clock.fetch_add(f.advance_per_file, Ordering::SeqCst);
        f.files.get(&sha).cloned()
    };
    match data {
        Some(d) => d.into_response(),
        None => StatusCode::NOT_FOUND.into_response(),
    }
}

fn sha_hex(d: &[u8]) -> String {
    Sha256::digest(d)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

async fn env() -> Env {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().to_path_buf();
    let clock = Arc::new(AtomicI64::new(1_000));
    let fake: Shared = Arc::new(Mutex::new(Fake {
        online: true,
        clock: clock.clone(),
        ..Fake::default()
    }));
    let files: Vec<(&str, &[u8])> = vec![("client/main.lua", b"print(1)"), ("data/a.bin", b"abc")];
    {
        let mut f = fake.lock().unwrap();
        let mut entries = Vec::new();
        let mut total = 0u64;
        for (p, d) in &files {
            f.files.insert(sha_hex(d), d.to_vec());
            entries.push(serde_json::json!({ "path": p, "sha256": sha_hex(d), "size": d.len() }));
            total += d.len() as u64;
        }
        f.manifest = serde_json::json!({
            "serverId": SERVER, "protocol": PROTOCOL,
            "resources": [{ "name": "core", "version": "1", "files": entries }],
            "totalSize": total
        });
    }
    let app = Router::new()
        .route("/v1/auth/refresh", post(refresh))
        .route("/v1/tickets", post(ticket))
        .route("/v1/servers/{id}", get(server))
        .route("/cb/manifest", get(manifest))
        .route("/cb/files/{sha}", get(file))
        .with_state(fake.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = listener.local_addr().unwrap().port();
    fake.lock().unwrap().port = port;
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    let vault = Arc::new(MemoryVault::default());
    vault.set("rt_1").unwrap();
    let hub = HubClient::new(&format!("http://127.0.0.1:{port}")).unwrap();
    let auth = Arc::new(Auth::with_clock(hub, vault, shared_clock(&clock)));
    Env {
        _tmp: tmp,
        root,
        fake,
        clock,
        auth,
    }
}

impl Env {
    fn ctx(&self, simulate: bool, game: Option<GameInstall>) -> PlayContext {
        PlayContext {
            auth: self.auth.clone(),
            content: ContentStore::new(self.root.join("cache"), self.root.join("servers"))
                .with_retry_delay(Duration::from_millis(5)),
            mods: ModInstaller::new(self.root.join("mods"), "chave-nao-usada".into()),
            run_dir: self.root.join("run"),
            clock: shared_clock(&self.clock),
            simulate,
            game,
            limits: Limits::new(2048),
            cache_limit_bytes: 10_240 * 1024 * 1024,
            keep: HashSet::new(),
            connect_ttl: CONNECT_TTL,
        }
    }

    async fn run(
        &self,
        ctx: &PlayContext,
    ) -> (
        cbmp_launcher_lib::error::AppResult<LaunchOutcome>,
        Vec<Stage>,
    ) {
        let stages = Arc::new(Mutex::new(Vec::new()));
        let st = stages.clone();
        let r = play(
            ctx,
            SERVER,
            &move |s| st.lock().unwrap().push(s),
            |_| {},
            CancellationToken::new(),
        )
        .await;
        let v = stages.lock().unwrap().clone();
        (r, v)
    }

    fn connect(&self) -> serde_json::Value {
        serde_json::from_slice(&std::fs::read(self.root.join("run").join("connect.json")).unwrap())
            .unwrap()
    }
}

#[tokio::test]
async fn play_simulado_percorre_as_etapas_e_grava_a_conexao() {
    let e = env().await;
    let (r, stages) = e.run(&e.ctx(true, None)).await;
    let out = r.unwrap();
    assert_eq!(
        stages,
        vec![
            Stage::Ticket,
            Stage::Manifest,
            Stage::Download,
            Stage::Mount,
            Stage::Mod,
            Stage::Connect,
            Stage::Launch
        ]
    );
    let LaunchOutcome::Simulated(p) = &out else {
        panic!("{out:?}")
    };
    let mounted = e.root.join("servers").join(SERVER);
    assert_eq!(p.content_dir, mounted);
    assert_eq!((p.port, p.protocol), (7777, PROTOCOL));
    assert_eq!(
        std::fs::read(mounted.join("core").join("client").join("main.lua")).unwrap(),
        b"print(1)"
    );
    let c = e.connect();
    assert_eq!(c["ticket"], format!("{TICKET_MARK}_1"));
    assert_eq!(c["serverId"], SERVER);
    assert_eq!(c["contentDir"], mounted.display().to_string());
    assert_eq!(e.fake.lock().unwrap().tickets, 1);
    assert!(!serde_json::to_string(&out).unwrap().contains(TICKET_MARK));
    // Etapas serializam como texto curto para a UI.
    assert_eq!(serde_json::to_value(Stage::Mod).unwrap(), "mod");
}

#[tokio::test]
async fn ticket_renovado_quando_passa_de_240_s_desde_a_emissao() {
    assert_eq!(TICKET_RENEW_AFTER_SECS, 240);
    let e = env().await;
    // 2 arquivos × 121 s = 242 s entre a emissão e a conexão.
    e.fake.lock().unwrap().advance_per_file = 121;
    let (r, _) = e.run(&e.ctx(true, None)).await;
    r.unwrap();
    assert_eq!(e.fake.lock().unwrap().tickets, 2);
    assert_eq!(e.connect()["ticket"], format!("{TICKET_MARK}_2"));
}

#[tokio::test]
async fn ticket_nao_renovado_dentro_de_240_s() {
    let e = env().await;
    e.fake.lock().unwrap().advance_per_file = 120;
    let (r, _) = e.run(&e.ctx(true, None)).await;
    r.unwrap();
    assert_eq!(e.fake.lock().unwrap().tickets, 1);
    assert_eq!(e.connect()["ticket"], format!("{TICKET_MARK}_1"));
}

#[tokio::test]
async fn sem_simular_e_sem_jogo_falha_antes_do_ticket() {
    let e = env().await;
    let (r, stages) = e.run(&e.ctx(false, None)).await;
    assert_eq!(r.unwrap_err().code, "game_not_found");
    assert!(stages.is_empty());
    assert_eq!(e.fake.lock().unwrap().tickets, 0);
}

#[tokio::test]
async fn servidor_offline_e_server_unavailable() {
    let e = env().await;
    e.fake.lock().unwrap().online = false;
    let (r, _) = e.run(&e.ctx(true, None)).await;
    assert_eq!(r.unwrap_err().code, "server_unavailable");
    assert_eq!(e.fake.lock().unwrap().tickets, 0);
}

#[tokio::test]
async fn manifest_de_outro_protocolo_e_recusado() {
    let e = env().await;
    e.fake.lock().unwrap().manifest["protocol"] = 9.into();
    let (r, stages) = e.run(&e.ctx(true, None)).await;
    assert_eq!(r.unwrap_err().code, "manifest_mismatch");
    assert!(!stages.contains(&Stage::Download));
    assert!(!e.root.join("run").join("connect.json").exists());
}

#[tokio::test]
async fn cancelado_antes_de_comecar() {
    let e = env().await;
    let c = CancellationToken::new();
    c.cancel();
    let r = play(&e.ctx(true, None), SERVER, &|_| {}, |_| {}, c).await;
    assert_eq!(r.unwrap_err().code, "cancelled");
    assert_eq!(e.fake.lock().unwrap().tickets, 0);
}

#[tokio::test]
async fn play_e_um_por_vez() {
    let gate = PlayGate::default();
    let run = gate.try_begin().unwrap();
    assert_eq!(gate.try_begin().unwrap_err().code, "busy");
    // `cancel` alcança o play em andamento.
    assert!(gate.cancel());
    assert!(run.cancel.is_cancelled());
    drop(run);
    let again = gate.try_begin().unwrap();
    assert!(!again.cancel.is_cancelled());
    drop(again);
    assert!(!gate.cancel(), "nada em andamento");
}
