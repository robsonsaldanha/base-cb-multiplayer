//! `settings.json`: preferências locais do launcher.
//!
//! Gravação atômica (arquivo `.tmp` + rename). Arquivo ilegível ou inválido não derruba o launcher: vira
//! `settings.json.bad` e o launcher segue com os padrões.

use crate::error::{AppError, AppResult};
use serde::{Deserialize, Serialize};
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
    pub hub_url: String,
    pub favorites: Vec<String>,
    pub recent: Vec<String>,
    pub game_dir: Option<PathBuf>,
    #[serde(rename = "cacheLimitMB")]
    pub cache_limit_mb: u64,
    #[serde(rename = "maxServerContentMB")]
    pub max_server_content_mb: u64,
    pub simulate_launch: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            hub_url: DEFAULT_HUB_URL.into(),
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
    /// Valida com a regra do build atual: `http://` de loopback só é aceito em debug.
    pub fn validate(&self) -> AppResult<()> {
        self.validate_with(cfg!(debug_assertions))
    }

    /// `dev` libera `http://` para loopback (Hub local durante o desenvolvimento).
    pub fn validate_with(&self, dev: bool) -> AppResult<()> {
        validate_hub_url(&self.hub_url, dev)?;
        validate_server_list("favoritos", &self.favorites)?;
        validate_server_list("recentes", &self.recent)?;
        if let Some(dir) = &self.game_dir {
            if !dir.is_absolute() {
                return Err(AppError::invalid_settings(
                    "A pasta do jogo precisa ser um caminho completo.",
                ));
            }
        }
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

    /// Lê o arquivo. Ausente → padrões. Corrompido ou inválido → renomeia para `.bad` e devolve padrões.
    pub fn load(path: &Path) -> Settings {
        let bytes = match std::fs::read(path) {
            Ok(b) => b,
            // Ausente é o normal na primeira execução; erro de leitura não deve impedir o launcher de abrir.
            Err(_) => return Settings::default(),
        };
        // Só objeto JSON: o serde aceitaria também um array (campos por posição), que não é formato nosso.
        let parsed = match serde_json::from_slice::<serde_json::Value>(&bytes) {
            Ok(v @ serde_json::Value::Object(_)) => serde_json::from_value::<Settings>(v).ok(),
            _ => None,
        };
        match parsed {
            Some(s) if s.validate().is_ok() => s,
            _ => {
                // Guarda o original para diagnóstico; se nem renomear der, seguimos com os padrões assim mesmo.
                let _ = std::fs::rename(path, sibling(path, ".bad"));
                Settings::default()
            }
        }
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

    /// Endereço do Hub efetivo: `CBMP_HUB_URL` sobrepõe o salvo, mas só em builds de debug.
    pub fn effective_hub_url(&self) -> String {
        resolve_hub_url(
            &self.hub_url,
            std::env::var("CBMP_HUB_URL").ok(),
            cfg!(debug_assertions),
        )
    }
}

/// `env` só vale com `dev` e se passar na mesma validação do endereço salvo; senão fica o salvo.
pub fn resolve_hub_url(stored: &str, env: Option<String>, dev: bool) -> String {
    match env {
        Some(e) if dev && validate_hub_url(&e, true).is_ok() => e,
        _ => stored.to_string(),
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

fn validate_server_list(what: &str, ids: &[String]) -> AppResult<()> {
    if ids.len() > MAX_SERVER_LIST {
        return Err(AppError::invalid_settings(format!(
            "Lista de {what} grande demais (máximo {MAX_SERVER_LIST})."
        )));
    }
    let ok = |id: &String| {
        !id.is_empty() && id.len() <= MAX_SERVER_ID_LEN && !id.chars().any(char::is_control)
    };
    if !ids.iter().all(ok) {
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
            hub_url: "https://hub.example.com".into(),
            favorites: vec!["srv-1".into(), "srv-2".into()],
            recent: vec!["srv-2".into()],
            game_dir: Some(std::env::temp_dir().join("Hogwarts Legacy")),
            cache_limit_mb: 4096,
            max_server_content_mb: 1024,
            simulate_launch: false,
        }
    }

    fn bad_path(p: &Path) -> PathBuf {
        let mut s = p.as_os_str().to_owned();
        s.push(".bad");
        PathBuf::from(s)
    }

    #[test]
    fn padroes() {
        let s = Settings::default();
        assert_eq!(s.hub_url, DEFAULT_HUB_URL);
        assert!(s.favorites.is_empty() && s.recent.is_empty());
        assert_eq!(s.game_dir, None);
        assert_eq!(s.cache_limit_mb, 10_240);
        assert_eq!(s.max_server_content_mb, 2_048);
        assert!(s.simulate_launch);
        s.validate_with(false).unwrap();
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
        assert_eq!(Settings::load(&path), s);
        // Sobrescreve e não deixa `.tmp` para trás.
        let mut s2 = s.clone();
        s2.favorites.push("srv-3".into());
        s2.save(&path).unwrap();
        assert_eq!(Settings::load(&path), s2);
        let names: Vec<_> = std::fs::read_dir(dir.path())
            .unwrap()
            .map(|e| e.unwrap().file_name())
            .collect();
        assert_eq!(names, vec![std::ffi::OsString::from("settings.json")]);
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
        assert_eq!(Settings::load(&path), Settings::default());
        assert!(!bad_path(&path).exists());
    }

    #[test]
    fn arquivo_corrompido_vira_bad_e_padroes() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::write(&path, b"{ isto nao e json").unwrap();
        assert_eq!(Settings::load(&path), Settings::default());
        assert!(!path.exists());
        assert_eq!(
            std::fs::read(bad_path(&path)).unwrap(),
            b"{ isto nao e json"
        );
    }

    #[test]
    fn arquivo_valido_mas_recusado_pela_validacao_vira_bad() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::write(&path, br#"{"hubUrl":"http://evil.example.com"}"#).unwrap();
        assert_eq!(Settings::load(&path), Settings::default());
        assert!(bad_path(&path).exists());
    }

    #[test]
    fn bad_antigo_e_substituido() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::write(bad_path(&path), b"velho").unwrap();
        std::fs::write(&path, b"[]").unwrap();
        assert_eq!(Settings::load(&path), Settings::default());
        assert_eq!(std::fs::read(bad_path(&path)).unwrap(), b"[]");
    }

    #[test]
    fn campos_ausentes_recebem_padrao() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        std::fs::write(&path, br#"{"favorites":["x"],"campoNovo":1}"#).unwrap();
        let s = Settings::load(&path);
        assert_eq!(s.favorites, vec!["x".to_string()]);
        assert_eq!(s.cache_limit_mb, DEFAULT_CACHE_LIMIT_MB);
        assert_eq!(s.hub_url, DEFAULT_HUB_URL);
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
        s(256, 1).validate_with(false).unwrap();
        s(1_048_576, 65_536).validate_with(false).unwrap();
        assert!(s(255, 2048).validate_with(false).is_err());
        assert!(s(1_048_577, 2048).validate_with(false).is_err());
        assert!(s(10_240, 0).validate_with(false).is_err());
        assert!(s(10_240, 65_537).validate_with(false).is_err());
    }

    #[test]
    fn listas_de_servidores() {
        let s = |f: Vec<String>| Settings {
            favorites: f.clone(),
            recent: f,
            ..Settings::default()
        };
        s(vec!["a".into(), "x".repeat(64)])
            .validate_with(false)
            .unwrap();
        assert!(s(vec!["".into()]).validate_with(false).is_err());
        assert!(s(vec!["x".repeat(65)]).validate_with(false).is_err());
        assert!(s(vec!["a\nb".into()]).validate_with(false).is_err());
        assert!(s((0..501).map(|i| i.to_string()).collect())
            .validate_with(false)
            .is_err());
        let only_recent = Settings {
            recent: vec!["".into()],
            ..Settings::default()
        };
        assert!(only_recent.validate_with(false).is_err());
    }

    #[test]
    fn pasta_do_jogo_precisa_ser_absoluta() {
        let rel = Settings {
            game_dir: Some(PathBuf::from("jogo")),
            ..Settings::default()
        };
        assert_eq!(
            rel.validate_with(false).unwrap_err().code,
            "invalid_settings"
        );
        let abs = Settings {
            game_dir: Some(std::env::temp_dir()),
            ..Settings::default()
        };
        abs.validate_with(false).unwrap();
    }

    #[test]
    fn override_do_hub_so_em_dev_e_so_valido() {
        let stored = "https://hub.example.com";
        let env = || Some("http://127.0.0.1:8080".to_string());
        assert_eq!(
            resolve_hub_url(stored, env(), true),
            "http://127.0.0.1:8080"
        );
        assert_eq!(resolve_hub_url(stored, env(), false), stored);
        assert_eq!(resolve_hub_url(stored, None, true), stored);
        assert_eq!(resolve_hub_url(stored, Some("".into()), true), stored);
        assert_eq!(
            resolve_hub_url(stored, Some("http://evil.example.com".into()), true),
            stored
        );
    }
}
