//! Perfil do build. Quatro sabores:
//! - **release oficial** (padrão): Hub fixo em https, servidores só em endereços públicos;
//! - **debug** (`cargo test`, `tauri dev`): regras de rede relaxadas e `CBMP_HUB_URL`/`CBMP_DATA_DIR` aceitos;
//! - **teste local** (`--features local-test` + `tauri.localtest.conf.json`, ligado só por flag explícita): build
//!   release para os testes manuais com o Hub local (`tools/dev-env.mjs`). Hub em [`LOCAL_TEST_HUB_URL`], servidores
//!   em loopback/rede privada aceitos, updater apontado para o Hub local, nome "CB Multiplayer (Teste local)" e
//!   identificador próprio. Nunca é publicado.
//! - **teste online** (`--features online-test` + `tauri.onlinetest.conf.json`, só por flag explícita): build release
//!   para testadores com o Hub de teste PÚBLICO em [`ONLINE_TEST_HUB_URL`] (`docs/TESTE-ONLINE.md`). Mesmas regras de
//!   rede do oficial (Hub só https, servidores só em endereços públicos); muda só o Hub, o updater, o nome
//!   "CB Multiplayer (Teste)", o identificador, a pasta de dados e o serviço do cofre. Nunca é o build oficial.

/// Build de teste local (feature Cargo `local-test`).
pub const LOCAL_TEST: bool = cfg!(feature = "local-test");

/// Build de teste online (feature Cargo `online-test`).
pub const ONLINE_TEST: bool = cfg!(feature = "online-test");

#[cfg(all(feature = "local-test", feature = "online-test"))]
compile_error!("as features `local-test` e `online-test` são mutuamente exclusivas");

/// Regras de rede relaxadas: `http://` de loopback para o Hub e o navegador, servidores de jogo em loopback e redes
/// privadas. Debug ou teste local; nunca no release oficial.
pub const RELAXED_NETWORK: bool = cfg!(debug_assertions) || LOCAL_TEST;

/// Hub do build de teste local (o Hub de `tools/dev-env.mjs`; IPv4 literal, como o `cb-server` usa).
pub const LOCAL_TEST_HUB_URL: &str = "http://127.0.0.1:8787";

/// Hub do build de teste online (ambiente de teste na VPS, `docs/AMBIENTE-TESTE-ONLINE.md`).
pub const ONLINE_TEST_HUB_URL: &str = "https://cbmultiplayer.cntoficial.com.br";

#[cfg(test)]
mod tests {
    #[test]
    fn testes_rodam_com_regras_relaxadas() {
        // `cargo test` é debug: as regras relaxadas valem com ou sem a feature.
        const { assert!(super::RELAXED_NETWORK) };
    }

    #[test]
    fn teste_online_nao_relaxa_a_rede_no_release() {
        // O teste online usa as regras do oficial: só o debug (`cargo test`) relaxa.
        assert_eq!(super::RELAXED_NETWORK, cfg!(debug_assertions) || super::LOCAL_TEST);
        assert!(super::ONLINE_TEST_HUB_URL.starts_with("https://"));
    }
}
