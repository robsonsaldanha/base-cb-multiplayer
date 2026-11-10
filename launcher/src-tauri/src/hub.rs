//! Cliente HTTP do Hub (`/v1/...`). Só `https://`; `http://` de loopback apenas em build de debug.
//!
//! Erros do Hub (`{error, message}`) viram [`AppError`] pelo código estável: códigos conhecidos passam com mensagem
//! pt-BR do launcher, 401 vira `session_expired`, `bad_code` vira `login_failed` e o resto vira `hub_error`. Falha de
//! rede é `hub_unreachable`; resposta fora do contrato, `hub_bad_response`. Tokens nunca entram em mensagens de erro.

use crate::error::{AppError, AppResult};
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use std::fmt;
use std::time::Duration;
use url::Url;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);
/// Teto do corpo das respostas JSON do Hub (a lista de servidores é a maior).
const MAX_BODY: usize = 4 * 1024 * 1024;
/// Teto da imagem de avatar baixada do CDN do Discord.
const MAX_AVATAR: usize = 256 * 1024;
/// Único lugar de onde o launcher baixa avatar.
pub const AVATAR_PREFIX: &str = "https://cdn.discordapp.com/";

/// `GET /v1/servers` (contrato `ServerListing`). `host` pode ser IPv6.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ServerListing {
    pub id: String,
    pub name: String,
    pub description: String,
    pub tags: Vec<String>,
    pub locale: String,
    pub players: i64,
    pub max_players: i64,
    pub host: String,
    pub game_port: u16,
    pub http_port: u16,
    pub version: String,
    pub protocol: i64,
    pub featured: bool,
}

/// `GET /v1/servers/:id` (contrato `ServerDetail`): offline traz só os dados básicos.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ServerDetail {
    pub id: String,
    pub name: String,
    pub description: String,
    pub tags: Vec<String>,
    pub locale: String,
    pub featured: bool,
    pub online: bool,
    #[serde(default)]
    pub players: Option<i64>,
    #[serde(default)]
    pub max_players: Option<i64>,
    #[serde(default)]
    pub host: Option<String>,
    #[serde(default)]
    pub game_port: Option<u16>,
    #[serde(default)]
    pub http_port: Option<u16>,
    #[serde(default)]
    pub version: Option<String>,
    #[serde(default)]
    pub protocol: Option<i64>,
}

/// `GET /v1/me`. `avatar` é o hash do Discord (não URL).
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Me {
    pub id: String,
    pub discord_id: String,
    pub name: String,
    pub avatar: Option<String>,
    #[serde(default)]
    pub is_platform_admin: bool,
}

/// Resposta de `/v1/auth/exchange` e `/v1/auth/refresh` (unix seconds). `Debug` não mostra os tokens.
#[derive(Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Tokens {
    pub access_token: String,
    pub access_expires_at: i64,
    pub refresh_token: Option<String>,
    pub refresh_expires_at: Option<i64>,
    /// Hora do Hub (cabeçalho `Date`, unix seconds) quando respondeu. Com ela a validade vira relativa
    /// (`accessExpiresAt - server_time`), imune a relógio local errado.
    #[serde(skip)]
    pub server_time: Option<i64>,
}

impl Tokens {
    /// Segundos de validade do access token a partir do recebimento. Sem `Date`, usa o relógio local (`wall_now`).
    pub fn access_lifetime(&self, wall_now: i64) -> i64 {
        (self.access_expires_at - self.server_time.unwrap_or(wall_now)).max(0)
    }
}

impl fmt::Debug for Tokens {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Tokens")
            .field("access_token", &"<oculto>")
            .field("access_expires_at", &self.access_expires_at)
            .field(
                "refresh_token",
                &self.refresh_token.as_ref().map(|_| "<oculto>"),
            )
            .field("refresh_expires_at", &self.refresh_expires_at)
            .field("server_time", &self.server_time)
            .finish()
    }
}

/// `GET /v1/mod/latest?protocol=N` (contrato `ModInfo`). `signature` é o conteúdo do `.sig` (minisign).
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct ModInfo {
    pub version: String,
    pub protocol: u32,
    pub url: String,
    pub sha256: String,
    pub size: u64,
    pub signature: String,
}

#[derive(Deserialize)]
struct TicketResponse {
    ticket: String,
}

#[derive(Deserialize)]
struct ErrorBody {
    error: String,
}

pub struct HubClient {
    base: Url,
    http: reqwest::Client,
}

/// `https://`, ou `http://` de loopback em build de debug. Sem usuário/senha.
pub fn allowed_url(url: &Url, dev: bool) -> bool {
    if !url.username().is_empty() || url.password().is_some() {
        return false;
    }
    match url.scheme() {
        "https" => url.host().is_some(),
        "http" => dev && url.host().is_some_and(|h| is_loopback(&h)),
        _ => false,
    }
}

fn is_loopback(host: &url::Host<&str>) -> bool {
    match host {
        url::Host::Domain(d) => d.eq_ignore_ascii_case("localhost"),
        url::Host::Ipv4(ip) => ip.is_loopback(),
        url::Host::Ipv6(ip) => ip.is_loopback(),
    }
}

/// Cliente HTTP comum (rustls + `ring`, sem seguir redirecionamentos, prazos fixos).
pub fn http_client() -> reqwest::Client {
    if rustls::crypto::CryptoProvider::get_default().is_none() {
        let _ = rustls::crypto::ring::default_provider().install_default();
    }
    reqwest::Client::builder()
        .connect_timeout(CONNECT_TIMEOUT)
        .timeout(REQUEST_TIMEOUT)
        .redirect(reqwest::redirect::Policy::none())
        .user_agent(concat!("CBMultiplayer/", env!("CARGO_PKG_VERSION")))
        .build()
        .expect("cliente HTTP")
}

/// Cliente para arquivos grandes do Hub (mod, update): sem prazo total, mas conexão em 10 s e no máx. 30 s sem bytes.
/// O Hub derruba quem fica abaixo de 32 KiB/s por 30 s e não serve `Range`: quem chama tenta de novo do zero.
pub fn download_client() -> reqwest::Client {
    if rustls::crypto::CryptoProvider::get_default().is_none() {
        let _ = rustls::crypto::ring::default_provider().install_default();
    }
    reqwest::Client::builder()
        .connect_timeout(CONNECT_TIMEOUT)
        .read_timeout(Duration::from_secs(30))
        .redirect(reqwest::redirect::Policy::none())
        .user_agent(concat!("CBMultiplayer/", env!("CARGO_PKG_VERSION")))
        .build()
        .expect("cliente HTTP")
}

/// Download do Hub que caiu ou ficou lento demais em todas as tentativas. `what`: "o mod", "a atualização".
pub fn download_slow(what: &str) -> AppError {
    AppError::new(
        "download_slow",
        format!(
            "A conexão está lenta demais ou caiu ao baixar {what} (o Hub exige pelo menos 32 KiB/s). Tente de \
             novo em instantes ou em outra rede."
        ),
    )
}

fn unreachable_err() -> AppError {
    AppError::new(
        "hub_unreachable",
        "Não foi possível falar com o Hub. Verifique sua conexão.",
    )
}

fn bad_response() -> AppError {
    AppError::new("hub_bad_response", "O Hub respondeu algo inesperado.")
}

/// Erro do Hub → código estável do launcher.
pub fn map_hub_error(status: u16, code: Option<&str>) -> AppError {
    if status == 401 {
        return AppError::new("session_expired", "Sua sessão expirou. Entre de novo.");
    }
    let (code, message): (&'static str, &str) = match code.unwrap_or("") {
        "bad_code" => (
            "login_failed",
            "Não foi possível concluir o login. Tente de novo.",
        ),
        "banned_global" => (
            "banned_global",
            "Sua conta está na lista global de banimentos.",
        ),
        "not_found" => ("not_found", "Servidor não encontrado."),
        "rate_limited" => (
            "rate_limited",
            "Muitas tentativas seguidas. Aguarde um pouco.",
        ),
        "forbidden" => ("forbidden", "Acesso negado pelo Hub."),
        "invalid_body" => ("hub_rejected", "O Hub recusou o pedido do launcher."),
        "updates_disabled" => ("updates_disabled", "Atualizações indisponíveis no momento."),
        "no_mod" => (
            "mod_unavailable",
            "Ainda não há mod publicado para a versão deste servidor.",
        ),
        "busy" | "too_many_downloads" => ("hub_busy", "O Hub está ocupado. Tente daqui a pouco."),
        _ => {
            return AppError::new(
                "hub_error",
                format!("O Hub respondeu com erro (HTTP {status})."),
            )
        }
    };
    AppError::new(code, message)
}

/// Lê o corpo inteiro até `max` bytes.
async fn read_limited(mut resp: reqwest::Response, max: usize) -> AppResult<Vec<u8>> {
    if resp.content_length().is_some_and(|n| n > max as u64) {
        return Err(bad_response());
    }
    let mut out = Vec::new();
    while let Some(chunk) = resp.chunk().await.map_err(|_| unreachable_err())? {
        if out.len() + chunk.len() > max {
            return Err(bad_response());
        }
        out.extend_from_slice(&chunk);
    }
    Ok(out)
}

impl HubClient {
    /// Valida o endereço (https; http de loopback só em debug).
    pub fn new(base: &str) -> AppResult<Self> {
        let invalid = || AppError::invalid_settings("Endereço do Hub inválido.");
        let base = Url::parse(base).map_err(|_| invalid())?;
        if !allowed_url(&base, cfg!(debug_assertions))
            || base.query().is_some()
            || base.fragment().is_some()
        {
            return Err(invalid());
        }
        Ok(Self {
            base,
            http: http_client(),
        })
    }

    pub fn base(&self) -> &Url {
        &self.base
    }

    /// `base` + caminho, sem perder um prefixo de caminho do Hub (mesma regra do updater).
    fn url(&self, path: &str) -> Url {
        let mut s = self.base.as_str().trim_end_matches('/').to_string();
        s.push('/');
        s.push_str(path);
        Url::parse(&s).expect("caminho fixo do Hub")
    }

    /// Página de login do Hub para o navegador do sistema (PKCE S256 + state do launcher).
    pub fn login_url(&self, code_challenge: &str, state: &str) -> Url {
        let mut u = self.url("v1/auth/discord/login");
        u.query_pairs_mut()
            .append_pair("client", "launcher")
            .append_pair("code_challenge", code_challenge)
            .append_pair("code_challenge_method", "S256")
            .append_pair("state", state);
        u
    }

    async fn send<T: DeserializeOwned>(&self, req: reqwest::RequestBuilder) -> AppResult<T> {
        Ok(self.send_dated(req).await?.0)
    }

    /// Como `send`, devolvendo também o cabeçalho `Date` da resposta (unix seconds), se válido.
    async fn send_dated<T: DeserializeOwned>(
        &self,
        req: reqwest::RequestBuilder,
    ) -> AppResult<(T, Option<i64>)> {
        let resp = req.send().await.map_err(|_| unreachable_err())?;
        let date = resp
            .headers()
            .get(reqwest::header::DATE)
            .and_then(|v| v.to_str().ok())
            .and_then(parse_http_date);
        let status = resp.status();
        let body = read_limited(resp, MAX_BODY).await?;
        if !status.is_success() {
            let code = serde_json::from_slice::<ErrorBody>(&body)
                .ok()
                .map(|b| b.error);
            return Err(map_hub_error(status.as_u16(), code.as_deref()));
        }
        let v = serde_json::from_slice(&body).map_err(|_| bad_response())?;
        Ok((v, date))
    }

    async fn send_tokens(&self, req: reqwest::RequestBuilder) -> AppResult<Tokens> {
        let (mut t, date): (Tokens, _) = self.send_dated(req).await?;
        t.server_time = date;
        Ok(t)
    }

    pub async fn servers(&self) -> AppResult<Vec<ServerListing>> {
        self.send(self.http.get(self.url("v1/servers"))).await
    }

    pub async fn server(&self, id: &str) -> AppResult<ServerDetail> {
        let mut u = self.url("v1/servers");
        u.path_segments_mut().expect("URL http").push(id);
        self.send(self.http.get(u)).await
    }

    /// Ticket de entrada (TTL 300 s). Nunca registrar o valor.
    pub async fn ticket(&self, server_id: &str, access: &str) -> AppResult<String> {
        let r: TicketResponse = self
            .send(
                self.http
                    .post(self.url("v1/tickets"))
                    .bearer_auth(access)
                    .json(&serde_json::json!({ "serverId": server_id })),
            )
            .await?;
        Ok(r.ticket)
    }

    /// Mod mais novo publicado para `protocol`; `None` quando o Hub responde `404 no_mod`.
    pub async fn mod_latest(&self, protocol: u32) -> AppResult<Option<ModInfo>> {
        let mut u = self.url("v1/mod/latest");
        u.query_pairs_mut()
            .append_pair("protocol", &protocol.to_string());
        match self.send(self.http.get(u)).await {
            Ok(m) => Ok(Some(m)),
            Err(e) if e.code == "mod_unavailable" => Ok(None),
            Err(e) => Err(e),
        }
    }

    pub async fn me(&self, access: &str) -> AppResult<Me> {
        self.send(self.http.get(self.url("v1/me")).bearer_auth(access))
            .await
    }

    pub async fn exchange(&self, code: &str, verifier: &str) -> AppResult<Tokens> {
        self.send_tokens(
            self.http
                .post(self.url("v1/auth/exchange"))
                .json(&serde_json::json!({ "code": code, "codeVerifier": verifier })),
        )
        .await
    }

    pub async fn refresh(&self, refresh_token: &str) -> AppResult<Tokens> {
        self.send_tokens(
            self.http
                .post(self.url("v1/auth/refresh"))
                .json(&serde_json::json!({ "refreshToken": refresh_token })),
        )
        .await
    }

    /// Encerra a sessão no Hub (Bearer do access e/ou `{refreshToken}` no corpo).
    pub async fn logout(&self, access: Option<&str>, refresh: Option<&str>) -> AppResult<()> {
        let mut req = self.http.post(self.url("v1/auth/logout"));
        if let Some(a) = access {
            req = req.bearer_auth(a);
        }
        let body = match refresh {
            Some(r) => serde_json::json!({ "refreshToken": r }),
            None => serde_json::json!({}),
        };
        let _: serde_json::Value = self.send(req.json(&body)).await?;
        Ok(())
    }

    /// Avatar do Discord como `data:` URL (a CSP da UI só aceita imagens `self`/`data:`). Qualquer falha → `None`.
    pub async fn avatar_data_url(&self, me: &Me) -> Option<String> {
        let url = avatar_url(&me.discord_id, me.avatar.as_deref()?)?;
        fetch_image_data_url(&self.http, &url, AVATAR_PREFIX).await
    }
}

/// Cabeçalho HTTP `Date` → unix seconds.
pub fn parse_http_date(v: &str) -> Option<i64> {
    let t = httpdate::parse_http_date(v).ok()?;
    let secs = t.duration_since(std::time::UNIX_EPOCH).ok()?.as_secs();
    i64::try_from(secs).ok()
}

/// URL do avatar no CDN do Discord, só com id numérico e hash no formato do Discord.
pub fn avatar_url(discord_id: &str, hash: &str) -> Option<String> {
    let id_ok =
        (1..=20).contains(&discord_id.len()) && discord_id.bytes().all(|b| b.is_ascii_digit());
    let h = hash.strip_prefix("a_").unwrap_or(hash);
    let hash_ok = h.len() == 32 && h.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'));
    (id_ok && hash_ok).then(|| format!("{AVATAR_PREFIX}avatars/{discord_id}/{hash}.png?size=64"))
}

/// Baixa uma imagem PNG/JPEG/WebP/GIF pequena de `url` (que precisa começar com `allowed_prefix`).
pub async fn fetch_image_data_url(
    http: &reqwest::Client,
    url: &str,
    allowed_prefix: &str,
) -> Option<String> {
    use base64::Engine as _;
    if !url.starts_with(allowed_prefix) {
        return None;
    }
    let resp = http.get(url).send().await.ok()?;
    if !resp.status().is_success() {
        return None;
    }
    let mime = resp
        .headers()
        .get(reqwest::header::CONTENT_TYPE)?
        .to_str()
        .ok()?
        .split(';')
        .next()?
        .trim()
        .to_ascii_lowercase();
    if !matches!(
        mime.as_str(),
        "image/png" | "image/jpeg" | "image/webp" | "image/gif"
    ) {
        return None;
    }
    let body = read_limited(resp, MAX_AVATAR).await.ok()?;
    Some(format!(
        "data:{mime};base64,{}",
        base64::engine::general_purpose::STANDARD.encode(body)
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mapeia_erros_do_hub() {
        assert_eq!(
            map_hub_error(401, Some("bad_refresh")).code,
            "session_expired"
        );
        assert_eq!(
            map_hub_error(401, Some("unauthorized")).code,
            "session_expired"
        );
        assert_eq!(map_hub_error(401, None).code, "session_expired");
        assert_eq!(map_hub_error(400, Some("bad_code")).code, "login_failed");
        assert_eq!(
            map_hub_error(403, Some("banned_global")).code,
            "banned_global"
        );
        assert_eq!(map_hub_error(404, Some("not_found")).code, "not_found");
        assert_eq!(
            map_hub_error(429, Some("rate_limited")).code,
            "rate_limited"
        );
        assert_eq!(map_hub_error(503, Some("busy")).code, "hub_busy");
        assert_eq!(map_hub_error(404, Some("no_mod")).code, "mod_unavailable");
        let e = map_hub_error(500, Some("internal"));
        assert_eq!(e.code, "hub_error");
        assert!(e.message.contains("500"));
        assert_eq!(map_hub_error(418, None).code, "hub_error");
    }

    #[test]
    fn url_monta_com_prefixo_e_login_url_tem_os_parametros() {
        for base in ["https://hub.example.com", "https://hub.example.com/"] {
            let h = HubClient::new(base).unwrap();
            assert_eq!(h.url("v1/me").as_str(), "https://hub.example.com/v1/me");
        }
        let h = HubClient::new("https://x.example/base").unwrap();
        assert_eq!(h.url("v1/me").as_str(), "https://x.example/base/v1/me");
        let u = h.login_url("CH", "ST");
        assert_eq!(
            u.as_str(),
            "https://x.example/base/v1/auth/discord/login?client=launcher&code_challenge=CH&code_challenge_method=S256&state=ST"
        );
    }

    #[test]
    fn hub_recusa_endereco_invalido() {
        for bad in [
            "http://hub.example.com",
            "https://u:p@hub.example.com",
            "https://hub.example.com/?x=1",
            "https://hub.example.com/#f",
            "file:///C:/x",
            "nada",
        ] {
            assert!(HubClient::new(bad).is_err(), "{bad}");
        }
        assert!(!allowed_url(
            &Url::parse("http://127.0.0.1").unwrap(),
            false
        ));
        assert!(allowed_url(&Url::parse("http://127.0.0.1").unwrap(), true));
        assert!(allowed_url(&Url::parse("http://[::1]:1").unwrap(), true));
        assert!(!allowed_url(&Url::parse("http://10.0.0.1").unwrap(), true));
    }

    #[test]
    fn avatar_so_do_cdn_do_discord_com_id_e_hash_validos() {
        let hash = "0123456789abcdef0123456789abcdef";
        assert_eq!(
            avatar_url("123", hash).as_deref(),
            Some("https://cdn.discordapp.com/avatars/123/0123456789abcdef0123456789abcdef.png?size=64")
        );
        assert!(avatar_url("123", &format!("a_{hash}")).is_some());
        for (id, h) in [
            ("12a", hash),
            ("", hash),
            ("123456789012345678901", hash),
            ("1", "av"),
            ("1", "../../evil"),
            ("1", "0123456789ABCDEF0123456789ABCDEF"),
            ("1", "0123456789abcdef0123456789abcdef/x"),
        ] {
            assert_eq!(avatar_url(id, h), None, "{id} {h}");
        }
    }

    #[test]
    fn validade_relativa_usa_a_hora_do_hub() {
        let mut t = Tokens {
            access_token: "a".into(),
            access_expires_at: 10_900,
            refresh_token: None,
            refresh_expires_at: None,
            server_time: Some(10_000),
        };
        // Relógio local 2 h adiantado ou atrasado não muda a validade.
        assert_eq!(t.access_lifetime(10_000 + 7200), 900);
        assert_eq!(t.access_lifetime(10_000 - 7200), 900);
        t.server_time = None;
        assert_eq!(t.access_lifetime(10_100), 800);
        assert_eq!(t.access_lifetime(20_000), 0);
        assert_eq!(
            parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT"),
            Some(784_111_777)
        );
        assert_eq!(parse_http_date("ontem"), None);
    }

    #[test]
    fn tokens_nao_aparecem_no_debug() {
        let t = Tokens {
            access_token: "at_SEGREDO".into(),
            access_expires_at: 1,
            refresh_token: Some("rt_SEGREDO".into()),
            refresh_expires_at: Some(2),
            server_time: None,
        };
        assert!(!format!("{t:?}").contains("SEGREDO"));
    }
}
