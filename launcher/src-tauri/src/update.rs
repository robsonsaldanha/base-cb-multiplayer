//! Configuração do auto-update: chave pública embutida e endpoint do Hub.
//!
//! A assinatura (minisign/Ed25519) é conferida pelo plugin com esta chave; o Hub só repassa os arquivos.

use url::Url;

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

#[cfg(test)]
mod tests {
    use super::*;

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
