//! Ping dos servidores (`GET http://host:httpPort/info`) contra servidores locais: rápido, lento (> 2 s), recusado,
//! endereço inválido, limite de 16 simultâneos e a lista guardada em `servers-cache.json`.

use axum::extract::State;
use axum::routing::get;
use axum::Router;
use cbmp_launcher_lib::hub::ServerListing;
use cbmp_launcher_lib::ping::{
    ping_all, ping_ids, ping_with, PingOptions, PingResult, PingTarget, DEADLINE, MAX_CONCURRENT,
};
use cbmp_launcher_lib::server_cache::{CachedServers, ServerDirectory};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

#[derive(Default)]
struct Load {
    now: AtomicUsize,
    max: AtomicUsize,
}

async fn info(delay: Duration, load: Arc<Load>) -> &'static str {
    let n = load.now.fetch_add(1, Ordering::SeqCst) + 1;
    load.max.fetch_max(n, Ordering::SeqCst);
    tokio::time::sleep(delay).await;
    load.now.fetch_sub(1, Ordering::SeqCst);
    r#"{"serverId":"x","name":"x","players":1,"maxPlayers":2,"protocol":1,"version":"1.0.0"}"#
}

/// `cb-server` falso que responde `/info` depois de `delay`.
async fn start(delay: Duration) -> (u16, Arc<Load>) {
    let load = Arc::new(Load::default());
    let app = Router::new()
        .route(
            "/info",
            get(move |State(l): State<Arc<Load>>| info(delay, l)),
        )
        .with_state(load.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = listener.local_addr().unwrap().port();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    (port, load)
}

/// Porta sem ninguém escutando (conexão recusada).
async fn closed_port() -> u16 {
    let l = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    l.local_addr().unwrap().port()
}

fn target(id: &str, host: &str, port: u16) -> PingTarget {
    PingTarget {
        id: id.into(),
        host: host.into(),
        http_port: port,
    }
}

#[test]
fn valores_do_contrato() {
    assert_eq!(DEADLINE, Duration::from_secs(2));
    assert_eq!(MAX_CONCURRENT, 16);
    let v = serde_json::to_value(PingResult {
        id: "a".into(),
        ms: None,
    })
    .unwrap();
    assert_eq!(v, serde_json::json!({ "id": "a", "ms": null }));
}

#[tokio::test(flavor = "multi_thread")]
async fn rapido_lento_e_recusado() {
    let (fast, _) = start(Duration::ZERO).await;
    let (slow, _) = start(Duration::from_secs(4)).await;
    let refused = closed_port().await;

    let started = Instant::now();
    let r = ping_all(vec![
        target("rapido", "127.0.0.1", fast),
        target("lento", "127.0.0.1", slow),
        target("recusado", "127.0.0.1", refused),
        target("invalido", "host com espaço", fast),
        target("porta0", "127.0.0.1", 0),
    ])
    .await;
    let took = started.elapsed();

    let ids: Vec<_> = r.iter().map(|p| p.id.as_str()).collect();
    assert_eq!(
        ids,
        ["rapido", "lento", "recusado", "invalido", "porta0"],
        "ordem da entrada"
    );
    let fast_ms = r[0].ms.expect("servidor rápido responde");
    assert!(fast_ms < 1000, "{fast_ms} ms");
    assert_eq!(r[1].ms, None, "lento passa do prazo de 2 s");
    assert_eq!(r[2].ms, None, "conexão recusada");
    assert_eq!(r[3].ms, None, "host inválido");
    assert_eq!(r[4].ms, None, "porta 0");
    assert!(
        took >= Duration::from_millis(1900) && took < Duration::from_millis(3500),
        "prazo de 2 s respeitado: {took:?}"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn ipv6_de_loopback() {
    let Ok(l) = tokio::net::TcpListener::bind("[::1]:0").await else {
        return; // máquina sem IPv6
    };
    let port = l.local_addr().unwrap().port();
    let app = Router::new().route("/info", get(|| async { "{}" }));
    tokio::spawn(async move { axum::serve(l, app).await.unwrap() });
    let r = ping_all(vec![target("v6", "::1", port)]).await;
    assert!(r[0].ms.is_some(), "{r:?}");
}

#[tokio::test(flavor = "multi_thread")]
async fn no_maximo_16_simultaneos() {
    let (port, load) = start(Duration::from_millis(150)).await;
    let targets: Vec<_> = (0..40)
        .map(|i| target(&format!("s{i}"), "127.0.0.1", port))
        .collect();
    let r = ping_all(targets).await;
    assert_eq!(r.len(), 40);
    assert!(r.iter().all(|p| p.ms.is_some()), "{r:?}");
    let max = load.max.load(Ordering::SeqCst);
    assert!(max <= 16, "{max} simultâneos");
    assert!(max >= 8, "em paralelo de verdade: {max}");
}

#[tokio::test(flavor = "multi_thread")]
async fn opcoes_menores_para_teste() {
    let (slow, _) = start(Duration::from_millis(600)).await;
    let r = ping_with(
        vec![target("a", "127.0.0.1", slow)],
        PingOptions {
            deadline: Duration::from_millis(200),
            max_concurrent: 1,
        },
    )
    .await;
    assert_eq!(r[0].ms, None);
}

fn listing(id: &str, port: u16) -> ServerListing {
    ServerListing {
        id: id.into(),
        name: format!("Servidor {id}"),
        description: String::new(),
        tags: vec!["rp".into()],
        locale: "pt-BR".into(),
        players: 1,
        max_players: 10,
        host: "127.0.0.1".into(),
        game_port: 7777,
        http_port: port,
        version: "1.0.0".into(),
        protocol: 1,
        featured: false,
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn ping_por_id_usa_a_lista_conhecida() {
    let (port, _) = start(Duration::ZERO).await;
    let dir = tempfile::tempdir().unwrap();
    let servers = ServerDirectory::new(dir.path().join("servers-cache.json"));
    servers.store_fresh(
        vec![listing("a", port), listing("b", closed_port().await)],
        100,
    );

    let r = ping_ids(
        &servers,
        vec!["b".into(), "desconhecido".into(), "a".into(), "a".into()],
    )
    .await;
    let ids: Vec<_> = r.iter().map(|p| p.id.as_str()).collect();
    assert_eq!(ids, ["b", "desconhecido", "a", "a"]);
    assert_eq!(r[0].ms, None);
    assert_eq!(r[1].ms, None, "id fora da lista não vira endereço");
    assert!(r[2].ms.is_some() && r[3].ms.is_some());
}

#[tokio::test(flavor = "multi_thread")]
async fn ping_por_id_limita_a_quantidade() {
    let dir = tempfile::tempdir().unwrap();
    let servers = ServerDirectory::new(dir.path().join("servers-cache.json"));
    let ids: Vec<String> = (0..1000).map(|i| format!("x{i}")).collect();
    let r = ping_ids(&servers, ids).await;
    assert_eq!(r.len(), cbmp_launcher_lib::ping::MAX_IDS);
}

#[test]
fn cache_da_lista_ida_e_volta() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("servers-cache.json");
    let servers = ServerDirectory::new(path.clone());
    assert_eq!(servers.cached(), None, "sem arquivo");

    servers.store_fresh(vec![listing("a", 30120)], 1_700_000_000);
    assert!(path.is_file());
    let names: Vec<_> = std::fs::read_dir(dir.path())
        .unwrap()
        .map(|e| e.unwrap().file_name())
        .collect();
    assert_eq!(names.len(), 1, "sem .tmp para trás: {names:?}");

    // Outra instância (launcher reaberto sem Hub) lê o arquivo e passa a conhecer os endereços.
    let reopened = ServerDirectory::new(path.clone());
    let cached = reopened.cached().unwrap();
    assert_eq!(
        cached,
        CachedServers {
            saved_at: 1_700_000_000,
            servers: vec![listing("a", 30120)]
        }
    );
    let v = serde_json::to_value(&cached).unwrap();
    assert!(v.get("savedAt").is_some() && v["servers"][0].get("httpPort").is_some());
    assert_eq!(
        reopened.targets(&["a".into()])[0]
            .as_ref()
            .unwrap()
            .http_port,
        30120
    );
}

#[test]
fn cache_corrompido_ou_grande_e_ignorado() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("servers-cache.json");
    std::fs::write(&path, b"{ nao e json").unwrap();
    assert_eq!(ServerDirectory::new(path.clone()).cached(), None);

    std::fs::write(&path, br#"{"savedAt":1,"servers":[{"id":"a"}]}"#).unwrap();
    assert_eq!(ServerDirectory::new(path.clone()).cached(), None);

    let big = vec![b' '; 5 * 1024 * 1024];
    std::fs::write(&path, big).unwrap();
    assert_eq!(ServerDirectory::new(path).cached(), None);
}
