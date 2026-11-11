//! Login (PKCE + deep link), cofre e cliente do Hub contra um Hub falso (axum em 127.0.0.1).

use axum::extract::{Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine as _;
use cbmp_launcher_lib::auth::{
    self, challenge_for, Auth, Clock, MemoryVault, TokenVault, PENDING_TTL_SECS,
};
use cbmp_launcher_lib::hub::HubClient;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use url::Url;

const T0: i64 = 1_800_000_000;
const ACCESS_TTL: i64 = 900;
/// Todo segredo do Hub falso tem este marcador: nenhuma linha de log pode contê-lo.
const SECRET: &str = "SEGREDO";

// ----- captura de log (logger global do processo de teste) -----

struct Capture(Mutex<Vec<String>>);

impl log::Log for Capture {
    fn enabled(&self, _: &log::Metadata) -> bool {
        true
    }
    fn log(&self, r: &log::Record) {
        self.0
            .lock()
            .unwrap()
            .push(format!("{} {}", r.target(), r.args()));
    }
    fn flush(&self) {}
}

fn logs() -> &'static Capture {
    static CAP: OnceLock<&'static Capture> = OnceLock::new();
    CAP.get_or_init(|| {
        let c: &'static Capture = Box::leak(Box::new(Capture(Mutex::new(Vec::new()))));
        log::set_logger(c).unwrap();
        log::set_max_level(log::LevelFilter::Trace);
        c
    })
}

// ----- Hub falso -----

#[derive(Default)]
struct Fake {
    /// código de login → code_challenge (como o Hub real guarda)
    codes: HashMap<String, String>,
    /// refresh token válido → número da geração
    refresh: HashMap<String, u32>,
    /// access token válido → expiração
    access: HashMap<String, i64>,
    generation: u32,
    exchanges: u32,
    refreshes: u32,
    logouts: Vec<(Option<String>, Option<String>)>,
    banned: bool,
    /// Relógio do Hub falso (`accessExpiresAt` e cabeçalho `Date`); independente do relógio do launcher.
    now: Arc<AtomicI64>,
    /// `/v1/auth/refresh` responde 503 (Hub com problema, refresh não recusado).
    refresh_down: bool,
    /// Atraso (ms) só da próxima chamada a `/v1/me`, que confere o token **depois** de esperar.
    me_delay_ms: u64,
    me_calls: u32,
}

type Shared = Arc<Mutex<Fake>>;

fn err(status: StatusCode, code: &str) -> Response {
    (status, Json(json!({ "error": code, "message": "x" }))).into_response()
}

/// Emite a geração seguinte de tokens, com `Date` do relógio do Hub falso (como o Hub real, que manda `Date`).
fn issue(f: &mut Fake) -> Response {
    f.generation += 1;
    let g = f.generation;
    let at = format!("at_{SECRET}_{g}");
    let rt = format!("rt_{SECRET}_{g}");
    let now = f.now.load(Ordering::SeqCst);
    let exp = now + ACCESS_TTL;
    f.access.insert(at.clone(), exp);
    f.refresh.insert(rt.clone(), g);
    let date =
        httpdate::fmt_http_date(std::time::UNIX_EPOCH + std::time::Duration::from_secs(now as u64));
    (
        [("date", date)],
        Json(
            json!({ "accessToken": at, "accessExpiresAt": exp, "refreshToken": rt, "refreshExpiresAt": now + 86400 }),
        ),
    )
        .into_response()
}

fn bearer(h: &HeaderMap) -> Option<String> {
    h.get("authorization")?
        .to_str()
        .ok()?
        .strip_prefix("Bearer ")
        .map(str::to_string)
}

async fn exchange(State(s): State<Shared>, Json(b): Json<Value>) -> Response {
    let mut f = s.lock().unwrap();
    f.exchanges += 1;
    let code = b["code"].as_str().unwrap_or_default().to_string();
    let verifier = b["codeVerifier"].as_str().unwrap_or_default();
    let Some(ch) = f.codes.remove(&code) else {
        return err(StatusCode::BAD_REQUEST, "bad_code");
    };
    if URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes())) != ch {
        return err(StatusCode::BAD_REQUEST, "bad_code");
    }
    issue(&mut f)
}

async fn refresh(State(s): State<Shared>, Json(b): Json<Value>) -> Response {
    let mut f = s.lock().unwrap();
    f.refreshes += 1;
    if f.refresh_down {
        return err(StatusCode::SERVICE_UNAVAILABLE, "busy");
    }
    let rt = b["refreshToken"].as_str().unwrap_or_default();
    let Some(g) = f.refresh.remove(rt) else {
        return err(StatusCode::UNAUTHORIZED, "bad_refresh");
    };
    // Como o Hub real: o refresh apaga a sessão inteira, então o access antigo deixa de valer na hora.
    f.access.remove(&format!("at_{SECRET}_{g}"));
    issue(&mut f)
}

async fn logout(State(s): State<Shared>, h: HeaderMap, body: Option<Json<Value>>) -> Response {
    let mut f = s.lock().unwrap();
    let at = bearer(&h);
    let rt = body.and_then(|Json(b)| b["refreshToken"].as_str().map(str::to_string));
    if let Some(a) = &at {
        f.access.remove(a);
    }
    if let Some(r) = &rt {
        f.refresh.remove(r);
    }
    f.logouts.push((at, rt));
    Json(json!({ "ok": true })).into_response()
}

async fn me(State(s): State<Shared>, h: HeaderMap) -> Response {
    let delay = {
        let mut f = s.lock().unwrap();
        f.me_calls += 1;
        std::mem::take(&mut f.me_delay_ms)
    };
    if delay > 0 {
        tokio::time::sleep(std::time::Duration::from_millis(delay)).await;
    }
    let f = s.lock().unwrap();
    match bearer(&h) {
        Some(a) if f.access.contains_key(&a) => Json(json!({
            "id": "usr_1", "discordId": "123", "name": "Robson <b>", "avatar": "0123456789abcdef0123456789abcdef",
            "isPlatformAdmin": false
        }))
        .into_response(),
        _ => err(StatusCode::UNAUTHORIZED, "unauthorized"),
    }
}

async fn servers() -> Response {
    Json(json!([{
        "id": "srv_A", "name": "Castelo", "description": "d", "tags": ["rp"], "locale": "pt-BR", "players": 3,
        "maxPlayers": 512, "host": "2001:db8::1", "gamePort": 30500, "httpPort": 30501, "version": "1.0",
        "protocol": 1, "featured": true
    }]))
    .into_response()
}

async fn server(Path(id): Path<String>) -> Response {
    match id.as_str() {
        "srv_off" => Json(json!({
            "id": "srv_off", "name": "Off", "description": "", "tags": [], "locale": "pt-BR", "featured": false,
            "online": false
        }))
        .into_response(),
        _ => err(StatusCode::NOT_FOUND, "not_found"),
    }
}

async fn tickets(State(s): State<Shared>, h: HeaderMap, Json(b): Json<Value>) -> Response {
    let f = s.lock().unwrap();
    match bearer(&h) {
        Some(a) if f.access.contains_key(&a) => {}
        _ => return err(StatusCode::UNAUTHORIZED, "unauthorized"),
    }
    if f.banned {
        return err(StatusCode::FORBIDDEN, "banned_global");
    }
    match b["serverId"].as_str() {
        Some("srv_A") => {
            Json(json!({ "ticket": format!("tk_{SECRET}"), "expiresAt": T0 + 300 })).into_response()
        }
        Some("srv_estranho") => err(StatusCode::IM_A_TEAPOT, "codigo_novo"),
        _ => err(StatusCode::NOT_FOUND, "not_found"),
    }
}

async fn image(Path(nome): Path<String>) -> Response {
    let png = |body: Vec<u8>| ([("content-type", "image/png")], body).into_response();
    match nome.as_str() {
        "ok.png" => png(vec![0x89, b'P', b'N', b'G']),
        "grande.png" => png(vec![0; 300 * 1024]),
        "pagina.png" => ([("content-type", "text/html")], "<script>").into_response(),
        _ => StatusCode::NOT_FOUND.into_response(),
    }
}

struct Env {
    base: Url,
    fake: Shared,
    vault: Arc<MemoryVault>,
    /// Relógio (monotônico) do launcher.
    clock: Arc<AtomicI64>,
    /// Relógio do Hub falso.
    hub_clock: Arc<AtomicI64>,
    auth: Auth,
}

async fn env() -> Env {
    logs();
    let clock = Arc::new(AtomicI64::new(T0));
    let hub_clock = Arc::new(AtomicI64::new(T0));
    let fake: Shared = Arc::new(Mutex::new(Fake {
        now: hub_clock.clone(),
        ..Fake::default()
    }));
    let app = Router::new()
        .route("/v1/auth/exchange", post(exchange))
        .route("/v1/auth/refresh", post(refresh))
        .route("/v1/auth/logout", post(logout))
        .route("/v1/me", get(me))
        .route("/v1/servers", get(servers))
        .route("/v1/servers/{id}", get(server))
        .route("/v1/tickets", post(tickets))
        .route("/img/{nome}", get(image))
        .with_state(fake.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    let base = Url::parse(&format!("http://{addr}/")).unwrap();
    let vault = Arc::new(MemoryVault::default());
    let c = clock.clone();
    let now: Clock = Arc::new(move || c.load(Ordering::SeqCst));
    let auth = Auth::with_clock(HubClient::new(base.as_str()).unwrap(), vault.clone(), now);
    Env {
        base,
        fake,
        vault,
        clock,
        hub_clock,
        auth,
    }
}

fn param(u: &Url, k: &str) -> String {
    u.query_pairs()
        .find(|(n, _)| n == k)
        .map(|(_, v)| v.into_owned())
        .unwrap_or_else(|| panic!("{k} ausente em {u}"))
}

impl Env {
    /// Começa o login e faz o "Hub" emitir um código para o challenge da URL. Devolve (state, código).
    fn start(&self, code: &str) -> (String, String) {
        let url = self.auth.begin_login();
        let challenge = param(&url, "code_challenge");
        self.fake
            .lock()
            .unwrap()
            .codes
            .insert(code.into(), challenge);
        (param(&url, "state"), code.to_string())
    }

    async fn login(&self) {
        let (state, code) = self.start(&format!("lc_{SECRET}"));
        self.auth
            .on_deep_link(&format!("cbmp://auth?code={code}&state={state}"))
            .await
            .unwrap();
    }
}

// ----- PKCE -----

#[test]
fn pkce_challenge_e_base64url_do_sha256_do_verifier() {
    let mut seen = std::collections::HashSet::new();
    for _ in 0..50 {
        let p = auth::pkce();
        assert_eq!(p.verifier.len(), 64);
        assert!(p
            .verifier
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || "-._~".contains(c)));
        assert_eq!(
            p.challenge,
            URL_SAFE_NO_PAD.encode(Sha256::digest(p.verifier.as_bytes()))
        );
        assert_eq!(p.challenge.len(), 43);
        assert_eq!(challenge_for(&p.verifier), p.challenge);
        assert!(seen.insert(p.verifier));
        let s = auth::new_state();
        assert_eq!(s.len(), 43, "32 bytes em base64url");
        assert!(s
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_'));
        assert!(seen.insert(s));
    }
    // Vetor do RFC 7636 (apêndice B).
    assert_eq!(
        challenge_for("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
        "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn begin_login_monta_a_url_do_hub() {
    let e = env().await;
    let u = e.auth.begin_login();
    assert_eq!(u.path(), "/v1/auth/discord/login");
    assert_eq!(u.origin(), e.base.origin());
    assert_eq!(param(&u, "client"), "launcher");
    assert_eq!(param(&u, "code_challenge_method"), "S256");
    assert_eq!(param(&u, "code_challenge").len(), 43);
    assert_eq!(param(&u, "state").len(), 43);
    let again = e.auth.begin_login();
    assert_ne!(param(&u, "state"), param(&again, "state"));
    assert_ne!(param(&u, "code_challenge"), param(&again, "code_challenge"));
}

// ----- deep link -----

#[tokio::test(flavor = "multi_thread")]
async fn deep_link_invalido_e_ignorado_com_codigo_sem_gastar_o_login() {
    let e = env().await;
    let (state, code) = e.start("lc_ok");
    let other = auth::new_state();
    let cases = [
        (
            format!("cbmp://auth?code={code}&state={other}"),
            "login_state_invalid",
        ),
        (
            format!("https://auth?code={code}&state={state}"),
            "invalid_deep_link",
        ),
        (
            format!("cbmp://outro?code={code}&state={state}"),
            "invalid_deep_link",
        ),
        (
            format!("cbmp://auth/x?code={code}&state={state}"),
            "invalid_deep_link",
        ),
        (format!("cbmp://auth?code={code}"), "invalid_deep_link"),
        (
            format!("cbmp://auth?code={code}&state={state}&state={state}"),
            "invalid_deep_link",
        ),
        (
            format!("cbmp://auth?code={code}&error=access_denied&state={state}"),
            "invalid_deep_link",
        ),
        (
            format!("cbmp://auth?code=a%20b&state={state}"),
            "invalid_deep_link",
        ),
        (
            format!("cbmp://auth?code={code}&state={state}&x=1"),
            "invalid_deep_link",
        ),
        ("nada disso".to_string(), "invalid_deep_link"),
    ];
    for (link, want) in &cases {
        let got = e.auth.on_deep_link(link).await.unwrap_err();
        assert_eq!(got.code, *want, "{link}");
    }
    assert_eq!(
        e.fake.lock().unwrap().exchanges,
        0,
        "nenhum ignorado chega ao Hub"
    );
    // O login pendente continua valendo (barra final também é aceita).
    e.auth
        .on_deep_link(&format!("cbmp://auth/?code={code}&state={state}"))
        .await
        .unwrap();
    // Repetido: state é de uso único.
    let again = e
        .auth
        .on_deep_link(&format!("cbmp://auth?code={code}&state={state}"))
        .await
        .unwrap_err();
    assert_eq!(again.code, "login_state_invalid");
    assert_eq!(e.fake.lock().unwrap().exchanges, 1);
}

#[tokio::test(flavor = "multi_thread")]
async fn deep_link_vencido_ou_sem_login_pendente() {
    let e = env().await;
    let none = e
        .auth
        .on_deep_link(&format!("cbmp://auth?code=x&state={}", auth::new_state()))
        .await
        .unwrap_err();
    assert_eq!(none.code, "login_state_invalid");

    let (state, code) = e.start("lc_venc");
    e.clock
        .fetch_add(PENDING_TTL_SECS as i64 + 1, Ordering::SeqCst);
    let late = e
        .auth
        .on_deep_link(&format!("cbmp://auth?code={code}&state={state}"))
        .await
        .unwrap_err();
    assert_eq!(late.code, "login_state_invalid");
    assert_eq!(e.fake.lock().unwrap().exchanges, 0);

    // No limite (10 min exatos) ainda vale.
    let (state, code) = e.start("lc_limite");
    e.clock.fetch_add(PENDING_TTL_SECS as i64, Ordering::SeqCst);
    e.auth
        .on_deep_link(&format!("cbmp://auth?code={code}&state={state}"))
        .await
        .unwrap();
}

#[tokio::test(flavor = "multi_thread")]
async fn novo_login_substitui_o_pendente() {
    let e = env().await;
    let (old_state, old_code) = e.start("lc_velho");
    let (state, code) = e.start("lc_novo");
    let old = e
        .auth
        .on_deep_link(&format!("cbmp://auth?code={old_code}&state={old_state}"))
        .await
        .unwrap_err();
    assert_eq!(old.code, "login_state_invalid");
    e.auth
        .on_deep_link(&format!("cbmp://auth?code={code}&state={state}"))
        .await
        .unwrap();
}

#[tokio::test(flavor = "multi_thread")]
async fn access_denied_vira_login_denied_e_consome_o_state() {
    let e = env().await;
    let (state, _) = e.start("lc_negado");
    let got = e
        .auth
        .on_deep_link(&format!("cbmp://auth?error=access_denied&state={state}"))
        .await
        .unwrap_err();
    assert_eq!(got.code, "login_denied");
    let again = e
        .auth
        .on_deep_link(&format!("cbmp://auth?error=access_denied&state={state}"))
        .await
        .unwrap_err();
    assert_eq!(again.code, "login_state_invalid");
    assert_eq!(e.vault.get().unwrap(), None);

    let (state, _) = e.start("lc_outro_erro");
    let other = e
        .auth
        .on_deep_link(&format!("cbmp://auth?error=server_error&state={state}"))
        .await
        .unwrap_err();
    assert_eq!(other.code, "login_failed");
}

// ----- troca e refresh -----

#[tokio::test(flavor = "multi_thread")]
async fn troca_guarda_refresh_no_cofre_e_access_so_em_memoria() {
    let e = env().await;
    assert_eq!(
        e.auth.access_token().await.unwrap_err().code,
        "not_logged_in"
    );
    e.login().await;
    assert_eq!(
        e.vault.get().unwrap().as_deref(),
        Some(format!("rt_{SECRET}_1").as_str())
    );
    assert_eq!(
        e.auth.access_token().await.unwrap(),
        format!("at_{SECRET}_1")
    );
    assert_eq!(e.fake.lock().unwrap().refreshes, 0);

    let me = e.auth.me().await.unwrap();
    assert_eq!(me.name, "Robson <b>");
    assert_eq!(me.discord_id, "123");
}

#[tokio::test(flavor = "multi_thread")]
async fn codigo_recusado_pelo_hub_vira_login_failed() {
    let e = env().await;
    let url = e.auth.begin_login();
    // Hub emitiu o código para outro challenge (ex.: código de outra pessoa).
    e.fake
        .lock()
        .unwrap()
        .codes
        .insert("lc_alheio".into(), auth::pkce().challenge);
    let got = e
        .auth
        .on_deep_link(&format!(
            "cbmp://auth?code=lc_alheio&state={}",
            param(&url, "state")
        ))
        .await
        .unwrap_err();
    assert_eq!(got.code, "login_failed");
    assert_eq!(e.vault.get().unwrap(), None);
}

#[tokio::test(flavor = "multi_thread")]
async fn refresh_automatico_com_margem_de_60s_gira_os_tokens() {
    let e = env().await;
    e.login().await;
    // 61 s antes de vencer: ainda usa o atual.
    e.clock.store(T0 + ACCESS_TTL - 61, Ordering::SeqCst);
    assert_eq!(
        e.auth.access_token().await.unwrap(),
        format!("at_{SECRET}_1")
    );
    // 60 s antes: renova.
    e.clock.store(T0 + ACCESS_TTL - 60, Ordering::SeqCst);
    assert_eq!(
        e.auth.access_token().await.unwrap(),
        format!("at_{SECRET}_2")
    );
    assert_eq!(
        e.vault.get().unwrap().as_deref(),
        Some(format!("rt_{SECRET}_2").as_str())
    );
    assert_eq!(e.fake.lock().unwrap().refreshes, 1);
    assert!(
        !e.fake
            .lock()
            .unwrap()
            .refresh
            .contains_key(&format!("rt_{SECRET}_1")),
        "o refresh antigo foi girado"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn refresh_concorrente_chama_o_hub_uma_vez() {
    let e = Arc::new(env().await);
    e.login().await;
    e.clock.store(T0 + ACCESS_TTL, Ordering::SeqCst);
    let mut hs = Vec::new();
    for _ in 0..8 {
        let e = e.clone();
        hs.push(tokio::spawn(
            async move { e.auth.access_token().await.unwrap() },
        ));
    }
    for h in hs {
        assert_eq!(h.await.unwrap(), format!("at_{SECRET}_2"));
    }
    assert_eq!(e.fake.lock().unwrap().refreshes, 1);
}

#[tokio::test(flavor = "multi_thread")]
async fn sessao_do_cofre_e_renovada_ao_abrir() {
    let e = env().await;
    e.fake
        .lock()
        .unwrap()
        .refresh
        .insert(format!("rt_{SECRET}_antigo"), 0);
    e.vault.set(&format!("rt_{SECRET}_antigo")).unwrap();
    assert!(e.auth.has_stored_session());
    assert_eq!(
        e.auth.access_token().await.unwrap(),
        format!("at_{SECRET}_1")
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn refresh_401_limpa_o_cofre_e_vira_session_expired() {
    let e = env().await;
    e.login().await;
    // Hub revogou a sessão.
    e.fake.lock().unwrap().refresh.clear();
    e.clock.store(T0 + ACCESS_TTL + 5, Ordering::SeqCst);
    let got = e.auth.access_token().await.unwrap_err();
    assert_eq!(got.code, "session_expired");
    assert_eq!(e.vault.get().unwrap(), None);
    assert!(!e.auth.has_stored_session());
    assert_eq!(
        e.auth.access_token().await.unwrap_err().code,
        "not_logged_in"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn hub_fora_no_refresh_mantem_o_cofre() {
    let e = env().await;
    e.vault.set("rt_qualquer").unwrap();
    let dead = Auth::with_clock(
        HubClient::new("http://127.0.0.1:9/").unwrap(),
        e.vault.clone(),
        Arc::new(|| T0),
    );
    assert_eq!(
        dead.access_token().await.unwrap_err().code,
        "hub_unreachable"
    );
    assert_eq!(e.vault.get().unwrap().as_deref(), Some("rt_qualquer"));
}

#[tokio::test(flavor = "multi_thread")]
async fn chamada_em_voo_durante_rotacao_nao_derruba_a_sessao() {
    let e = Arc::new(env().await);
    e.login().await;
    // `me` sai com at_1 e o Hub só confere depois de 300 ms; nesse meio-tempo outro chamador gira os tokens.
    e.fake.lock().unwrap().me_delay_ms = 300;
    let e2 = e.clone();
    let inflight = tokio::spawn(async move { e2.auth.me().await });
    tokio::time::sleep(std::time::Duration::from_millis(100)).await;
    e.clock.store(T0 + ACCESS_TTL - 60, Ordering::SeqCst);
    assert_eq!(
        e.auth.access_token().await.unwrap(),
        format!("at_{SECRET}_2")
    );

    let me = inflight.await.unwrap().unwrap();
    assert_eq!(me.discord_id, "123");
    let f = e.fake.lock().unwrap();
    assert_eq!(
        f.me_calls, 2,
        "401 com o token velho → repete uma vez com o atual"
    );
    assert_eq!(
        f.refreshes, 1,
        "o token já girado não provoca outro refresh"
    );
    drop(f);
    assert_eq!(
        e.vault.get().unwrap().as_deref(),
        Some(format!("rt_{SECRET}_2").as_str())
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn chamadas_concorrentes_com_401_fazem_um_refresh_so() {
    let e = Arc::new(env().await);
    e.login().await;
    // Hub esqueceu o access (ex.: relógio do launcher atrasado), mas o refresh vale.
    e.fake.lock().unwrap().access.clear();
    let mut hs = Vec::new();
    for _ in 0..6 {
        let e = e.clone();
        hs.push(tokio::spawn(async move { e.auth.me().await }));
    }
    for h in hs {
        assert_eq!(h.await.unwrap().unwrap().name, "Robson <b>");
    }
    assert_eq!(e.fake.lock().unwrap().refreshes, 1);
    assert_eq!(
        e.vault.get().unwrap().as_deref(),
        Some(format!("rt_{SECRET}_2").as_str())
    );
    assert_eq!(
        e.auth.access_token().await.unwrap(),
        format!("at_{SECRET}_2")
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn refresh_com_hub_em_erro_mantem_cofre_e_token_ainda_valido() {
    let e = env().await;
    e.login().await;
    e.fake.lock().unwrap().refresh_down = true;
    // Dentro da margem de 60 s mas ainda válido: refresh falha, o token atual continua servindo.
    e.clock.store(T0 + ACCESS_TTL - 30, Ordering::SeqCst);
    assert_eq!(
        e.auth.access_token().await.unwrap(),
        format!("at_{SECRET}_1")
    );
    // Vencido: erro do Hub, mas nada é apagado.
    e.clock.store(T0 + ACCESS_TTL + 1, Ordering::SeqCst);
    assert_eq!(e.auth.access_token().await.unwrap_err().code, "hub_busy");
    assert_eq!(
        e.vault.get().unwrap().as_deref(),
        Some(format!("rt_{SECRET}_1").as_str())
    );
    // 401 numa chamada + refresh com erro do Hub: também não apaga.
    e.clock.store(T0, Ordering::SeqCst);
    e.fake.lock().unwrap().access.clear();
    assert_eq!(e.auth.me().await.unwrap_err().code, "hub_busy");
    assert!(e.auth.has_stored_session());
    // Hub volta: a mesma sessão renova.
    e.fake.lock().unwrap().refresh_down = false;
    assert_eq!(e.auth.me().await.unwrap().discord_id, "123");
}

#[tokio::test(flavor = "multi_thread")]
async fn relogio_local_errado_nao_desloga_nem_renova_a_toa() {
    for skew in [7200, -7200] {
        let e = env().await;
        e.hub_clock.store(T0 + skew, Ordering::SeqCst);
        e.login().await;
        // Validade conta a partir do recebimento (900 s pelo `Date` do Hub), não da hora absoluta.
        e.clock.store(T0 + ACCESS_TTL - 61, Ordering::SeqCst);
        assert_eq!(
            e.auth.access_token().await.unwrap(),
            format!("at_{SECRET}_1")
        );
        assert_eq!(e.auth.me().await.unwrap().discord_id, "123");
        assert_eq!(e.fake.lock().unwrap().refreshes, 0, "skew {skew}");
        e.clock.store(T0 + ACCESS_TTL - 60, Ordering::SeqCst);
        assert_eq!(
            e.auth.access_token().await.unwrap(),
            format!("at_{SECRET}_2")
        );
        assert!(e.auth.has_stored_session());
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn login_por_cima_de_sessao_viva_encerra_a_antiga() {
    let e = env().await;
    e.login().await;
    e.login().await;
    assert_eq!(
        e.vault.get().unwrap().as_deref(),
        Some(format!("rt_{SECRET}_2").as_str())
    );
    assert_eq!(
        e.fake.lock().unwrap().logouts,
        vec![(
            Some(format!("at_{SECRET}_1")),
            Some(format!("rt_{SECRET}_1"))
        )]
    );
    assert_eq!(
        e.auth.access_token().await.unwrap(),
        format!("at_{SECRET}_2")
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn me_com_401_derruba_a_sessao() {
    let e = env().await;
    e.login().await;
    e.fake.lock().unwrap().access.clear();
    e.fake.lock().unwrap().refresh.clear();
    assert_eq!(e.auth.me().await.unwrap_err().code, "session_expired");
    assert_eq!(e.vault.get().unwrap(), None);
}

#[tokio::test(flavor = "multi_thread")]
async fn logout_avisa_o_hub_e_limpa_o_cofre() {
    let e = env().await;
    e.login().await;
    e.auth.logout().await.unwrap();
    assert_eq!(e.vault.get().unwrap(), None);
    assert_eq!(
        e.fake.lock().unwrap().logouts,
        vec![(
            Some(format!("at_{SECRET}_1")),
            Some(format!("rt_{SECRET}_1"))
        )]
    );
    assert_eq!(
        e.auth.access_token().await.unwrap_err().code,
        "not_logged_in"
    );
    // Logout sem sessão não chama o Hub e não falha.
    e.auth.logout().await.unwrap();
    assert_eq!(e.fake.lock().unwrap().logouts.len(), 1);
}

#[tokio::test(flavor = "multi_thread")]
async fn logout_com_hub_fora_ainda_limpa_o_cofre() {
    let vault = Arc::new(MemoryVault::default());
    vault.set("rt_x").unwrap();
    let a = Auth::with_clock(
        HubClient::new("http://127.0.0.1:9/").unwrap(),
        vault.clone(),
        Arc::new(|| T0),
    );
    a.logout().await.unwrap();
    assert_eq!(vault.get().unwrap(), None);
}

// ----- cliente do Hub -----

#[tokio::test(flavor = "multi_thread")]
async fn hub_lista_detalhe_ticket_e_erros_estaveis() {
    let e = env().await;
    let hub = HubClient::new(e.base.as_str()).unwrap();
    let list = hub.servers().await.unwrap();
    assert_eq!(list.len(), 1);
    assert_eq!(list[0].id, "srv_A");
    assert_eq!(list[0].host, "2001:db8::1");
    assert_eq!((list[0].game_port, list[0].http_port), (30500, 30501));
    assert!(list[0].featured);

    let off = hub.server("srv_off").await.unwrap();
    assert!(!off.online);
    assert_eq!(off.host, None);
    assert_eq!(hub.server("nao").await.unwrap_err().code, "not_found");
    assert_eq!(hub.server("a/b").await.unwrap_err().code, "not_found");

    e.login().await;
    let at = e.auth.access_token().await.unwrap();
    assert_eq!(
        hub.ticket("srv_A", &at).await.unwrap(),
        format!("tk_{SECRET}")
    );
    assert_eq!(
        e.auth.ticket("srv_A").await.unwrap(),
        format!("tk_{SECRET}")
    );
    assert_eq!(
        hub.ticket("srv_estranho", &at).await.unwrap_err().code,
        "hub_error"
    );
    assert_eq!(
        hub.ticket("srv_A", "at_x").await.unwrap_err().code,
        "session_expired"
    );
    e.fake.lock().unwrap().banned = true;
    let b = hub.ticket("srv_A", &at).await.unwrap_err();
    assert_eq!(b.code, "banned_global");
    assert!(!b.message.is_empty());

    let dead = HubClient::new("http://127.0.0.1:9/").unwrap();
    assert_eq!(dead.servers().await.unwrap_err().code, "hub_unreachable");
}

#[tokio::test(flavor = "multi_thread")]
async fn avatar_vira_data_url_so_de_imagem_pequena_do_prefixo_permitido() {
    use cbmp_launcher_lib::hub::{fetch_image_data_url, http_client};
    let e = env().await;
    let http = http_client();
    let prefix = e.base.join("img/").unwrap().to_string();
    let get = |n: &str| e.base.join(&format!("img/{n}")).unwrap().to_string();
    assert_eq!(
        fetch_image_data_url(&http, &get("ok.png"), &prefix)
            .await
            .as_deref(),
        Some("data:image/png;base64,iVBORw==")
    );
    assert_eq!(
        fetch_image_data_url(&http, &get("grande.png"), &prefix).await,
        None
    );
    assert_eq!(
        fetch_image_data_url(&http, &get("pagina.png"), &prefix).await,
        None
    );
    assert_eq!(
        fetch_image_data_url(&http, &get("falta.png"), &prefix).await,
        None
    );
    // Fora do prefixo nem tenta.
    let other = e.base.join("v1/servers").unwrap().to_string();
    assert_eq!(fetch_image_data_url(&http, &other, &prefix).await, None);
}

#[test]
fn hub_client_recusa_http_fora_do_loopback() {
    assert!(HubClient::new("http://hub.example.com/").is_err());
    assert!(HubClient::new("ftp://127.0.0.1/").is_err());
    assert!(HubClient::new("https://hub.example.com").is_ok());
    // Loopback http só vale em build de debug.
    assert_eq!(
        HubClient::new("http://localhost:8787").is_ok(),
        cfg!(debug_assertions)
    );
}

// ----- logs -----

#[tokio::test(flavor = "multi_thread")]
async fn nada_de_token_codigo_verifier_ou_state_nos_logs() {
    let e = env().await;
    let (state, code) = e.start(&format!("lc_{SECRET}_log"));
    let _ = e
        .auth
        .on_deep_link(&format!(
            "cbmp://auth?code={code}&state={}",
            auth::new_state()
        ))
        .await;
    let _ = e
        .auth
        .on_deep_link(&format!("cbmp://outro?code={code}&state={state}"))
        .await;
    e.auth
        .on_deep_link(&format!("cbmp://auth?code={code}&state={state}"))
        .await
        .unwrap();
    e.clock.store(T0 + ACCESS_TTL, Ordering::SeqCst);
    e.auth.access_token().await.unwrap();
    let _ = e.auth.ticket("srv_A").await;
    e.fake.lock().unwrap().refresh.clear();
    e.clock.store(T0 + 10 * ACCESS_TTL, Ordering::SeqCst);
    let _ = e.auth.access_token().await;
    e.auth.logout().await.unwrap();

    let lines = logs().0.lock().unwrap().clone();
    assert!(
        lines.iter().any(|l| l.contains("deep link ignorado")),
        "o teste precisa ver os logs do auth: {lines:?}"
    );
    for l in &lines {
        assert!(!l.contains(SECRET), "segredo no log: {l}");
        assert!(!l.contains(&state), "state no log: {l}");
        assert!(!l.contains("lc_"), "código no log: {l}");
    }
}

// ----- cofre real (manual) -----

/// Grava, lê e apaga no Credential Manager de verdade (serviço de teste, não o do launcher).
/// Rodar à mão: `cargo test --test auth -- --ignored cofre_real`.
#[test]
#[ignore]
fn cofre_real_do_windows() {
    let v = auth::KeyringVault::with_names("CBMultiplayer-teste", "refresh_token");
    v.clear().unwrap();
    assert_eq!(v.get().unwrap(), None);
    v.set("rt_teste_manual").unwrap();
    assert_eq!(v.get().unwrap().as_deref(), Some("rt_teste_manual"));
    v.set("rt_teste_manual_2").unwrap();
    assert_eq!(v.get().unwrap().as_deref(), Some("rt_teste_manual_2"));
    v.clear().unwrap();
    v.clear().unwrap();
    assert_eq!(v.get().unwrap(), None);
}

/// Cofre cujo `set` pode falhar sob comando (Credential Manager indisponível).
#[derive(Default)]
struct FlakyVault {
    inner: MemoryVault,
    fail_set: std::sync::atomic::AtomicBool,
}

impl TokenVault for FlakyVault {
    fn get(&self) -> cbmp_launcher_lib::error::AppResult<Option<String>> {
        self.inner.get()
    }
    fn set(&self, rt: &str) -> cbmp_launcher_lib::error::AppResult<()> {
        if self.fail_set.load(Ordering::SeqCst) {
            return Err(cbmp_launcher_lib::error::AppError::new("vault_error", "x"));
        }
        self.inner.set(rt)
    }
    fn clear(&self) -> cbmp_launcher_lib::error::AppResult<()> {
        self.inner.clear()
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn cofre_falhando_depois_da_rotacao_mantem_a_sessao_desta_execucao() {
    let e = env().await;
    let vault = Arc::new(FlakyVault::default());
    let c = e.clock.clone();
    let auth = Auth::with_clock(
        HubClient::new(e.base.as_str()).unwrap(),
        vault.clone(),
        Arc::new(move || c.load(Ordering::SeqCst)),
    );
    let url = auth.begin_login();
    e.fake
        .lock()
        .unwrap()
        .codes
        .insert("lc_cofre".into(), param(&url, "code_challenge"));
    auth.on_deep_link(&format!(
        "cbmp://auth?code=lc_cofre&state={}",
        param(&url, "state")
    ))
    .await
    .unwrap();
    vault.fail_set.store(true, Ordering::SeqCst);
    e.clock.store(T0 + ACCESS_TTL, Ordering::SeqCst);
    // O Hub já girou: o access novo vale nesta execução mesmo sem conseguir gravar o refresh novo.
    assert_eq!(auth.access_token().await.unwrap(), format!("at_{SECRET}_2"));
    assert_eq!(auth.me().await.unwrap().discord_id, "123");
}
