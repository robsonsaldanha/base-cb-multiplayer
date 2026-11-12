//! Arquivo de conexão (ACL só do usuário, `launchId`), ponteiro do mod (`cbmp-run.json`), prazo de 240 s,
//! abertura/simulação e a orquestração do `play` contra um Hub + `cb-server` falsos (axum em 127.0.0.1).

use axum::extract::{Path as AxPath, State};
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use cbmp_launcher_lib::auth::{Auth, Clock, MemoryVault, TokenVault};
use cbmp_launcher_lib::content::{resolve_game_ip, ContentStore, Limits};
use cbmp_launcher_lib::game::{GameInstall, Processes, Store};
use cbmp_launcher_lib::hub::HubClient;
use cbmp_launcher_lib::launch::{
    self, acl, connect_json, expire_launch, launch, new_launch_id, pointer_json,
    remove_pointer_if_ours, write_connect_file, write_pointer, ConnectInfo, LaunchOutcome,
    CONNECT_TTL, POINTER_FILE,
};
use cbmp_launcher_lib::modpkg::{container_dir, win64_dir, ModInstaller};
use cbmp_launcher_lib::play::{play, PlayContext, PlayGate, Stage, TICKET_RENEW_AFTER_SECS};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio_util::sync::CancellationToken;

#[path = "common/modzip.rs"]
mod modzip;

const TICKET_MARK: &str = "TICKETSECRETO";
const LAUNCH_ID: &str = "0123456789abcdef0123456789abcdef";

fn info(content_dir: &Path) -> ConnectInfo {
    ConnectInfo {
        host: "127.0.0.1".into(),
        port: 7777,
        ticket: format!("{TICKET_MARK}_1"),
        server_id: "srv_A".into(),
        content_dir: content_dir.to_path_buf(),
        protocol: 3,
        launch_id: LAUNCH_ID.into(),
    }
}

fn is_launch_id(s: &str) -> bool {
    s.len() == 32 && s.bytes().all(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f'))
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
    // O mesmo `launchId` do ponteiro: o mod confere que o arquivo é desta abertura.
    assert_eq!(v["launchId"], LAUNCH_ID);
    let mut keys: Vec<&str> = v.as_object().unwrap().keys().map(String::as_str).collect();
    keys.sort_unstable();
    assert_eq!(
        keys,
        [
            "contentDir",
            "contentReadOnly",
            "host",
            "launchId",
            "port",
            "protocol",
            "serverId",
            "ticket"
        ]
    );
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
    let plain = tmp.path().join("comum.txt");
    std::fs::write(&plain, b"x").unwrap();
    let inherited = acl::read_dacl(&plain).unwrap();
    assert!(!inherited.protected);
    assert!(
        inherited.aces.iter().any(|a| a.sid != me),
        "{:?}",
        inherited.aces
    );
    std::fs::remove_file(&plain).unwrap();
    // A pasta `run\` também é só do usuário, e o que nasce nela herda só ele.
    let run_dacl = acl::read_dacl(&run).unwrap();
    assert!(run_dacl.protected);
    assert!(
        run_dacl.aces.iter().all(|a| a.sid == me),
        "{:?}",
        run_dacl.aces
    );
    let child = run.join("filho.txt");
    std::fs::write(&child, b"x").unwrap();
    assert!(acl::read_dacl(&child)
        .unwrap()
        .aces
        .iter()
        .all(|a| a.sid == me));
    std::fs::remove_file(&child).unwrap();
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

#[test]
fn launch_id_e_aleatorio_com_32_hex_minusculos() {
    let ids: HashSet<String> = (0..64).map(|_| new_launch_id()).collect();
    assert_eq!(ids.len(), 64, "valores repetidos");
    for id in &ids {
        assert!(is_launch_id(id), "{id}");
    }
}

// ----- ponteiro (cbmp-run.json) -----

#[test]
fn ponteiro_tem_so_as_chaves_do_contrato_e_nenhum_segredo() {
    let tmp = tempfile::tempdir().unwrap();
    let run = tmp.path().join("run");
    let bytes = pointer_json(&run, LAUNCH_ID, 1_794_438_789, "0.2.0");
    assert!(bytes.len() <= 4096);
    let v: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
    let mut keys: Vec<&str> = v.as_object().unwrap().keys().map(String::as_str).collect();
    keys.sort_unstable();
    assert_eq!(keys, ["launchId", "launcher", "runDir", "v", "writtenAt"]);
    assert_eq!(v["v"], 1);
    assert_eq!(v["runDir"], run.display().to_string());
    assert_eq!(v["launchId"], LAUNCH_ID);
    assert_eq!(v["writtenAt"], 1_794_438_789u64);
    assert_eq!(v["launcher"], "0.2.0");
    let text = String::from_utf8(bytes).unwrap();
    assert!(!text.contains(TICKET_MARK) && !text.contains("ticket") && !text.contains("host"));
}

#[test]
fn ponteiro_e_gravado_por_rename_e_sobrescreve_o_anterior() {
    let tmp = tempfile::tempdir().unwrap();
    let container = tmp.path().join("cbmp");
    let run = tmp.path().join("run");
    let path = write_pointer(&container, &run, LAUNCH_ID, 10, "0.2.0").unwrap();
    assert_eq!(path, container.join(POINTER_FILE));
    assert_eq!(POINTER_FILE, "cbmp-run.json");
    assert_eq!(
        std::fs::read(&path).unwrap(),
        pointer_json(&run, LAUNCH_ID, 10, "0.2.0")
    );
    let other = new_launch_id();
    write_pointer(&container, &run, &other, 20, "0.2.0").unwrap();
    assert_eq!(
        std::fs::read(&path).unwrap(),
        pointer_json(&run, &other, 20, "0.2.0")
    );
    let names: Vec<_> = std::fs::read_dir(&container)
        .unwrap()
        .map(|e| e.unwrap().file_name())
        .collect();
    assert_eq!(names, vec![std::ffi::OsString::from(POINTER_FILE)]);
}

#[test]
fn remove_pointer_so_apaga_o_nosso() {
    let tmp = tempfile::tempdir().unwrap();
    let container = tmp.path().join("cbmp");
    let run = tmp.path().join("run");
    let path = write_pointer(&container, &run, LAUNCH_ID, 10, "0.2.0").unwrap();
    remove_pointer_if_ours(&container, &new_launch_id());
    assert!(path.exists(), "ponteiro de outro launchId fica");
    remove_pointer_if_ours(&container, LAUNCH_ID);
    assert!(!path.exists());
    // Ausente, ou que não é JSON: nada acontece.
    remove_pointer_if_ours(&container, LAUNCH_ID);
    std::fs::write(&path, b"lixo").unwrap();
    remove_pointer_if_ours(&container, LAUNCH_ID);
    assert!(path.exists());
}

// ----- prazo de 240 s -----

struct Launched {
    _tmp: tempfile::TempDir,
    run: PathBuf,
    container: PathBuf,
}

impl Launched {
    fn new() -> Self {
        let tmp = tempfile::tempdir().unwrap();
        Launched {
            run: tmp.path().join("run"),
            container: tmp.path().join("cbmp"),
            _tmp: tmp,
        }
    }

    /// Grava conexão e ponteiro como o `play` e agenda o prazo.
    fn write(&self, i: &ConnectInfo) -> (PathBuf, PathBuf, tokio::task::JoinHandle<()>) {
        let connect = write_connect_file(&self.run, i).unwrap();
        let pointer = write_pointer(&self.container, &self.run, &i.launch_id, 1, "0.2.0").unwrap();
        let task = tokio::spawn(expire_launch(
            connect.clone(),
            connect_json(i),
            Some(self.container.clone()),
            i.launch_id.clone(),
            CONNECT_TTL,
        ));
        (connect, pointer, task)
    }
}

#[tokio::test(start_paused = true)]
async fn conexao_e_ponteiro_saem_depois_de_240_s() {
    assert_eq!(CONNECT_TTL, Duration::from_secs(240));
    let l = Launched::new();
    let i = info(&l.run);
    let (connect, pointer, task) = l.write(&i);
    tokio::time::sleep(Duration::from_secs(239)).await;
    assert!(
        connect.exists() && pointer.exists(),
        "ainda dentro dos 240 s"
    );
    tokio::time::sleep(Duration::from_secs(1)).await;
    task.await.unwrap();
    assert!(!connect.exists(), "connect.json apagado aos 240 s");
    assert!(!pointer.exists(), "ponteiro apagado aos 240 s");
}

#[tokio::test(start_paused = true)]
async fn ponteiro_sai_mesmo_com_connect_json_ja_lido_pelo_mod() {
    let l = Launched::new();
    let i = info(&l.run);
    let (connect, pointer, task) = l.write(&i);
    tokio::time::sleep(Duration::from_secs(10)).await;
    std::fs::remove_file(&connect).unwrap(); // o mod leu e apagou
    tokio::time::sleep(Duration::from_secs(231)).await;
    task.await.unwrap();
    assert!(!pointer.exists());
}

#[tokio::test(start_paused = true)]
async fn prazo_nao_apaga_conexao_nem_ponteiro_de_um_play_mais_novo() {
    let l = Launched::new();
    let old = info(&l.run);
    let (connect, pointer, task) = l.write(&old);
    tokio::time::sleep(Duration::from_secs(100)).await;
    let mut new = info(&l.run);
    new.ticket = format!("{TICKET_MARK}_novo");
    new.launch_id = new_launch_id();
    write_connect_file(&l.run, &new).unwrap();
    write_pointer(&l.container, &l.run, &new.launch_id, 2, "0.2.0").unwrap();
    tokio::time::sleep(Duration::from_secs(141)).await;
    task.await.unwrap();
    assert_eq!(std::fs::read(&connect).unwrap(), connect_json(&new));
    assert_eq!(
        std::fs::read(&pointer).unwrap(),
        pointer_json(&l.run, &new.launch_id, 2, "0.2.0")
    );
}

#[tokio::test(start_paused = true)]
async fn prazo_sem_ponteiro_simulado_so_apaga_a_conexao() {
    let tmp = tempfile::tempdir().unwrap();
    let i = info(tmp.path());
    let connect = write_connect_file(tmp.path(), &i).unwrap();
    let task = tokio::spawn(expire_launch(
        connect.clone(),
        connect_json(&i),
        None,
        i.launch_id.clone(),
        CONNECT_TTL,
    ));
    tokio::time::sleep(Duration::from_secs(241)).await;
    task.await.unwrap();
    assert!(!connect.exists());
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
    /// Mod publicado: (zip, assinatura, versão anunciada) e as consultas recebidas.
    mod_pkg: Option<(Vec<u8>, String, String)>,
    mod_queries: Vec<String>,
    /// Atraso (ms) antes de responder o ticket.
    ticket_delay_ms: u64,
    /// `host` anunciado pelo Hub (padrão `127.0.0.1`).
    host: String,
    /// Pedidos de detalhe do servidor recebidos (o primeiro pedido de todo play).
    detail_requests: u32,
    /// Segundos somados ao relógio do launcher quando o manifest é servido.
    advance_on_manifest: i64,
}

type Shared = Arc<Mutex<Fake>>;

/// Lista de processos falsa (jogo aberto ou não).
struct FakeProcs(Vec<&'static str>);

impl Processes for FakeProcs {
    fn names(&self) -> Option<Vec<String>> {
        Some(self.0.iter().map(|s| s.to_string()).collect())
    }
}

struct Env {
    /// Chave pública do par gerado neste teste (formato do Tauri).
    pubkey: String,
    keys: modzip::Keys,
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
    let delay = s.lock().unwrap().ticket_delay_ms;
    tokio::time::sleep(Duration::from_millis(delay)).await;
    let mut f = s.lock().unwrap();
    f.tickets += 1;
    Json(serde_json::json!({ "ticket": format!("{TICKET_MARK}_{}", f.tickets) })).into_response()
}

async fn server(State(s): State<Shared>, AxPath(id): AxPath<String>) -> Response {
    let mut f = s.lock().unwrap();
    f.detail_requests += 1;
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
        v["host"] = f.host.clone().into();
        v["gamePort"] = 7777.into();
        v["httpPort"] = f.port.into();
        v["version"] = "1.0.0".into();
        v["protocol"] = PROTOCOL.into();
    }
    Json(v).into_response()
}

async fn manifest(State(s): State<Shared>) -> Response {
    let f = s.lock().unwrap();
    f.clock.fetch_add(f.advance_on_manifest, Ordering::SeqCst);
    Json(f.manifest.clone()).into_response()
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

async fn mod_latest(State(s): State<Shared>, uri: axum::http::Uri) -> Response {
    let mut f = s.lock().unwrap();
    f.mod_queries.push(uri.query().unwrap_or("").to_string());
    let Some((zip, sig, version)) = f.mod_pkg.clone() else {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({ "error": "no_mod", "message": "x" })),
        )
            .into_response();
    };
    Json(serde_json::json!({
        "version": version, "protocol": PROTOCOL,
        "url": format!("http://127.0.0.1:{}/v1/dl/mod-p{PROTOCOL}-v{version}/m.zip", f.port),
        "sha256": sha_hex(&zip), "size": zip.len(), "signature": sig,
    }))
    .into_response()
}

async fn mod_dl(State(s): State<Shared>) -> Response {
    match s.lock().unwrap().mod_pkg.clone() {
        Some((zip, _, _)) => zip.into_response(),
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
        host: "127.0.0.1".into(),
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
        .route("/v1/mod/latest", get(mod_latest))
        .route("/v1/dl/{tag}/{asset}", get(mod_dl))
        .with_state(fake.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = listener.local_addr().unwrap().port();
    fake.lock().unwrap().port = port;
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    let vault = Arc::new(MemoryVault::default());
    vault.set("rt_1").unwrap();
    let hub = HubClient::new(&format!("http://127.0.0.1:{port}")).unwrap();
    let auth = Arc::new(Auth::with_clock(hub, vault, shared_clock(&clock)));
    let keys = modzip::keys();
    Env {
        pubkey: modzip::pubkey(&keys),
        keys,
        _tmp: tmp,
        root,
        fake,
        clock,
        auth,
    }
}

impl Env {
    fn ctx(&self, simulate: bool, game: Option<GameInstall>) -> PlayContext {
        self.ctx_with(simulate, game, vec!["explorer.exe"])
    }

    fn ctx_with(
        &self,
        simulate: bool,
        game: Option<GameInstall>,
        processes: Vec<&'static str>,
    ) -> PlayContext {
        PlayContext {
            auth: self.auth.clone(),
            content: ContentStore::new(self.root.join("cache"), self.root.join("servers"))
                .with_retry_delay(Duration::from_millis(5)),
            // Sem fixar versão: o launcher já é 0.2.0 (o `minLauncher` do pacote).
            mods: ModInstaller::new(self.root.join("mods"), self.pubkey.clone()),
            run_dir: self.root.join("run"),
            clock: shared_clock(&self.clock),
            simulate,
            game,
            limits: Limits::new(2048),
            cache_limit_bytes: 10_240 * 1024 * 1024,
            keep: HashSet::new(),
            connect_ttl: CONNECT_TTL,
            processes: Arc::new(FakeProcs(processes)),
            launcher_version: "0.2.0".into(),
            confirm_conflict: false,
        }
    }

    /// Pasta de jogo falsa (`HogwartsLegacy.exe` é cópia do `whoami.exe` no Windows: nunca o jogo real).
    fn fake_game(&self) -> GameInstall {
        let dir = self.root.join("HL");
        std::fs::create_dir_all(dir.join("Phoenix").join("Binaries").join("Win64")).unwrap();
        let exe = dir.join("HogwartsLegacy.exe");
        #[cfg(windows)]
        {
            let sys = std::env::var_os("SystemRoot").unwrap();
            std::fs::copy(Path::new(&sys).join("System32").join("whoami.exe"), &exe).unwrap();
        }
        #[cfg(not(windows))]
        std::fs::write(&exe, b"x").unwrap();
        GameInstall {
            dir,
            exe,
            store: Store::Manual,
        }
    }

    /// Publica no Hub falso o mod 1.0.0 (layout 2, carregador `b"mod"`) do protocolo do servidor, assinado como no
    /// release.
    fn publish_mod(&self) {
        let zip = modzip::build("1.0.0", PROTOCOL, b"mod");
        let sig = modzip::sign(&self.keys, &zip, PROTOCOL, "1.0.0");
        self.fake.lock().unwrap().mod_pkg = Some((zip, sig, "1.0.0".into()));
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
async fn ticket_emitido_ha_31_s_e_renovado() {
    assert_eq!(TICKET_RENEW_AFTER_SECS, 30);
    let e = env().await;
    e.fake.lock().unwrap().advance_on_manifest = 31;
    let (r, _) = e.run(&e.ctx(true, None)).await;
    r.unwrap();
    assert_eq!(e.fake.lock().unwrap().tickets, 2);
    assert_eq!(e.connect()["ticket"], format!("{TICKET_MARK}_2"));
}

#[tokio::test]
async fn ticket_emitido_ha_29_s_nao_e_renovado() {
    let e = env().await;
    e.fake.lock().unwrap().advance_on_manifest = 29;
    let (r, _) = e.run(&e.ctx(true, None)).await;
    r.unwrap();
    assert_eq!(e.fake.lock().unwrap().tickets, 1);
    assert_eq!(e.connect()["ticket"], format!("{TICKET_MARK}_1"));
}

#[tokio::test]
async fn host_por_nome_vira_ip_literal_no_connect_json() {
    let e = env().await;
    e.fake.lock().unwrap().host = "localhost".into();
    let (r, _) = e.run(&e.ctx(true, None)).await;
    let out = r.unwrap();
    let c = e.connect();
    assert_eq!(c["host"], "127.0.0.1");
    assert!(is_launch_id(c["launchId"].as_str().unwrap()));
    // A prévia mostra o mesmo endereço gravado para o mod.
    let LaunchOutcome::Simulated(p) = out else {
        panic!("{out:?}")
    };
    assert_eq!(p.host, "127.0.0.1");
}

#[tokio::test]
async fn resolver_endereco_do_servidor_de_jogo() {
    use std::net::IpAddr;
    let ip = |s: &str| s.parse::<IpAddr>().unwrap();
    assert_eq!(
        resolve_game_ip("localhost", true).await.unwrap(),
        ip("127.0.0.1")
    );
    assert_eq!(
        resolve_game_ip("8.8.8.8", false).await.unwrap(),
        ip("8.8.8.8")
    );
    assert_eq!(
        resolve_game_ip("[2001:4860:4860::8888]", false)
            .await
            .unwrap(),
        ip("2001:4860:4860::8888")
    );
    // Formas numéricas do WHATWG e IPv4 mapeado viram IPv4 comum.
    assert_eq!(
        resolve_game_ip("127.1", true).await.unwrap(),
        ip("127.0.0.1")
    );
    assert_eq!(
        resolve_game_ip("[::ffff:8.8.4.4]", false).await.unwrap(),
        ip("8.8.4.4")
    );
    // Nome (ou IP) que leva a endereço proibido pela regra do build oficial.
    for bad in [
        "localhost",
        "127.0.0.1",
        "10.0.0.1",
        "[::1]",
        "0.0.0.0",
        "169.254.1.1",
    ] {
        assert_eq!(
            resolve_game_ip(bad, false).await.unwrap_err().code,
            "server_unavailable",
            "{bad}"
        );
    }
    for bad in ["", "a b", "x/y", "nao-existe.invalid", "[zz]"] {
        assert_eq!(
            resolve_game_ip(bad, true).await.unwrap_err().code,
            "server_unavailable",
            "{bad}"
        );
    }
}

#[tokio::test]
async fn jogo_aberto_e_recusado_antes_de_qualquer_pedido_ao_hub() {
    let e = env().await;
    let game = e.fake_game();
    let ctx = e.ctx_with(false, Some(game), vec!["steam.exe", "HogwartsLegacy.exe"]);
    let (r, stages) = e.run(&ctx).await;
    let err = r.unwrap_err();
    assert_eq!(err.code, "game_running");
    assert_eq!(
        err.message,
        "O Hogwarts Legacy está aberto. Feche o jogo e tente de novo."
    );
    assert!(stages.is_empty());
    let f = e.fake.lock().unwrap();
    assert_eq!((f.detail_requests, f.tickets), (0, 0));
    assert!(f.mod_queries.is_empty());
}

#[tokio::test]
async fn simular_nao_confere_jogo_aberto_nem_grava_ponteiro() {
    let e = env().await;
    let ctx = e.ctx_with(true, None, vec!["HogwartsLegacy.exe"]);
    let (r, _) = e.run(&ctx).await;
    assert!(matches!(r.unwrap(), LaunchOutcome::Simulated(_)));
    assert!(e.fake.lock().unwrap().mod_queries.is_empty(), "nada de mod");
    assert!(is_launch_id(e.connect()["launchId"].as_str().unwrap()));
}

#[tokio::test]
async fn conflito_plantado_para_o_jogar_sem_gravar_a_conexao() {
    let e = env().await;
    let game = e.fake_game();
    let win64 = win64_dir(&game);
    std::fs::write(win64.join("dwmapi.dll"), b"UE4SS do jogador").unwrap();
    e.publish_mod();
    let (r, stages) = e.run(&e.ctx(false, Some(game.clone()))).await;
    assert_eq!(r.unwrap_err().code, "mod_conflict");
    assert_eq!(stages.last(), Some(&Stage::Mod));
    assert_eq!(
        std::fs::read(win64.join("dwmapi.dll")).unwrap(),
        b"UE4SS do jogador"
    );
    assert!(!e.root.join("run").join("connect.json").exists());
    assert!(!container_dir(&game).join(POINTER_FILE).exists());
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

/// Fluxo completo sem simular: mod pedido com o protocolo do servidor, instalado e ativado no jogo, conexão e ponteiro
/// gravados com o mesmo `launchId`, e o executável aberto (cópia do `whoami.exe`, nunca o jogo real).
#[cfg(windows)]
#[tokio::test]
async fn play_sem_simular_instala_ativa_grava_o_ponteiro_e_abre_o_jogo() {
    let e = env().await;
    let game = e.fake_game();
    let win64 = win64_dir(&game);
    e.publish_mod();

    let (r, stages) = e.run(&e.ctx(false, Some(game.clone()))).await;
    let out = r.unwrap();
    assert_eq!(out, LaunchOutcome::Started);
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
    assert_eq!(
        e.fake.lock().unwrap().mod_queries,
        vec![format!("protocol={PROTOCOL}")]
    );
    assert_eq!(
        std::fs::read(
            win64
                .join("cbmp")
                .join("atual")
                .join("loader")
                .join("dwmapi.dll")
        )
        .unwrap(),
        b"mod"
    );
    // Ativo: carregador em `Win64\` e `override.txt` apontando para `cbmp\atual\ue4ss`.
    assert_eq!(std::fs::read(win64.join("dwmapi.dll")).unwrap(), b"mod");
    assert_eq!(
        std::fs::read_to_string(win64.join("override.txt"))
            .unwrap()
            .trim(),
        r"cbmp\atual\ue4ss"
    );
    let c = e.connect();
    assert_eq!(c["ticket"], format!("{TICKET_MARK}_1"));
    assert_eq!(c["host"], "127.0.0.1");
    let launch_id = c["launchId"].as_str().unwrap().to_string();
    assert!(is_launch_id(&launch_id));
    let pointer_path = win64.join("cbmp").join(POINTER_FILE);
    let p: serde_json::Value =
        serde_json::from_slice(&std::fs::read(&pointer_path).unwrap()).unwrap();
    assert_eq!(p["v"], 1);
    assert_eq!(p["launchId"], launch_id.as_str());
    assert_eq!(p["runDir"], e.root.join("run").display().to_string());
    assert_eq!(p["launcher"], "0.2.0");
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs();
    assert!(p["writtenAt"].as_u64().unwrap().abs_diff(now) < 60);
    // Segundo Jogar: outro `launchId`, ponteiro regravado.
    let (r, _) = e.run(&e.ctx(false, Some(game))).await;
    r.unwrap();
    let id2 = e.connect()["launchId"].as_str().unwrap().to_string();
    assert_ne!(id2, launch_id);
    let p2: serde_json::Value =
        serde_json::from_slice(&std::fs::read(&pointer_path).unwrap()).unwrap();
    assert_eq!(p2["launchId"], id2.as_str());
}

#[tokio::test]
async fn cancelar_interrompe_pedido_em_andamento() {
    let e = env().await;
    // Ticket que nunca chega: o cancelamento tem de encerrar sem esperar o prazo de rede.
    let c = CancellationToken::new();
    let c2 = c.clone();
    tokio::spawn(async move {
        tokio::time::sleep(Duration::from_millis(50)).await;
        c2.cancel();
    });
    e.fake.lock().unwrap().ticket_delay_ms = 60_000;
    let ctx = e.ctx(true, None);
    let r = tokio::time::timeout(
        Duration::from_secs(5),
        play(&ctx, SERVER, &|_| {}, |_| {}, c),
    )
    .await
    .expect("cancelamento não pode esperar o prazo de rede");
    assert_eq!(r.unwrap_err().code, "cancelled");
}
