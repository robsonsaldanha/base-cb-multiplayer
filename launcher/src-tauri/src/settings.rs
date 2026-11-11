//! `settings.json`: preferências locais do launcher.
//!
//! Gravação atômica (arquivo `.tmp` + rename). Arquivo que não é JSON vira `settings.json.bad` e o launcher segue
//! com os padrões; campo inválido isolado é descartado sozinho (o original fica copiado em `.bad`). Arquivo que existe
//! mas não pôde ser lido é devolvido como erro, para quem chama não sobrescrevê-lo.
//!
//! Quem pode mudar o quê:
//! - `hub_url`: fixo por build ([`build_hub_url`]); o valor no arquivo é ignorado.
//! - `game_dir`: só o Rust (seletor de pasta / localizador do jogo).
//! - o resto: a UI, via [`SettingsUpdate`].

use crate::error::{AppError, AppResult};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::path::{Path, PathBuf};

/// Placeholder: precisa ser configurado com o endereço real do Hub antes da primeira versão pública.
pub const DEFAULT_HUB_URL: &str = "https://hub.cbmp.invalid";
pub const DEFAULT_CACHE_LIMIT_MB: u64 = 10_240;
pub const DEFAULT_MAX_SERVER_CONTENT_MB: u64 = 2_048;

const CACHE_LIMIT_RANGE_MB: std::ops::RangeInclusive<u64> = 256..=1_048_576;
const MAX_SERVER_CONTENT_RANGE_MB: std::ops::RangeInclusive<u64> = 1..=65_536;
const MAX_SERVER_LIST: usize = 500;
const MAX_SERVER_ID_LEN: usize = 64;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Settings {
    /// Só leitura para a UI. Nunca lido do arquivo: vem de [`build_hub_url`].
    #[serde(skip_deserializing)]
    pub hub_url: String,
    pub favorites: Vec<String>,
    pub recent: Vec<String>,
    /// Só leitura para a UI; definido pelo Rust (seletor de pasta / localizador).
    pub game_dir: Option<PathBuf>,
    #[serde(rename = "cacheLimitMB")]
    pub cache_limit_mb: u64,
    #[serde(rename = "maxServerContentMB")]
    pub max_server_content_mb: u64,
    pub simulate_launch: bool,
}

/// Campos que a UI pode alterar (`set_settings`). Campo a mais (ex.: `gameDir`, `hubUrl`) é recusado.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct SettingsUpdate {
    pub favorites: Vec<String>,
    pub recent: Vec<String>,
    #[serde(rename = "cacheLimitMB")]
    pub cache_limit_mb: u64,
    #[serde(rename = "maxServerContentMB")]
    pub max_server_content_mb: u64,
    pub simulate_launch: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            hub_url: build_hub_url(),
            favorites: Vec::new(),
            recent: Vec::new(),
            game_dir: None,
            cache_limit_mb: DEFAULT_CACHE_LIMIT_MB,
            max_server_content_mb: DEFAULT_MAX_SERVER_CONTENT_MB,
            simulate_launch: true,
        }
    }
}

impl Settings {
    /// Valida com a regra do build atual: `http://` de loopback só é aceito em debug/teste local.
    pub fn validate(&self) -> AppResult<()> {
        self.validate_with(crate::profile::RELAXED_NETWORK)
    }

    /// `dev` libera `http://` para loopback (Hub local durante o desenvolvimento).
    pub fn validate_with(&self, dev: bool) -> AppResult<()> {
        validate_hub_url(&self.hub_url, dev)?;
        validate_server_list("favoritos", &self.favorites)?;
        validate_server_list("recentes", &self.recent)?;
        validate_game_dir(self.game_dir.as_deref())?;
        if !CACHE_LIMIT_RANGE_MB.contains(&self.cache_limit_mb) {
            return Err(AppError::invalid_settings(format!(
                "Limite do cache fora do intervalo ({} a {} MB).",
                CACHE_LIMIT_RANGE_MB.start(),
                CACHE_LIMIT_RANGE_MB.end()
            )));
        }
        if !MAX_SERVER_CONTENT_RANGE_MB.contains(&self.max_server_content_mb) {
            return Err(AppError::invalid_settings(format!(
                "Tamanho máximo do conteúdo de servidor fora do intervalo ({} a {} MB).",
                MAX_SERVER_CONTENT_RANGE_MB.start(),
                MAX_SERVER_CONTENT_RANGE_MB.end()
            )));
        }
        Ok(())
    }

    /// Aplica o que a UI pode mudar; `hub_url` e `game_dir` ficam como estão.
    pub fn with_update(&self, u: SettingsUpdate) -> Settings {
        Settings {
            hub_url: self.hub_url.clone(),
            game_dir: self.game_dir.clone(),
            favorites: u.favorites,
            recent: u.recent,
            cache_limit_mb: u.cache_limit_mb,
            max_server_content_mb: u.max_server_content_mb,
            simulate_launch: u.simulate_launch,
        }
    }

    /// Lê o arquivo.
    /// - ausente → padrões;
    /// - existe mas não pôde ser lido (permissão, arquivo em uso) → `Err`, sem tocar no arquivo;
    /// - não é objeto JSON → renomeado para `.bad`, padrões;
    /// - campo com tipo/valor inválido → só esse campo volta ao padrão (item inválido de lista é descartado) e o
    ///   original é copiado para `.bad`.
    pub fn load(path: &Path) -> AppResult<Settings> {
        let bytes = match std::fs::read(path) {
            Ok(b) => b,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Settings::default()),
            Err(e) => {
                return Err(AppError::new(
                    "settings_unreadable",
                    format!(
                        "Não foi possível ler {}: {e}. As configurações não serão gravadas até reiniciar o launcher com o arquivo legível.",
                        path.display()
                    ),
                ))
            }
        };
        // Só objeto JSON: o serde aceitaria também um array (campos por posição), que não é formato nosso.
        let Ok(Value::Object(map)) = serde_json::from_slice::<Value>(&bytes) else {
            // Guarda o original para diagnóstico; se nem renomear der, seguimos com os padrões assim mesmo.
            let _ = std::fs::rename(path, sibling(path, ".bad"));
            return Ok(Settings::default());
        };
        let (settings, lossy) = salvage(&map);
        if lossy {
            let _ = std::fs::copy(path, sibling(path, ".bad"));
        }
        Ok(settings)
    }

    /// Grava de forma atômica: escreve `<arquivo>.tmp`, sincroniza e renomeia por cima do atual.
    pub fn save(&self, path: &Path) -> AppResult<()> {
        self.validate()?;
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|e| AppError::io(parent, &e))?;
        }
        let json = serde_json::to_vec_pretty(self).expect("Settings sempre serializa");
        let tmp = sibling(path, ".tmp");
        let written = (|| {
            use std::io::Write;
            let mut f = std::fs::File::create(&tmp)?;
            f.write_all(&json)?;
            f.sync_all()
        })();
        if let Err(e) = written.and_then(|()| std::fs::rename(&tmp, path)) {
            let _ = std::fs::remove_file(&tmp);
            return Err(AppError::io(path, &e));
        }
        Ok(())
    }
}

/// Monta as configurações campo a campo; devolve `true` se algo do arquivo foi descartado.
fn salvage(map: &Map<String, Value>) -> (Settings, bool) {
    fn field<T: serde::de::DeserializeOwned>(
        map: &Map<String, Value>,
        key: &str,
        lossy: &mut bool,
    ) -> Option<T> {
        let v = map.get(key)?;
        let parsed = serde_json::from_value(v.clone()).ok();
        *lossy |= parsed.is_none();
        parsed
    }

    let mut lossy = false;
    let mut s = Settings::default();
    for (key, list) in [("favorites", &mut s.favorites), ("recent", &mut s.recent)] {
        if let Some(items) = field::<Vec<String>>(map, key, &mut lossy) {
            let before = items.len();
            let kept: Vec<String> = items
                .into_iter()
                .filter(|id| valid_server_id(id))
                .take(MAX_SERVER_LIST)
                .collect();
            lossy |= kept.len() != before;
            *list = kept;
        }
    }
    if let Some(dir) = field::<Option<PathBuf>>(map, "gameDir", &mut lossy) {
        if validate_game_dir(dir.as_deref()).is_ok() {
            s.game_dir = dir;
        } else {
            lossy = true;
        }
    }
    if let Some(n) = field::<u64>(map, "cacheLimitMB", &mut lossy) {
        if CACHE_LIMIT_RANGE_MB.contains(&n) {
            s.cache_limit_mb = n;
        } else {
            lossy = true;
        }
    }
    if let Some(n) = field::<u64>(map, "maxServerContentMB", &mut lossy) {
        if MAX_SERVER_CONTENT_RANGE_MB.contains(&n) {
            s.max_server_content_mb = n;
        } else {
            lossy = true;
        }
    }
    if let Some(b) = field::<bool>(map, "simulateLaunch", &mut lossy) {
        s.simulate_launch = b;
    }
    (s, lossy)
}

/// Hub padrão deste build: o oficial, o local no build de teste local ou o de teste online.
pub fn default_hub_url() -> &'static str {
    if crate::profile::LOCAL_TEST {
        crate::profile::LOCAL_TEST_HUB_URL
    } else if crate::profile::ONLINE_TEST {
        crate::profile::ONLINE_TEST_HUB_URL
    } else {
        DEFAULT_HUB_URL
    }
}

/// Endereço do Hub deste build: o padrão ([`default_hub_url`]), ou `CBMP_HUB_URL` em builds de debug (se válido).
pub fn build_hub_url() -> String {
    resolve_hub_url(
        default_hub_url(),
        std::env::var("CBMP_HUB_URL").ok(),
        cfg!(debug_assertions),
    )
}

/// `env` só vale com `dev` e se passar na validação do endereço; senão fica `default`.
pub fn resolve_hub_url(default: &str, env: Option<String>, dev: bool) -> String {
    match env {
        Some(e) if dev && validate_hub_url(&e, true).is_ok() => e,
        _ => default.to_string(),
    }
}

fn sibling(path: &Path, suffix: &str) -> PathBuf {
    let mut s = path.as_os_str().to_owned();
    s.push(suffix);
    PathBuf::from(s)
}

fn validate_hub_url(raw: &str, dev: bool) -> AppResult<()> {
    let invalid = || {
        AppError::invalid_settings(
            "Endereço do Hub inválido: use https:// (sem usuário, senha, ? ou #).",
        )
    };
    let url = url::Url::parse(raw).map_err(|_| invalid())?;
    if !url.username().is_empty()
        || url.password().is_some()
        || url.query().is_some()
        || url.fragment().is_some()
    {
        return Err(invalid());
    }
    let host = url.host().ok_or_else(invalid)?;
    match url.scheme() {
        "https" => Ok(()),
        "http" if dev && is_loopback(&host) => Ok(()),
        _ => Err(invalid()),
    }
}

fn is_loopback(host: &url::Host<&str>) -> bool {
    match host {
        url::Host::Domain(d) => d.eq_ignore_ascii_case("localhost"),
        url::Host::Ipv4(ip) => ip.is_loopback(),
        url::Host::Ipv6(ip) => ip.is_loopback(),
    }
}

pub(crate) fn validate_game_dir(dir: Option<&Path>) -> AppResult<()> {
    match dir {
        Some(d) if !d.is_absolute() => Err(AppError::invalid_settings(
            "A pasta do jogo precisa ser um caminho completo.",
        )),
        _ => Ok(()),
    }
}

fn valid_server_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= MAX_SERVER_ID_LEN && !id.chars().any(char::is_control)
}

fn validate_server_list(what: &str, ids: &[String]) -> AppResult<()> {
    if ids.len() > MAX_SERVER_LIST {
        return Err(AppError::invalid_settings(format!(
            "Lista de {what} grande demais (máximo {MAX_SERVER_LIST})."
        )));
    }
    if !ids.iter().all(|id| valid_server_id(id)) {
        return Err(AppError::invalid_settings(format!(
            "Identificador de servidor inválido em {what}."
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Settings {
        Settings {
            hub_url: build_hub_url(),
            favorites: vec!["srv-1".into(), "srv-2".into()],
            recent: vec!["srv-2".into()],
            game_dir: Some(std::env::temp_dir().join("Hogwarts Legacy")),
            cache_limit_mb: 4096,
            max_server_content_mb: 1024,
            simulate_launch: false,
        }
    }

    /// Regra de rede do build release deste sabor: o oficial é estrito; o de teste local aceita o Hub em
    /// `http://127.0.0.1` (o padrão dele).
    const RELEASE_RULES: bool = crate::profile::LOCAL_TEST;

    fn bad_path(p: &Path) -> PathBuf {
        sibling(p, ".bad")
    }

    fn load(p: &Path) -> Settings {
        Settings::load(p).unwrap()
    }

    #[test]
    fn padroes() {
        let s = Settings::default();
        assert_eq!(s.hub_url, default_hub_url());
        assert!(s.favorites.is_empty() && s.recent.is_empty());
        assert_eq!(s.game_dir, None);
        assert_eq!(s.cache_limit_mb, 10_240);
        assert_eq!(s.max_server_content_mb, 2_048);
        assert!(s.simulate_launch);
        s.validate_with(RELEASE_RULES).unwrap();
    }

    #[test]
    fn json_usa_nomes_do_contrato() {
        let v = serde_json::to_value(Settings::default()).unwrap();
        for k in [
            "hubUrl",
            "favorites",
            "recent",
            "gameDir",
            "cacheLimitMB",
            "maxServerContentMB",
            "simulateLaunch",
        ] {
            assert!(v.get(k).is_some(), "faltou {k} em {v}");
        }
    }

    #[test]
    fn ida_e_volta() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        let s = sample();
        s.save(&path).unwrap();
        assert_eq!(load(&path), s);
        // Sobrescreve e não deixa `.tmp` para trás.
        let mut s2 = s.clone();
        s2.favorites.push("srv-3".into());
        s2.save(&path).unwrap();
        assert_eq!(load(&path), s2);
        let names: Vec<_> = std::fs::read_dir(dir.path())
            .unwrap()
            .map(|e| e.unwrap().file_name())
            .collect();
        assert_eq!(names, vec![std::ffi::OsString::from("settings.json")]);
    }

    #[test]
    fn hub_url_do_arquivo_e_ignorado() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::write(
            &path,
            br#"{"hubUrl":"https://evil.example.com","favorites":["a"]}"#,
        )
        .unwrap();
        let s = load(&path);
        assert_eq!(s.hub_url, build_hub_url());
        assert_eq!(s.favorites, vec!["a".to_string()]);
        assert!(!bad_path(&path).exists(), "hubUrl não é erro, só ignorado");
    }

    #[test]
    fn save_cria_a_pasta() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("a").join("b").join("settings.json");
        Settings::default().save(&path).unwrap();
        assert!(path.is_file());
    }

    #[test]
    fn save_recusa_invalido() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        let s = Settings {
            cache_limit_mb: 0,
            ..Settings::default()
        };
        assert_eq!(s.save(&path).unwrap_err().code, "invalid_settings");
        assert!(!path.exists());
    }

    #[test]
    fn arquivo_ausente_da_padroes_sem_bad() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        assert_eq!(load(&path), Settings::default());
        assert!(!bad_path(&path).exists());
    }

    #[test]
    fn arquivo_ilegivel_e_erro_e_fica_intacto() {
        // Uma pasta no lugar do arquivo: `read` falha com erro diferente de NotFound.
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::create_dir(&path).unwrap();
        assert_eq!(
            Settings::load(&path).unwrap_err().code,
            "settings_unreadable"
        );
        assert!(path.is_dir());
        assert!(!bad_path(&path).exists());
    }

    #[test]
    fn arquivo_corrompido_vira_bad_e_padroes() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::write(&path, b"{ isto nao e json").unwrap();
        assert_eq!(load(&path), Settings::default());
        assert!(!path.exists());
        assert_eq!(
            std::fs::read(bad_path(&path)).unwrap(),
            b"{ isto nao e json"
        );
    }

    #[test]
    fn bad_antigo_e_substituido() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::write(bad_path(&path), b"velho").unwrap();
        std::fs::write(&path, b"[]").unwrap();
        assert_eq!(load(&path), Settings::default());
        assert_eq!(std::fs::read(bad_path(&path)).unwrap(), b"[]");
    }

    #[test]
    fn campo_invalido_so_perde_o_proprio_campo() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        let game = std::env::temp_dir().join("Jogo");
        let original = serde_json::json!({
            "favorites": ["ok", "", "x".repeat(65), "ok2"],
            "recent": "não é lista",
            "gameDir": game,
            "cacheLimitMB": 0,
            "maxServerContentMB": 512,
            "simulateLaunch": false
        })
        .to_string();
        std::fs::write(&path, &original).unwrap();
        let s = load(&path);
        assert_eq!(s.favorites, vec!["ok".to_string(), "ok2".to_string()]);
        assert!(s.recent.is_empty());
        assert_eq!(s.game_dir, Some(game));
        assert_eq!(s.cache_limit_mb, DEFAULT_CACHE_LIMIT_MB);
        assert_eq!(s.max_server_content_mb, 512);
        assert!(!s.simulate_launch);
        s.validate().unwrap();
        // Original preservado: o arquivo continua no lugar e uma cópia vai para `.bad`.
        assert!(path.exists());
        assert_eq!(std::fs::read_to_string(bad_path(&path)).unwrap(), original);
    }

    #[test]
    fn pasta_do_jogo_relativa_no_arquivo_e_descartada() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::write(&path, br#"{"gameDir":"jogo","favorites":["a"]}"#).unwrap();
        let s = load(&path);
        assert_eq!(s.game_dir, None);
        assert_eq!(s.favorites, vec!["a".to_string()]);
        assert!(bad_path(&path).exists());
    }

    #[test]
    fn campos_ausentes_recebem_padrao() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::write(&path, br#"{"favorites":["x"],"campoNovo":1}"#).unwrap();
        let s = load(&path);
        assert_eq!(s.favorites, vec!["x".to_string()]);
        assert_eq!(s.cache_limit_mb, DEFAULT_CACHE_LIMIT_MB);
        assert_eq!(s.hub_url, default_hub_url());
        assert!(!bad_path(&path).exists());
    }

    #[test]
    fn update_nao_aceita_game_dir_nem_hub_url() {
        let base = serde_json::json!({
            "favorites": [], "recent": [], "cacheLimitMB": 1024,
            "maxServerContentMB": 1024, "simulateLaunch": true
        });
        serde_json::from_value::<SettingsUpdate>(base.clone()).unwrap();
        for (k, v) in [
            ("gameDir", serde_json::json!("C:\\Users\\x\\Startup")),
            ("hubUrl", serde_json::json!("https://evil.example.com")),
        ] {
            let mut with = base.clone();
            with[k] = v;
            assert!(
                serde_json::from_value::<SettingsUpdate>(with).is_err(),
                "{k} aceito"
            );
        }
    }

    #[test]
    fn with_update_preserva_hub_e_pasta_do_jogo() {
        let cur = sample();
        let u = SettingsUpdate {
            favorites: vec!["z".into()],
            recent: vec![],
            cache_limit_mb: 300,
            max_server_content_mb: 10,
            simulate_launch: true,
        };
        let new = cur.with_update(u);
        assert_eq!(new.hub_url, cur.hub_url);
        assert_eq!(new.game_dir, cur.game_dir);
        assert_eq!(new.favorites, vec!["z".to_string()]);
        assert_eq!(
            (
                new.cache_limit_mb,
                new.max_server_content_mb,
                new.simulate_launch
            ),
            (300, 10, true)
        );
    }

    fn with_hub(url: &str) -> Settings {
        Settings {
            hub_url: url.into(),
            ..Settings::default()
        }
    }

    #[test]
    fn hub_url_https_aceito() {
        for u in [
            "https://hub.example.com",
            "https://hub.example.com/",
            "https://hub.example.com:8443/base",
        ] {
            with_hub(u)
                .validate_with(false)
                .unwrap_or_else(|e| panic!("{u}: {e}"));
        }
    }

    #[test]
    fn hub_url_http_loopback_so_em_dev() {
        for u in [
            "http://127.0.0.1:8080",
            "http://localhost:3000",
            "http://[::1]:3000",
        ] {
            with_hub(u)
                .validate_with(true)
                .unwrap_or_else(|e| panic!("{u}: {e}"));
            assert_eq!(
                with_hub(u).validate_with(false).unwrap_err().code,
                "invalid_settings",
                "{u}"
            );
        }
    }

    #[test]
    fn hub_url_recusados() {
        for u in [
            "",
            "hub.example.com",
            "http://hub.example.com",
            "http://127.0.0.2.example.com",
            "ftp://hub.example.com",
            "file:///C:/hub",
            "https://user:senha@hub.example.com",
            "https://hub.example.com/?x=1",
            "https://hub.example.com/#frag",
            "https://",
        ] {
            let err = with_hub(u).validate_with(true).unwrap_err();
            assert_eq!(err.code, "invalid_settings", "{u}");
        }
    }

    #[test]
    fn limites_numericos() {
        let s = |c, m| Settings {
            cache_limit_mb: c,
            max_server_content_mb: m,
            ..Settings::default()
        };
        s(256, 1).validate_with(RELEASE_RULES).unwrap();
        s(1_048_576, 65_536).validate_with(RELEASE_RULES).unwrap();
        assert!(s(255, 2048).validate_with(RELEASE_RULES).is_err());
        assert!(s(1_048_577, 2048).validate_with(RELEASE_RULES).is_err());
        assert!(s(10_240, 0).validate_with(RELEASE_RULES).is_err());
        assert!(s(10_240, 65_537).validate_with(RELEASE_RULES).is_err());
    }

    #[test]
    fn listas_de_servidores() {
        let s = |f: Vec<String>| Settings {
            favorites: f.clone(),
            recent: f,
            ..Settings::default()
        };
        s(vec!["a".into(), "x".repeat(64)])
            .validate_with(RELEASE_RULES)
            .unwrap();
        assert!(s(vec!["".into()]).validate_with(RELEASE_RULES).is_err());
        assert!(s(vec!["x".repeat(65)])
            .validate_with(RELEASE_RULES)
            .is_err());
        assert!(s(vec!["a\nb".into()]).validate_with(RELEASE_RULES).is_err());
        assert!(s((0..501).map(|i| i.to_string()).collect())
            .validate_with(RELEASE_RULES)
            .is_err());
        let only_recent = Settings {
            recent: vec!["".into()],
            ..Settings::default()
        };
        assert!(only_recent.validate_with(RELEASE_RULES).is_err());
    }

    #[test]
    fn pasta_do_jogo_precisa_ser_absoluta() {
        let rel = Settings {
            game_dir: Some(PathBuf::from("jogo")),
            ..Settings::default()
        };
        assert_eq!(
            rel.validate_with(RELEASE_RULES).unwrap_err().code,
            "invalid_settings"
        );
        let abs = Settings {
            game_dir: Some(std::env::temp_dir()),
            ..Settings::default()
        };
        abs.validate_with(RELEASE_RULES).unwrap();
    }

    #[test]
    fn override_do_hub_so_em_dev_e_so_valido() {
        let default = "https://hub.example.com";
        let env = || Some("http://127.0.0.1:8080".to_string());
        assert_eq!(
            resolve_hub_url(default, env(), true),
            "http://127.0.0.1:8080"
        );
        assert_eq!(resolve_hub_url(default, env(), false), default);
        assert_eq!(resolve_hub_url(default, None, true), default);
        assert_eq!(resolve_hub_url(default, Some("".into()), true), default);
        assert_eq!(
            resolve_hub_url(default, Some("http://evil.example.com".into()), true),
            default
        );
    }
}
