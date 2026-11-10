//! Ponta a ponta com o Hub e o `cb-server` de verdade: `tools/dev-env.mjs --run` sobe o Hub local (node, dev-login,
//! servidores privados, dados numa pasta temporária) e o `cb-server` em modo Hub com o `hello-client`, cadastrado como
//! "Castelo Bruxo (teste)" em destaque. Daqui o launcher faz o login de dev pelo mesmo caminho do botão Entrar
//! (PKCE + state → formulário do Hub → `cbmp://auth`), acha o servidor na lista, roda o `play` em modo simular e
//! confere que o conteúdo montado bate com o manifest servido pelo `cb-server` e com os arquivos do `hello-client`.
//!
//! Pré-requisitos: `node` no PATH, `npm ci` na raiz (o Hub roda com o `tsx` de `node_modules`) e o binário do
//! `cb-server` (`CBMP_CB_SERVER`, ou `build-native\windows-release\server\Release\cb-server.exe`).
//! Sem o binário o teste é pulado com aviso (o Rust não tem `#[ignore]` condicional); com `CBMP_E2E=1` (CI) a falta
//! vira falha, para o teste não sumir em silêncio.
//!
//! Portas livres escolhidas na hora: não colide com um `dev-env --run` aberto nas portas 8787/30500.

use cbmp_launcher_lib::auth::{monotonic_clock, Auth, MemoryVault};
use cbmp_launcher_lib::content::{self, ContentStore, Limits};
use cbmp_launcher_lib::hub::{self, HubClient};
use cbmp_launcher_lib::launch::{LaunchOutcome, CONNECT_TTL};
use cbmp_launcher_lib::modpkg::ModInstaller;
use cbmp_launcher_lib::play::{play, PlayContext, Stage};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};
use tokio_util::sync::CancellationToken;

const SERVER_NAME: &str = "Castelo Bruxo (teste)";
const READY: &str = "[dev-env] rodando.";

fn repo() -> PathBuf {
    // Sem `canonicalize`: no Windows ele devolve um caminho `\\?\C:\...`, que o node não aceita como script.
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(2)
        .expect("raiz do repositório")
        .to_path_buf()
}

fn cb_server() -> Option<PathBuf> {
    let p = std::env::var_os("CBMP_CB_SERVER")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            repo()
                .join("build-native")
                .join("windows-release")
                .join("server")
                .join("Release")
                .join(if cfg!(windows) {
                    "cb-server.exe"
                } else {
                    "cb-server"
                })
        });
    p.is_file().then_some(p)
}

fn free_port() -> u16 {
    std::net::TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
}

/// Porta livre em TCP e UDP (o `cb-server` usa as duas no mesmo número).
fn free_game_port() -> u16 {
    loop {
        let p = free_port();
        if std::net::UdpSocket::bind(("127.0.0.1", p)).is_ok() {
            return p;
        }
    }
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// `dev-env --run` rodando; ao sair do escopo encerra a árvore (Hub e cb-server são filhos dele).
struct DevEnv {
    child: Child,
    dir: tempfile::TempDir,
    hub_url: String,
    output: Arc<Mutex<Vec<String>>>,
}

impl DevEnv {
    fn start(cb_server: &Path) -> Self {
        let dir = tempfile::tempdir().unwrap();
        let (hub_port, game_port) = (free_port(), free_game_port());
        let mut child = Command::new("node")
            .arg(repo().join("tools").join("dev-env.mjs"))
            .args(["--run", "--hub-port", &hub_port.to_string()])
            .args(["--game-port", &game_port.to_string()])
            .arg("--dir")
            .arg(dir.path())
            .arg("--cb-server")
            .arg(cb_server)
            // Nada de app Discord herdado do ambiente: o teste usa só o dev-login.
            .env_remove("DISCORD_CLIENT_ID")
            .env_remove("DISCORD_CLIENT_SECRET")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("node no PATH (o Hub local roda em Node)");
        let output = Arc::new(Mutex::new(Vec::new()));
        let (tx, rx) = mpsc::channel::<()>();
        let pipe = |r: Box<dyn std::io::Read + Send>, tx: Option<mpsc::Sender<()>>| {
            let out = output.clone();
            std::thread::spawn(move || {
                for line in BufReader::new(r).lines().map_while(Result::ok) {
                    if line.starts_with(READY) {
                        if let Some(tx) = &tx {
                            let _ = tx.send(());
                        }
                    }
                    out.lock().unwrap().push(line);
                }
            });
        };
        pipe(Box::new(child.stdout.take().unwrap()), Some(tx));
        pipe(Box::new(child.stderr.take().unwrap()), None);
        let env = DevEnv {
            child,
            dir,
            hub_url: format!("http://127.0.0.1:{hub_port}"),
            output,
        };
        // Primeira execução do Hub pelo tsx compila o TypeScript: prazo folgado.
        let deadline = Instant::now() + Duration::from_secs(180);
        let mut env = env;
        loop {
            if rx.recv_timeout(Duration::from_millis(250)).is_ok() {
                return env;
            }
            if let Ok(Some(st)) = env.child.try_wait() {
                env.fail(&format!("dev-env terminou antes de ficar pronto ({st})"));
            }
            if Instant::now() > deadline {
                env.fail("dev-env não ficou pronto em 180 s");
            }
        }
    }

    /// Falha com a saída do dev-env e os logs do Hub/cb-server.
    fn fail(&self, msg: &str) -> ! {
        let mut report = format!("{msg}\n--- saída do dev-env ---\n");
        for l in self.output.lock().unwrap().iter() {
            report.push_str(l);
            report.push('\n');
        }
        for name in ["hub.log", "cb-server.log"] {
            let p = self.dir.path().join("logs").join(name);
            if let Ok(t) = std::fs::read_to_string(&p) {
                let tail: Vec<&str> = t.lines().rev().take(60).collect();
                report.push_str(&format!("--- {name} (fim) ---\n"));
                for l in tail.into_iter().rev() {
                    report.push_str(l);
                    report.push('\n');
                }
            }
        }
        panic!("{report}");
    }
}

impl Drop for DevEnv {
    fn drop(&mut self) {
        // A árvore toda: o dev-env não recebe Ctrl+C aqui, e matar só o node deixaria Hub e cb-server órfãos.
        #[cfg(windows)]
        let _ = Command::new("taskkill")
            .args(["/T", "/F", "/PID", &self.child.id().to_string()])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn hub_e_cb_server_reais_login_dev_e_play_simulado() {
    let Some(exe) = cb_server() else {
        if std::env::var_os("CBMP_E2E").is_some_and(|v| v == "1") {
            panic!("CBMP_E2E=1 e o cb-server não foi encontrado (defina CBMP_CB_SERVER)");
        }
        eprintln!(
            "e2e PULADO: cb-server não encontrado (compile o preset windows-release ou defina CBMP_CB_SERVER)"
        );
        return;
    };
    let env = DevEnv::start(&exe);
    let r = run(&env).await;
    if let Err(msg) = r {
        env.fail(&msg);
    }
}

/// Corpo do teste; erro vira falha com os logs do ambiente.
async fn run(env: &DevEnv) -> Result<(), String> {
    let data = tempfile::tempdir().unwrap();
    let root = data.path();
    let hub = HubClient::new(&env.hub_url).map_err(|e| format!("HubClient: {}", e.code))?;
    let auth = Arc::new(Auth::new(hub, Arc::new(MemoryVault::default())));

    // ----- login: mesmo caminho do botão Entrar, com o formulário de dev do Hub no lugar do Discord -----
    let login = auth.begin_login();
    let http = hub::http_client();
    let page = http
        .get(login.clone())
        .send()
        .await
        .map_err(|e| format!("página de login: {e}"))?;
    if page.status() != 200 {
        return Err(format!("página de login: HTTP {}", page.status()));
    }
    let html = page.text().await.unwrap_or_default();
    let state = login
        .query_pairs()
        .find(|(k, _)| k == "state")
        .map(|(_, v)| v.into_owned())
        .unwrap();
    if !html.contains("/v1/auth/dev-login") || !html.contains(&state) {
        return Err("o Hub não serviu o formulário de dev com o state do launcher".into());
    }
    // "Entrar como dev": o formulário repete os parâmetros do launcher e acrescenta a conta fictícia.
    let mut submit = url::Url::parse(&format!("{}/v1/auth/dev-login", env.hub_url)).unwrap();
    submit
        .query_pairs_mut()
        .extend_pairs(login.query_pairs())
        .append_pair("discordId", "111111111111111111")
        .append_pair("name", "Dev Local");
    let r = http
        .get(submit)
        .send()
        .await
        .map_err(|e| format!("dev-login: {e}"))?;
    let link = r
        .headers()
        .get("location")
        .and_then(|v| v.to_str().ok())
        .ok_or_else(|| format!("dev-login sem redirecionamento (HTTP {})", r.status()))?
        .to_string();
    if !link.starts_with("cbmp://auth?") {
        return Err("dev-login não redirecionou para cbmp://auth".into());
    }
    auth.on_deep_link(&link)
        .await
        .map_err(|e| format!("deep link recusado: {}", e.code))?;
    let me = auth.me().await.map_err(|e| format!("me: {}", e.code))?;
    if me.name != "Dev Local" {
        return Err(format!("usuário inesperado: {}", me.name));
    }

    // ----- lista: o servidor de teste em destaque -----
    let servers = auth
        .hub()
        .servers()
        .await
        .map_err(|e| format!("lista: {}", e.code))?;
    let srv = servers
        .iter()
        .find(|s| s.name == SERVER_NAME)
        .ok_or("servidor de teste fora da lista")?;
    if !srv.featured {
        return Err("servidor de teste sem destaque".into());
    }
    let server_id = srv.id.clone();

    // ----- play simulado -----
    let ctx = PlayContext {
        auth: auth.clone(),
        content: ContentStore::new(root.join("cache"), root.join("servers")),
        mods: ModInstaller::new(
            root.join("mods"),
            cbmp_launcher_lib::update::pubkey().to_string(),
        ),
        run_dir: root.join("run"),
        clock: monotonic_clock(),
        simulate: true,
        game: None,
        limits: Limits::new(2048),
        cache_limit_bytes: 10_240 * 1024 * 1024,
        keep: HashSet::new(),
        connect_ttl: CONNECT_TTL,
    };
    let stages = Arc::new(Mutex::new(Vec::new()));
    let st = stages.clone();
    let out = play(
        &ctx,
        &server_id,
        &move |s| st.lock().unwrap().push(s),
        |_| {},
        CancellationToken::new(),
    )
    .await
    .map_err(|e| format!("play: {} ({})", e.code, e.message))?;
    let stages = stages.lock().unwrap().clone();
    let want = vec![
        Stage::Ticket,
        Stage::Manifest,
        Stage::Download,
        Stage::Mount,
        Stage::Mod,
        Stage::Connect,
        Stage::Launch,
    ];
    if stages != want {
        return Err(format!("etapas: {stages:?}"));
    }
    let LaunchOutcome::Simulated(preview) = out else {
        return Err("play não terminou em Simulated".into());
    };
    if preview.server_id != server_id || preview.host != "127.0.0.1" {
        return Err(format!(
            "prévia da conexão inesperada: {} {}",
            preview.server_id, preview.host
        ));
    }
    let content_dir = preview.content_dir.clone();
    if content_dir != root.join("servers").join(&server_id) {
        return Err(format!(
            "pasta montada inesperada: {}",
            content_dir.display()
        ));
    }

    // ----- conteúdo montado = manifest do cb-server = arquivos do hello-client -----
    let detail = auth
        .hub()
        .server(&server_id)
        .await
        .map_err(|e| format!("detalhe: {}", e.code))?;
    let base = content::server_base_url(&detail.host.unwrap(), detail.http_port.unwrap())
        .map_err(|e| format!("base: {}", e.code))?;
    let manifest = content::fetch_manifest(&base)
        .await
        .map_err(|e| format!("manifest: {}", e.code))?;
    if manifest.server_id != server_id || manifest.resources.is_empty() {
        return Err("manifest sem resources ou de outro servidor".into());
    }
    let source = repo().join("resources.example");
    let mut expected = HashSet::new();
    for r in &manifest.resources {
        for f in &r.files {
            let rel = format!("{}/{}", r.name, f.path);
            let got = std::fs::read(content_dir.join(&r.name).join(&f.path))
                .map_err(|e| format!("{rel} não montado: {e}"))?;
            if got.len() as u64 != f.size || hex(&Sha256::digest(&got)) != f.sha256 {
                return Err(format!("{rel} difere do manifest"));
            }
            let orig = std::fs::read(source.join(&r.name).join(&f.path))
                .map_err(|e| format!("{rel} sem original em resources.example: {e}"))?;
            if orig != got {
                return Err(format!("{rel} difere do original do hello-client"));
            }
            expected.insert(rel);
        }
    }
    if !manifest.resources.iter().any(|r| r.name == "hello-client") {
        return Err("hello-client ausente do manifest".into());
    }
    // Nada além do manifest na pasta montada (fora o registro interno do launcher, se houver).
    let mut extra = Vec::new();
    walk(&content_dir, &content_dir, &mut |rel| {
        if !expected.contains(rel) {
            extra.push(rel.to_string());
        }
    });
    if !extra.is_empty() {
        return Err(format!(
            "arquivos fora do manifest na pasta montada: {extra:?}"
        ));
    }

    // ----- connect.json: gravado para o mod, com ticket e conteúdo somente leitura -----
    let connect: serde_json::Value = serde_json::from_slice(
        &std::fs::read(root.join("run").join("connect.json"))
            .map_err(|e| format!("connect.json: {e}"))?,
    )
    .map_err(|e| format!("connect.json inválido: {e}"))?;
    if connect["serverId"] != server_id.as_str()
        || connect["contentReadOnly"] != true
        || connect["ticket"].as_str().is_none_or(|t| t.is_empty())
        || connect["port"] != detail.game_port.unwrap()
    {
        return Err("connect.json incompleto".into());
    }
    Ok(())
}

fn walk(root: &Path, dir: &Path, f: &mut dyn FnMut(&str)) {
    for e in std::fs::read_dir(dir).unwrap().map_while(Result::ok) {
        let p = e.path();
        if p.is_dir() {
            walk(root, &p, f);
        } else {
            let rel = p
                .strip_prefix(root)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/");
            f(&rel);
        }
    }
}
