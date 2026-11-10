//! Login com Discord pelo Hub: PKCE S256, deep link `cbmp://auth`, troca/refresh e cofre.
//!
//! - `begin_login` gera `codeVerifier` (64 chars) e `state` (32 bytes, base64url), guarda o pendente por 10 min e
//!   devolve a URL do Hub para o navegador do sistema (aberta por [`open_in_browser`], nunca pela UI).
//! - `on_deep_link` só aceita `cbmp://auth?code=…&state=…` ou `cbmp://auth?error=…&state=…`, com o `state` do
//!   pendente (uso único). Link ignorado não gasta o pendente.
//! - Refresh token só no cofre ([`TokenVault`], Credential Manager no Windows); access token só em memória.
//! - Logs: só códigos de erro; nunca tokens, códigos OAuth, verifier ou state.

use crate::error::{AppError, AppResult};
use crate::hub::{HubClient, Me, Tokens};
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine as _;
use sha2::{Digest, Sha256};
use std::sync::{Arc, Mutex};
use url::Url;

/// Serviço do Credential Manager.
pub const VAULT_SERVICE: &str = "CBMultiplayer";
/// Conta (usuário) da credencial que guarda o refresh token.
pub const VAULT_USER: &str = "refresh_token";
/// Validade do login pendente (state + verifier).
pub const PENDING_TTL_SECS: u64 = 600;
/// Renova o access token quando faltar isto ou menos para vencer.
pub const REFRESH_MARGIN_SECS: i64 = 60;

// ----- cofre -----

/// Onde fica o refresh token. Só ele é persistido.
pub trait TokenVault: Send + Sync {
    fn get(&self) -> AppResult<Option<String>>;
    fn set(&self, refresh_token: &str) -> AppResult<()>;
    fn clear(&self) -> AppResult<()>;
}

/// Cofre em memória (testes).
#[derive(Default)]
pub struct MemoryVault(Mutex<Option<String>>);

impl TokenVault for MemoryVault {
    fn get(&self) -> AppResult<Option<String>> {
        Ok(self.0.lock().unwrap_or_else(|e| e.into_inner()).clone())
    }
    fn set(&self, refresh_token: &str) -> AppResult<()> {
        *self.0.lock().unwrap_or_else(|e| e.into_inner()) = Some(refresh_token.to_string());
        Ok(())
    }
    fn clear(&self) -> AppResult<()> {
        *self.0.lock().unwrap_or_else(|e| e.into_inner()) = None;
        Ok(())
    }
}

/// Windows Credential Manager via `keyring` (serviço [`VAULT_SERVICE`]).
pub struct KeyringVault {
    service: String,
    user: String,
}

impl Default for KeyringVault {
    fn default() -> Self {
        Self::with_names(VAULT_SERVICE, VAULT_USER)
    }
}

fn vault_error() -> AppError {
    AppError::new(
        "vault_error",
        "Não foi possível usar o Gerenciador de Credenciais do Windows.",
    )
}

impl KeyringVault {
    pub fn with_names(service: &str, user: &str) -> Self {
        Self {
            service: service.into(),
            user: user.into(),
        }
    }

    fn entry(&self) -> AppResult<keyring::Entry> {
        keyring::Entry::new(&self.service, &self.user).map_err(|_| vault_error())
    }
}

impl TokenVault for KeyringVault {
    fn get(&self) -> AppResult<Option<String>> {
        match self.entry()?.get_password() {
            Ok(v) => Ok(Some(v)),
            Err(keyring::Error::NoEntry) => Ok(None),
            Err(_) => Err(vault_error()),
        }
    }
    fn set(&self, refresh_token: &str) -> AppResult<()> {
        self.entry()?
            .set_password(refresh_token)
            .map_err(|_| vault_error())
    }
    fn clear(&self) -> AppResult<()> {
        match self.entry()?.delete_credential() {
            Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
            Err(_) => Err(vault_error()),
        }
    }
}

// ----- PKCE e state -----

pub struct Pkce {
    pub verifier: String,
    pub challenge: String,
}

fn random_b64url(bytes: usize) -> String {
    let mut buf = vec![0u8; bytes];
    getrandom::fill(&mut buf).expect("gerador aleatório do sistema");
    URL_SAFE_NO_PAD.encode(buf)
}

/// `code_challenge` S256 = base64url(sha256(verifier)), sem padding.
pub fn challenge_for(verifier: &str) -> String {
    URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes()))
}

/// Verifier de 64 chars (48 bytes aleatórios em base64url, dentro de `[A-Za-z0-9._~-]`).
pub fn pkce() -> Pkce {
    let verifier = random_b64url(48);
    let challenge = challenge_for(&verifier);
    Pkce {
        verifier,
        challenge,
    }
}

/// `state` do launcher: 32 bytes aleatórios em base64url (43 chars).
pub fn new_state() -> String {
    random_b64url(32)
}

fn ct_eq(a: &str, b: &str) -> bool {
    a.len() == b.len()
        && a.bytes()
            .zip(b.bytes())
            .fold(0u8, |acc, (x, y)| acc | (x ^ y))
            == 0
}

// ----- deep link -----

#[derive(Debug, PartialEq, Eq)]
pub enum DeepLink {
    Code { code: String, state: String },
    Error { error: String, state: String },
}

fn invalid_link() -> AppError {
    AppError::new("invalid_deep_link", "Link de login inválido; ignorado.")
}

fn state_invalid() -> AppError {
    AppError::new(
        "login_state_invalid",
        "Este link de login não é deste launcher ou já venceu; ignorado.",
    )
}

fn login_failed() -> AppError {
    AppError::new(
        "login_failed",
        "Não foi possível concluir o login. Tente de novo.",
    )
}

fn b64url_token(s: &str, min: usize, max: usize) -> bool {
    (min..=max).contains(&s.len())
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

/// Só `cbmp://auth` (barra final opcional), sem porta/usuário/fragmento, com `state` e exatamente um de
/// `code`/`error`, sem parâmetros repetidos ou desconhecidos.
pub fn parse_deep_link(raw: &str) -> AppResult<DeepLink> {
    let u = Url::parse(raw).map_err(|_| invalid_link())?;
    if u.scheme() != "cbmp"
        || u.host_str() != Some("auth")
        || !matches!(u.path(), "" | "/")
        || u.port().is_some()
        || !u.username().is_empty()
        || u.password().is_some()
        || u.fragment().is_some()
    {
        return Err(invalid_link());
    }
    let (mut code, mut error, mut state) = (None, None, None);
    for (k, v) in u.query_pairs() {
        let slot = match k.as_ref() {
            "code" => &mut code,
            "error" => &mut error,
            "state" => &mut state,
            _ => return Err(invalid_link()),
        };
        if slot.replace(v.into_owned()).is_some() {
            return Err(invalid_link());
        }
    }
    let state = state
        .filter(|s| b64url_token(s, 43, 43))
        .ok_or_else(invalid_link)?;
    match (code, error) {
        (Some(code), None) if b64url_token(&code, 1, 256) => Ok(DeepLink::Code { code, state }),
        (None, Some(error))
            if (1..=64).contains(&error.len())
                && error.bytes().all(|b| b.is_ascii_lowercase() || b == b'_') =>
        {
            Ok(DeepLink::Error { error, state })
        }
        _ => Err(invalid_link()),
    }
}

// ----- sessão -----

/// Relógio monotônico em segundos (injetável nos testes). Mede o TTL do login pendente e a validade do access
/// token; nunca é comparado com horas absolutas do Hub.
pub type Clock = Arc<dyn Fn() -> i64 + Send + Sync>;

/// Segundos desde a criação, por `Instant` (não muda se o usuário acertar o relógio do Windows).
pub fn monotonic_clock() -> Clock {
    let base = std::time::Instant::now();
    Arc::new(move || base.elapsed().as_secs() as i64)
}

/// Hora do sistema em unix seconds: só para o caso raro de o Hub não mandar `Date`.
fn wall_now() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

struct Pending {
    verifier: String,
    state: String,
    created_at: i64,
}

struct Session {
    access: String,
    /// Em segundos do [`Clock`] monotônico: recebimento + validade informada pelo Hub.
    expires_at: i64,
}

pub struct Auth {
    hub: HubClient,
    vault: Arc<dyn TokenVault>,
    clock: Clock,
    pending: Mutex<Option<Pending>>,
    /// Lock assíncrono: serializa refresh (um só pedido ao Hub mesmo com chamadas simultâneas).
    session: tokio::sync::Mutex<Option<Session>>,
}

fn not_logged_in() -> AppError {
    AppError::new("not_logged_in", "Entre com o Discord para continuar.")
}

fn session_expired() -> AppError {
    AppError::new("session_expired", "Sua sessão expirou. Entre de novo.")
}

/// Refresh recusado pelo Hub (token inválido/revogado): só nesse caso a sessão é apagada.
fn refresh_refused(e: &AppError) -> bool {
    matches!(e.code, "session_expired" | "hub_rejected")
}

impl Auth {
    pub fn new(hub: HubClient, vault: Arc<dyn TokenVault>) -> Self {
        Self::with_clock(hub, vault, monotonic_clock())
    }

    pub fn with_clock(hub: HubClient, vault: Arc<dyn TokenVault>, clock: Clock) -> Self {
        Self {
            hub,
            vault,
            clock,
            pending: Mutex::new(None),
            session: tokio::sync::Mutex::new(None),
        }
    }

    pub fn hub(&self) -> &HubClient {
        &self.hub
    }

    fn pending(&self) -> std::sync::MutexGuard<'_, Option<Pending>> {
        self.pending.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Há refresh token guardado (sessão de uma abertura anterior).
    pub fn has_stored_session(&self) -> bool {
        matches!(self.vault.get(), Ok(Some(_)))
    }

    fn session_from(&self, t: &Tokens) -> Session {
        Session {
            access: t.access_token.clone(),
            expires_at: (self.clock)() + t.access_lifetime(wall_now()),
        }
    }

    /// Novo login: substitui o pendente anterior e devolve a URL do Hub para o navegador.
    pub fn begin_login(&self) -> Url {
        let p = pkce();
        let state = new_state();
        let url = self.hub.login_url(&p.challenge, &state);
        *self.pending() = Some(Pending {
            verifier: p.verifier,
            state,
            created_at: (self.clock)(),
        });
        log::info!("login iniciado no navegador");
        url
    }

    /// Trata `cbmp://auth?...`. Erro com código: `invalid_deep_link`/`login_state_invalid` (ignorado, nada muda),
    /// `login_denied`, `login_failed` ou erro do Hub/cofre.
    pub async fn on_deep_link(&self, raw: &str) -> AppResult<()> {
        let r = self.handle_deep_link(raw).await;
        match &r {
            Ok(()) => log::info!("login concluído"),
            Err(e) if matches!(e.code, "invalid_deep_link" | "login_state_invalid") => {
                log::warn!("deep link ignorado: {}", e.code)
            }
            Err(e) => log::warn!("login falhou: {}", e.code),
        }
        r
    }

    async fn handle_deep_link(&self, raw: &str) -> AppResult<()> {
        let link = parse_deep_link(raw)?;
        let state = match &link {
            DeepLink::Code { state, .. } | DeepLink::Error { state, .. } => state,
        };
        let verifier = {
            let mut pending = self.pending();
            let Some(p) = pending.as_ref() else {
                return Err(state_invalid());
            };
            if !ct_eq(&p.state, state) {
                return Err(state_invalid());
            }
            let p = pending.take().expect("pendente presente");
            if (self.clock)() - p.created_at > PENDING_TTL_SECS as i64 {
                return Err(state_invalid());
            }
            p.verifier
        };
        let code = match link {
            DeepLink::Error { error, .. } if error == "access_denied" => {
                return Err(AppError::new("login_denied", "Login cancelado no Discord."))
            }
            DeepLink::Error { .. } => return Err(login_failed()),
            DeepLink::Code { code, .. } => code,
        };
        let mut session = self.session.lock().await;
        let t = self.hub.exchange(&code, &verifier).await?;
        let rt = t.refresh_token.clone().ok_or_else(login_failed)?;
        let old_rt = self.vault.get().ok().flatten();
        let old_access = session.as_ref().map(|s| s.access.clone());
        self.vault.set(&rt)?;
        *session = Some(self.session_from(&t));
        // Login por cima de uma sessão viva: encerra a antiga no Hub (melhor esforço).
        if old_rt.is_some() || old_access.is_some() {
            if let Err(e) = self
                .hub
                .logout(old_access.as_deref(), old_rt.as_deref())
                .await
            {
                log::warn!("logout da sessão anterior falhou: {}", e.code);
            }
        }
        Ok(())
    }

    /// Access token válido por pelo menos mais 60 s, renovando pelo cofre se preciso.
    /// - Refresh recusado → cofre limpo e `session_expired`; sem refresh token → `not_logged_in`.
    /// - Hub fora/erro → cofre e sessão ficam; se o token atual ainda não venceu, ele é devolvido.
    pub async fn access_token(&self) -> AppResult<String> {
        let mut session = self.session.lock().await;
        if let Some(s) = session.as_ref() {
            if s.expires_at - (self.clock)() > REFRESH_MARGIN_SECS {
                return Ok(s.access.clone());
            }
        }
        match self.refresh_locked(&mut session).await {
            Ok(at) => Ok(at),
            Err(e) => match session.as_ref() {
                Some(s) if !refresh_refused(&e) && s.expires_at > (self.clock)() => {
                    Ok(s.access.clone())
                }
                _ => Err(e),
            },
        }
    }

    /// Refresh com o lock da sessão na mão. Só apaga sessão e cofre se o Hub recusar o refresh.
    async fn refresh_locked(&self, session: &mut Option<Session>) -> AppResult<String> {
        let Some(rt) = self.vault.get()? else {
            *session = None;
            return Err(not_logged_in());
        };
        match self.hub.refresh(&rt).await {
            Ok(t) => {
                let Some(new_rt) = t.refresh_token.as_deref() else {
                    *session = None;
                    let _ = self.vault.clear();
                    log::warn!("refresh sem refresh token novo; sessão encerrada");
                    return Err(session_expired());
                };
                // O Hub já girou: mesmo se o cofre falhar, a sessão nova vale nesta execução.
                let saved = self.vault.set(new_rt);
                *session = Some(self.session_from(&t));
                match saved {
                    Ok(()) => log::info!("sessão renovada"),
                    Err(e) => log::warn!("sessão renovada, mas o cofre falhou: {}", e.code),
                }
                Ok(t.access_token)
            }
            Err(e) if refresh_refused(&e) => {
                *session = None;
                let _ = self.vault.clear();
                log::warn!("refresh recusado pelo Hub; sessão encerrada");
                Err(session_expired())
            }
            Err(e) => {
                log::warn!("refresh falhou: {}", e.code);
                Err(e)
            }
        }
    }

    /// O Hub recusou `used` (401). Se outro chamador já girou o token, devolve o atual; senão faz um refresh
    /// (serializado pelo lock: quem chegar depois vê o token novo e não renova de novo).
    async fn after_unauthorized(&self, used: &str) -> AppResult<String> {
        let mut session = self.session.lock().await;
        if let Some(s) = session.as_ref() {
            if !ct_eq(&s.access, used) {
                return Ok(s.access.clone());
            }
        }
        self.refresh_locked(&mut session).await
    }

    /// Chama o Hub com a sessão; num 401, tenta uma vez de novo com o token atual (ou renovado). A sessão só cai
    /// se o refresh for recusado; erro de rede nunca a apaga.
    async fn with_session<T, F, Fut>(&self, f: F) -> AppResult<T>
    where
        F: Fn(String) -> Fut,
        Fut: std::future::Future<Output = AppResult<T>>,
    {
        let access = self.access_token().await?;
        match f(access.clone()).await {
            Err(e) if e.code == "session_expired" => {
                let fresh = self.after_unauthorized(&access).await?;
                f(fresh).await
            }
            r => r,
        }
    }

    /// `GET /v1/me` com a sessão atual.
    pub async fn me(&self) -> AppResult<Me> {
        self.with_session(|at| async move { self.hub.me(&at).await })
            .await
    }

    /// Ticket de entrada para `server_id` com a sessão atual.
    pub async fn ticket(&self, server_id: &str) -> AppResult<String> {
        self.with_session(|at| async move { self.hub.ticket(server_id, &at).await })
            .await
    }

    /// Encerra no Hub (melhor esforço) e limpa cofre, sessão e login pendente.
    pub async fn logout(&self) -> AppResult<()> {
        let mut session = self.session.lock().await;
        let access = session.take().map(|s| s.access);
        self.pending().take();
        let rt = self.vault.get().ok().flatten();
        if access.is_some() || rt.is_some() {
            if let Err(e) = self.hub.logout(access.as_deref(), rt.as_deref()).await {
                log::warn!("logout no Hub falhou: {}", e.code);
            }
        }
        self.vault.clear()?;
        log::info!("logout concluído");
        Ok(())
    }
}

// ----- navegador do sistema -----

/// Só `https://` (ou `http://` de loopback em debug) vai para o navegador.
pub fn browser_url_allowed(url: &Url, dev: bool) -> bool {
    crate::hub::allowed_url(url, dev)
}

/// Abre `url` no navegador padrão (ShellExecuteW). Não exposto à UI como URL livre: só o Rust chama.
pub fn open_in_browser(url: &Url) -> AppResult<()> {
    if !browser_url_allowed(url, cfg!(debug_assertions)) {
        return Err(AppError::new(
            "invalid_url",
            "Endereço não permitido para abrir no navegador.",
        ));
    }
    open_url_os(url.as_str())
}

#[cfg(windows)]
fn open_url_os(url: &str) -> AppResult<()> {
    use windows_sys::Win32::UI::Shell::ShellExecuteW;
    use windows_sys::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;
    let wide = |s: &str| s.encode_utf16().chain(Some(0)).collect::<Vec<u16>>();
    let (verb, target) = (wide("open"), wide(url));
    // SAFETY: strings UTF-16 terminadas em zero que vivem até o fim da chamada; demais ponteiros nulos.
    let r = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            verb.as_ptr(),
            target.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            SW_SHOWNORMAL,
        )
    };
    // ShellExecuteW: valor > 32 = sucesso.
    if r as isize > 32 {
        Ok(())
    } else {
        Err(AppError::new(
            "browser_failed",
            "Não foi possível abrir o navegador.",
        ))
    }
}

#[cfg(not(windows))]
fn open_url_os(_url: &str) -> AppResult<()> {
    Err(AppError::new(
        "browser_failed",
        "Abrir o navegador só é suportado no Windows.",
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    const S: &str = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ";

    #[test]
    fn parse_aceita_code_e_error() {
        assert_eq!(
            parse_deep_link(&format!("cbmp://auth?code=abc_-1&state={S}")).unwrap(),
            DeepLink::Code {
                code: "abc_-1".into(),
                state: S.into()
            }
        );
        assert_eq!(
            parse_deep_link(&format!("cbmp://auth/?state={S}&error=access_denied")).unwrap(),
            DeepLink::Error {
                error: "access_denied".into(),
                state: S.into()
            }
        );
    }

    #[test]
    fn parse_recusa_o_resto() {
        for l in [
            format!("CBMP://AUTH?code=a&state={S}x"),
            format!("cbmp://auth:1?code=a&state={S}"),
            format!("cbmp://u@auth?code=a&state={S}"),
            format!("cbmp://auth?code=a&state={S}#f"),
            format!("cbmp://auth?code=a&state={}", &S[..42]),
            format!("cbmp://auth?code=&state={S}"),
            format!("cbmp://auth?error=Access-Denied&state={S}"),
            format!("cbmp://auth?state={S}"),
            format!("cbmp://auth?code={}&state={S}", "a".repeat(257)),
            "cbmp:auth".into(),
        ] {
            assert_eq!(
                parse_deep_link(&l).unwrap_err().code,
                "invalid_deep_link",
                "{l}"
            );
        }
    }

    #[test]
    fn navegador_so_https_ou_loopback_em_debug() {
        let ok = |s: &str, dev| browser_url_allowed(&Url::parse(s).unwrap(), dev);
        assert!(ok(
            "https://hub.example.com/v1/auth/discord/login?x=1",
            false
        ));
        assert!(!ok("http://hub.example.com/", true));
        assert!(!ok("http://127.0.0.1:8787/", false));
        assert!(ok("http://127.0.0.1:8787/", true));
        assert!(!ok("file:///C:/Windows/System32/calc.exe", true));
        assert!(!ok("cbmp://auth", true));
        assert!(!ok("javascript:alert(1)", true));
        assert_eq!(
            open_in_browser(&Url::parse("file:///C:/x").unwrap())
                .unwrap_err()
                .code,
            "invalid_url"
        );
    }

    #[test]
    fn ct_eq_compara() {
        assert!(ct_eq("abc", "abc"));
        assert!(!ct_eq("abc", "abd"));
        assert!(!ct_eq("abc", "ab"));
    }
}
