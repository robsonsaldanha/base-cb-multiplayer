//! Auto-update pelo plugin updater: chave pública embutida, endpoint do Hub, checagem ao iniciar e a cada 6 h.
//!
//! A assinatura (minisign/Ed25519) é conferida pelo plugin com esta chave; o Hub só repassa os arquivos. Nada é
//! instalado sozinho: a checagem periódica só avisa a UI (`update://available`) e a instalação é o comando
//! `install_update`. O download tenta 3 vezes com espera crescente (o Hub derruba quem fica abaixo de 32 KiB/s e não
//! serve `Range`).

use crate::error::{AppError, AppResult};
use serde::Serialize;
use std::time::Duration;
use url::Url;

/// Intervalo da checagem em segundo plano.
pub const CHECK_INTERVAL: Duration = Duration::from_secs(6 * 60 * 60);
/// Evento para a UI quando a checagem periódica acha versão nova (carga: [`UpdateStatus`]).
pub const UPDATE_AVAILABLE: &str = "update://available";
/// Tentativas de download da atualização.
pub const ATTEMPTS: u32 = 3;
const RETRY_DELAY: Duration = Duration::from_secs(3);

/// Resposta de `check_update`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateStatus {
    pub available: bool,
    pub version: Option<String>,
    pub notes: Option<String>,
}

/// Conteúdo de `keys/update.pub` (base64 do arquivo de chave pública do minisign, como o Tauri espera).
pub const PUBKEY: &str = include_str!("../keys/update.pub");

/// Caminho da rota de update no Hub; `{{…}}` são preenchidos pelo plugin updater.
pub const ENDPOINT_PATH: &str = "v1/launcher/update/{{target}}/{{arch}}/{{current_version}}";

pub fn pubkey() -> &'static str {
    PUBKEY.trim()
}

/// Endpoint do updater para um Hub. Monta como texto (`Url::join` descartaria um prefixo sem barra final).
/// O `Url` codifica `{{target}}` como `%7B%7Btarget%7D%7D`; o plugin updater substitui as duas formas.
pub fn endpoint(hub_url: &str) -> Result<Url, url::ParseError> {
    let base = Url::parse(hub_url)?;
    let mut s = base.as_str().trim_end_matches('/').to_string();
    s.push('/');
    s.push_str(ENDPOINT_PATH);
    Url::parse(&s)
}

/// Mesmo esquema, host e porta.
pub fn same_origin(a: &Url, b: &Url) -> bool {
    a.scheme() == b.scheme()
        && a.host() == b.host()
        && a.port_or_known_default() == b.port_or_known_default()
}

fn signature_invalid() -> AppError {
    AppError::new(
        "signature_invalid",
        "A atualização baixada não tem assinatura válida e foi descartada; o launcher atual continua.",
    )
}

/// Versão gravada pelo Tauri no comentário confiável da assinatura (`timestamp:..\tfile:..\tversion:X.Y.Z`,
/// `tauri build` / `tauri signer sign --app-version`). O comentário é coberto pela assinatura.
pub fn signed_launcher_version(trusted_comment: &str) -> Option<&str> {
    trusted_comment
        .split('\t')
        .find_map(|f| f.strip_prefix("version:"))
        .filter(|v| !v.is_empty())
}

/// Confere de novo (além do plugin, que já exige `requireSignedVersion`) que o pacote baixado está assinado com a
/// chave embutida **e para a versão anunciada**: um Hub comprometido não consegue servir um launcher antigo
/// assinado como se fosse novo.
pub fn verify_launcher_package(
    bytes: &[u8],
    signature: &str,
    pubkey: &str,
    announced: &str,
) -> AppResult<()> {
    let comment = crate::modpkg::verify_minisign(bytes, signature, pubkey)
        .map_err(|_| signature_invalid())?;
    let signed = signed_launcher_version(&comment).ok_or_else(|| {
        log::warn!("atualização assinada sem versão");
        signature_invalid()
    })?;
    let same = match (
        semver::Version::parse(signed.trim_start_matches('v')),
        semver::Version::parse(announced.trim_start_matches('v')),
    ) {
        (Ok(a), Ok(b)) => a == b,
        _ => false,
    };
    if !same {
        log::warn!("atualização assinada para {signed}, anunciada como {announced}");
        return Err(signature_invalid());
    }
    Ok(())
}

/// Falha do download que vale tentar de novo (rede, corpo cortado, status de erro do Hub).
pub fn retriable(e: &tauri_plugin_updater::Error) -> bool {
    use tauri_plugin_updater::Error as E;
    matches!(e, E::Reqwest(_) | E::Network(_))
}

/// Erro do plugin → código estável. `checking`: falha ao procurar (não ao baixar/instalar).
pub fn map_updater_error(e: &tauri_plugin_updater::Error, checking: bool) -> AppError {
    use tauri_plugin_updater::Error as E;
    match e {
        E::Minisign(_)
        | E::Base64(_)
        | E::SignatureUtf8(_)
        | E::SignedVersionMismatch { .. }
        | E::MissingSignedVersion => signature_invalid(),
        _ if checking => AppError::new(
            "update_check_failed",
            "Não foi possível procurar atualização agora. Tente mais tarde.",
        ),
        E::Reqwest(_) | E::Network(_) => crate::hub::download_slow("a atualização"),
        _ => AppError::new(
            "update_failed",
            "Não foi possível instalar a atualização; o launcher atual continua.",
        ),
    }
}

fn hub_invalid() -> AppError {
    AppError::invalid_settings("Endereço do Hub inválido.")
}

async fn find(
    app: &tauri::AppHandle,
    hub_url: &str,
) -> AppResult<Option<tauri_plugin_updater::Update>> {
    use tauri_plugin_updater::UpdaterExt;
    let base = Url::parse(hub_url).map_err(|_| hub_invalid())?;
    let ep = endpoint(hub_url).map_err(|_| hub_invalid())?;
    let updater = app
        .updater_builder()
        .endpoints(vec![ep])
        .and_then(|b| b.build())
        .map_err(|e| map_updater_error(&e, true))?;
    let found = updater.check().await.map_err(|e| {
        log::warn!("checagem de atualização falhou: {e}");
        map_updater_error(&e, true)
    })?;
    // O pacote vem só do próprio Hub (`/v1/dl`); a assinatura é conferida de qualquer jeito.
    if let Some(u) = &found {
        if !same_origin(&u.download_url, &base) {
            log::warn!("atualização anunciada fora do Hub recusada");
            return Err(AppError::new(
                "hub_bad_response",
                "O Hub anunciou uma atualização inválida.",
            ));
        }
    }
    Ok(found)
}

/// Procura versão nova no Hub.
pub async fn check(app: &tauri::AppHandle, hub_url: &str) -> AppResult<UpdateStatus> {
    Ok(match find(app, hub_url).await? {
        Some(u) => UpdateStatus {
            available: true,
            version: Some(u.version.clone()),
            notes: u.body.clone(),
        },
        None => UpdateStatus {
            available: false,
            version: None,
            notes: None,
        },
    })
}

/// Baixa (3 tentativas, espera crescente), confere a assinatura e instala. No Windows o instalador fecha o app.
pub async fn install(app: &tauri::AppHandle, hub_url: &str) -> AppResult<()> {
    let Some(update) = find(app, hub_url).await? else {
        return Err(AppError::new(
            "no_update",
            "Nenhuma atualização disponível.",
        ));
    };
    let mut last = crate::hub::download_slow("a atualização");
    let mut bytes = None;
    for attempt in 0..ATTEMPTS {
        if attempt > 0 {
            tokio::time::sleep(RETRY_DELAY * 2u32.pow(attempt - 1)).await;
        }
        match update.download(|_, _| {}, || {}).await {
            Ok(b) => {
                bytes = Some(b);
                break;
            }
            Err(e) if retriable(&e) => {
                log::warn!(
                    "download da atualização falhou (tentativa {}): {e}",
                    attempt + 1
                );
                last = map_updater_error(&e, false);
            }
            Err(e) => {
                log::warn!("atualização recusada: {e}");
                return Err(map_updater_error(&e, false));
            }
        }
    }
    let bytes = bytes.ok_or(last)?;
    verify_launcher_package(&bytes, &update.signature, pubkey(), &update.version)?;
    log::info!("instalando atualização {}", update.version);
    update.install(bytes).map_err(|e| {
        log::warn!("instalação da atualização falhou: {e}");
        map_updater_error(&e, false)
    })
}

/// Checagem ao iniciar e a cada [`CHECK_INTERVAL`]; avisa a UI por [`UPDATE_AVAILABLE`]. Nunca instala sozinha.
pub fn spawn_periodic(app: tauri::AppHandle, hub_url: String) {
    use tauri::Emitter;
    tauri::async_runtime::spawn(async move {
        loop {
            match check(&app, &hub_url).await {
                Ok(s) if s.available => {
                    log::info!("atualização disponível: {:?}", s.version);
                    let _ = app.emit(UPDATE_AVAILABLE, s);
                }
                Ok(_) => {}
                Err(e) => log::info!("checagem de atualização: {}", e.code),
            }
            tokio::time::sleep(CHECK_INTERVAL).await;
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn erros_do_updater_viram_codigos_estaveis() {
        use tauri_plugin_updater::Error as E;
        let sig = E::Minisign(minisign_verify::Error::InvalidSignature);
        assert_eq!(map_updater_error(&sig, false).code, "signature_invalid");
        assert_eq!(map_updater_error(&sig, true).code, "signature_invalid");
        assert!(!retriable(&sig));
        let net = E::Network("Download request failed with status: 503".into());
        assert!(retriable(&net));
        let slow = map_updater_error(&net, false);
        assert_eq!(slow.code, "download_slow");
        assert!(slow.message.contains("lenta"));
        assert_eq!(map_updater_error(&net, true).code, "update_check_failed");
        assert_eq!(
            map_updater_error(&E::ReleaseNotFound, false).code,
            "update_failed"
        );
        assert!(!retriable(&E::ReleaseNotFound));
    }

    fn signer() -> (minisign::PublicKey, minisign::SecretKey) {
        let kp = minisign::KeyPair::generate_unencrypted_keypair().unwrap();
        (kp.pk, kp.sk)
    }

    fn b64(s: String) -> String {
        use base64::Engine as _;
        base64::engine::general_purpose::STANDARD.encode(s)
    }

    #[test]
    fn pacote_do_launcher_precisa_estar_assinado_para_a_versao_anunciada() {
        let (pk, sk) = signer();
        let pubkey = b64(pk.to_box().unwrap().to_string());
        let data = b"instalador".to_vec();
        let sign = |comment: &str| {
            b64(minisign::sign(
                Some(&pk),
                &sk,
                std::io::Cursor::new(&data),
                Some(comment),
                None,
            )
            .unwrap()
            .to_string())
        };
        let ok = sign("timestamp:1\tfile:CBMultiplayer_1.2.0_x64-setup.exe\tversion:1.2.0");
        verify_launcher_package(&data, &ok, &pubkey, "1.2.0").unwrap();
        // Launcher antigo assinado (1.0.0) anunciado como novo.
        let old = sign("timestamp:1\tfile:x.exe\tversion:1.0.0");
        assert_eq!(
            verify_launcher_package(&data, &old, &pubkey, "1.2.0")
                .unwrap_err()
                .code,
            "signature_invalid"
        );
        // Assinatura sem versão (CLI antigo).
        let none = sign("timestamp:1\tfile:x.exe");
        assert!(verify_launcher_package(&data, &none, &pubkey, "1.2.0").is_err());
        // Bytes alterados.
        assert!(verify_launcher_package(b"outro", &ok, &pubkey, "1.2.0").is_err());
        assert_eq!(signed_launcher_version("a:1\tversion:2.0.0"), Some("2.0.0"));
        assert_eq!(signed_launcher_version("version:"), None);
    }

    #[test]
    fn origem_do_pacote() {
        let hub = Url::parse("https://hub.example.com/base").unwrap();
        assert!(same_origin(
            &Url::parse("https://hub.example.com:443/v1/dl/x").unwrap(),
            &hub
        ));
        for bad in [
            "https://evil.example.com/v1/dl/x",
            "http://hub.example.com/v1/dl/x",
            "https://hub.example.com:8443/v1/dl/x",
        ] {
            assert!(!same_origin(&Url::parse(bad).unwrap(), &hub), "{bad}");
        }
    }

    #[test]
    fn status_serializa_para_a_ui() {
        let v = serde_json::to_value(UpdateStatus {
            available: true,
            version: Some("1.2.3".into()),
            notes: Some("novidades".into()),
        })
        .unwrap();
        assert_eq!(
            v,
            serde_json::json!({ "available": true, "version": "1.2.3", "notes": "novidades" })
        );
        assert_eq!(CHECK_INTERVAL, Duration::from_secs(21_600));
    }

    /// Forma legível (como no tauri.conf.json) do endpoint codificado.
    fn decoded(u: &Url) -> String {
        u.as_str().replace("%7B", "{").replace("%7D", "}")
    }

    #[test]
    fn endpoint_com_e_sem_barra() {
        for hub in ["https://hub.example.com", "https://hub.example.com/"] {
            assert_eq!(
                decoded(&endpoint(hub).unwrap()),
                "https://hub.example.com/v1/launcher/update/{{target}}/{{arch}}/{{current_version}}"
            );
        }
        for hub in ["https://x.example/base", "https://x.example/base/"] {
            assert_eq!(
                decoded(&endpoint(hub).unwrap()),
                "https://x.example/base/v1/launcher/update/{{target}}/{{arch}}/{{current_version}}"
            );
        }
    }

    #[test]
    fn endpoint_usa_os_marcadores_que_o_plugin_substitui() {
        let s = endpoint("https://hub.example.com").unwrap().to_string();
        for m in [
            "%7B%7Btarget%7D%7D",
            "%7B%7Barch%7D%7D",
            "%7B%7Bcurrent_version%7D%7D",
        ] {
            assert!(s.contains(m), "{s} sem {m}");
        }
    }

    #[test]
    fn chave_publica_e_minisign_em_base64() {
        let k = pubkey();
        assert!(!k.is_empty() && !k.contains(char::is_whitespace));
        assert!(k
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '+' || c == '/' || c == '='));
        // "untrusted comment: minisign public key" em base64.
        assert!(k.starts_with("dW50cnVzdGVkIGNvbW1lbnQ6IG1pbmlzaWduIHB1YmxpYyBrZXk"));
    }
}
