//! Perfil do build. Três sabores:
//! - **release oficial** (padrão): Hub fixo em https, servidores só em endereços públicos;
//! - **debug** (`cargo test`, `tauri dev`): regras de rede relaxadas e `CBMP_HUB_URL`/`CBMP_DATA_DIR` aceitos;
//! - **teste local** (`--features local-test` + `tauri.localtest.conf.json`, ligado só por flag explícita): build
//!   release para os testes manuais com o Hub local (`tools/dev-env.mjs`). Hub em [`LOCAL_TEST_HUB_URL`], servidores
//!   em loopback/rede privada aceitos, updater apontado para o Hub local, nome "CB Multiplayer (Teste local)" e
//!   identificador próprio. Nunca é publicado.

/// Build de teste local (feature Cargo `local-test`).
pub const LOCAL_TEST: bool = cfg!(feature = "local-test");

/// Regras de rede relaxadas: `http://` de loopback para o Hub e o navegador, servidores de jogo em loopback e redes
/// privadas. Debug ou teste local; nunca no release oficial.
pub const RELAXED_NETWORK: bool = cfg!(debug_assertions) || LOCAL_TEST;

/// Hub do build de teste local (o Hub de `tools/dev-env.mjs`; IPv4 literal, como o `cb-server` usa).
pub const LOCAL_TEST_HUB_URL: &str = "http://127.0.0.1:8787";

#[cfg(test)]
mod tests {
    #[test]
    fn testes_rodam_com_regras_relaxadas() {
        // `cargo test` é debug: as regras relaxadas valem com ou sem a feature.
        const { assert!(super::RELAXED_NETWORK) };
    }
}
