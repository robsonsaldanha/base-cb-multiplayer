//! Ping dos servidores (`GET http://host:httpPort/info`) contra servidores locais: rápido, lento (> 2 s), recusado,
//! endereço inválido/recusado, limite global de 16 simultâneos, um pedido por endereço, intervalo mínimo, regra de
//! endereços (IP literal e resolução de nomes) e a lista guardada em `servers-cache.json` (sem id repetido).

use axum::extract::State;
use axum::routing::get;
use axum::Router;
use cbmp_launcher_lib::content::{ip_allowed, server_base_url_with, ServerResolver};
use cbmp_launcher_lib::hub::ServerListing;
use cbmp_launcher_lib::ping::{
    PingOptions, PingResult, PingTarget, Pinger, DEADLINE, MAX_CONCURRENT, MAX_IDS, MIN_INTERVAL,
};
use cbmp_launcher_lib::server_cache::{sanitize, CachedServers, ServerDirectory, MAX_SERVERS};
use std::net::IpAddr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

#[derive(Default)]
struct Load {
    now: AtomicUsize,
    max: AtomicUsize,
    total: AtomicUsize,
}

async fn info(delay: Duration, load: Arc<Load>) -> &'static str {
    load.total.fetch_add(1, Ordering::SeqCst);
    let n = load.now.fetch_add(1, Ordering::SeqCst) + 1;
    load.max.fetch_max(n, Ordering::SeqCst);
    tokio::time::sleep(delay).await;
    load.now.fetch_sub(1, Ordering::SeqCst);
    r#"{"serverId":"x","name":"x","players":1,"maxPlayers":2,"protocol":1,"version":"1.0.0"}"#
}

/// `cb-server` falso que responde `/info` depois de `delay`, contando em `load` (pode ser compartilhado).
async fn start_with(delay: Duration, load: Arc<Load>) -> u16 {
    let app = Router::new()
        .route(
            "/info",
            get(move |State(l): State<Arc<Load>>| info(delay, l)),
        )
        .with_state(load);
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = listener.local_addr().unwrap().port();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    port
}

async fn start(delay: Duration) -> (u16, Arc<Load>) {
    let load = Arc::new(Load::default());
    (start_with(delay, load.clone()).await, load)
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

/// Sem intervalo mínimo (cada chamada pinga de verdade).
fn fresh() -> Pinger {
    Pinger::new(PingOptions {
        min_interval: Duration::ZERO,
        ..PingOptions::default()
    })
}

#[test]
fn valores_do_contrato() {
    assert_eq!(DEADLINE, Duration::from_secs(2));
    assert_eq!(MAX_CONCURRENT, 16);
    assert_eq!(MIN_INTERVAL, Duration::from_secs(25));
    let d = PingOptions::default();
    assert_eq!(
        (d.deadline, d.max_concurrent, d.min_interval),
        (DEADLINE, 16, MIN_INTERVAL)
    );
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
    let r = fresh()
        .ping(vec![
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
    // Folga para runner carregado; o essencial é não esperar os 4 s do lento.
    assert!(
        took >= Duration::from_millis(1900) && took < Duration::from_millis(3900),
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
    let r = fresh().ping(vec![target("v6", "::1", port)]).await;
    assert!(r[0].ms.is_some(), "{r:?}");
}

#[tokio::test(flavor = "multi_thread")]
async fn no_maximo_16_simultaneos_mesmo_entre_chamadas() {
    // 40 endereços distintos (portas diferentes) contando no mesmo `Load`.
    let load = Arc::new(Load::default());
    let mut ports = Vec::new();
    for _ in 0..40 {
        ports.push(start_with(Duration::from_millis(200), load.clone()).await);
    }
    let pinger = fresh();
    let batch = |from: usize| -> Vec<PingTarget> {
        ports[from..from + 20]
            .iter()
            .enumerate()
            .map(|(i, p)| target(&format!("s{}", from + i), "127.0.0.1", *p))
            .collect()
    };
    // Duas chamadas ao mesmo tempo (troca de tela + intervalo de 30 s) dividem o mesmo limite.
    let (a, b) = tokio::join!(pinger.ping(batch(0)), pinger.ping(batch(20)));
    assert!(a.iter().chain(&b).all(|p| p.ms.is_some()), "{a:?} {b:?}");
    let max = load.max.load(Ordering::SeqCst);
    assert!(max <= 16, "{max} simultâneos");
    assert!(max >= 4, "em paralelo de verdade: {max}");
    assert_eq!(load.total.load(Ordering::SeqCst), 40);
}

#[tokio::test(flavor = "multi_thread")]
async fn um_pedido_por_endereco() {
    let (port, load) = start(Duration::ZERO).await;
    let r = fresh()
        .ping(vec![
            target("a", "127.0.0.1", port),
            target("b", "127.0.0.1", port),
            target("c", "127.0.0.1", port),
            target("d", "127.0.0.1", port),
        ])
        .await;
    assert!(r.iter().all(|p| p.ms.is_some()), "{r:?}");
    assert_eq!(
        load.total.load(Ordering::SeqCst),
        1,
        "ids com o mesmo host:porta geram um pedido só"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn intervalo_minimo_reaproveita_o_resultado() {
    let (port, load) = start(Duration::ZERO).await;
    let pinger = Pinger::new(PingOptions {
        min_interval: Duration::from_millis(400),
        ..PingOptions::default()
    });
    let t = || vec![target("a", "127.0.0.1", port)];
    let first = pinger.ping(t()).await;
    let again = pinger.ping(t()).await;
    assert!(first[0].ms.is_some());
    assert_eq!(again, first, "chamada dentro do intervalo reaproveita");
    assert_eq!(load.total.load(Ordering::SeqCst), 1);
    tokio::time::sleep(Duration::from_millis(500)).await;
    pinger.ping(t()).await;
    assert_eq!(
        load.total.load(Ordering::SeqCst),
        2,
        "passado o intervalo, pinga de novo"
    );
    // Padrão do contrato: 25 s.
    let p = Pinger::default();
    p.ping(t()).await;
    p.ping(t()).await;
    assert_eq!(load.total.load(Ordering::SeqCst), 3);
}

#[tokio::test(flavor = "multi_thread")]
async fn opcoes_menores_para_teste() {
    let (slow, _) = start(Duration::from_millis(600)).await;
    let r = Pinger::new(PingOptions {
        deadline: Duration::from_millis(200),
        max_concurrent: 1,
        min_interval: Duration::ZERO,
    })
    .ping(vec![target("a", "127.0.0.1", slow)])
    .await;
    assert_eq!(r[0].ms, None);
}

// ----- regra de endereços -----

fn ip(s: &str) -> IpAddr {
    s.parse().unwrap()
}

#[test]
fn enderecos_sempre_recusados() {
    for a in [
        "0.0.0.0",
        "0.1.2.3",
        "::",
        "224.0.0.1",
        "239.255.255.250",
        "ff02::1",
        "255.255.255.255",
        "240.0.0.1",
        "169.254.169.254",
        "169.254.0.1",
        "fe80::1",
        "febf::1",
        "::ffff:169.254.169.254",
        "::ffff:0.0.0.0",
        "::ffff:224.0.0.1",
        "::ffff:255.255.255.255",
    ] {
        for dev in [false, true] {
            assert!(!ip_allowed(ip(a), dev), "{a} dev={dev}");
        }
    }
}

#[test]
fn locais_so_em_debug() {
    for a in [
        "127.0.0.1",
        "127.1.2.3",
        "::1",
        "10.0.0.1",
        "172.16.0.1",
        "172.31.255.255",
        "192.168.1.1",
        "100.64.0.1",
        "100.127.255.255",
        "fc00::1",
        "fd12:3456::1",
        "fec0::1",
        "::ffff:127.0.0.1",
        "::ffff:192.168.0.10",
    ] {
        assert!(!ip_allowed(ip(a), false), "{a} em produção");
        assert!(ip_allowed(ip(a), true), "{a} em debug");
    }
}

#[test]
fn publicos_sempre_aceitos() {
    for a in [
        "203.0.113.5",
        "8.8.8.8",
        "100.63.255.255",
        "100.128.0.1",
        "172.32.0.1",
        "2001:db8::1",
        "2606:4700::1111",
        "::ffff:8.8.8.8",
    ] {
        for dev in [false, true] {
            assert!(ip_allowed(ip(a), dev), "{a} dev={dev}");
        }
    }
}

#[test]
fn url_do_servidor_aplica_a_regra() {
    // Formas numéricas também viram IPv4 antes da conferência.
    for host in [
        "169.254.169.254",
        "[fe80::1]",
        "0.0.0.0",
        "224.0.0.1",
        "::ffff:169.254.169.254",
        "0x7f.1",
        "127.1",
        "10.1.2.3",
    ] {
        let e = server_base_url_with(host, 80, false).unwrap_err();
        assert_eq!(e.code, "server_unavailable", "{host}");
    }
    assert!(server_base_url_with("127.0.0.1", 80, true).is_ok());
    assert!(server_base_url_with("127.1", 80, true).is_ok());
    assert!(server_base_url_with("169.254.169.254", 80, true).is_err());
    assert!(server_base_url_with("203.0.113.5", 80, false).is_ok());
    // Nome de domínio passa aqui e é conferido na resolução.
    assert!(server_base_url_with("jogo.exemplo.com", 80, false).is_ok());
}

#[tokio::test(flavor = "multi_thread")]
async fn nome_que_resolve_para_local_e_recusado_em_producao() {
    let (port, load) = start(Duration::ZERO).await;
    let client = |dev| {
        reqwest::Client::builder()
            .dns_resolver(ServerResolver { dev })
            .no_proxy()
            .build()
            .unwrap()
    };
    let url = format!("http://localhost:{port}/info");
    let prod = client(false).get(&url).send().await;
    assert!(prod.is_err(), "localhost em produção: {prod:?}");
    assert_eq!(load.total.load(Ordering::SeqCst), 0);
    let dev = client(true).get(&url).send().await.unwrap();
    assert!(dev.status().is_success());
    assert_eq!(load.total.load(Ordering::SeqCst), 1);
}

// ----- lista conhecida -----

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
    let (port, load) = start(Duration::ZERO).await;
    let dir = tempfile::tempdir().unwrap();
    let servers = ServerDirectory::new(dir.path().join("servers-cache.json"));
    servers.store_fresh(
        vec![listing("a", port), listing("b", closed_port().await)],
        100,
    );

    let r = fresh()
        .ping_ids(
            &servers,
            vec!["b".into(), "desconhecido".into(), "a".into(), "a".into()],
        )
        .await;
    let ids: Vec<_> = r.iter().map(|p| p.id.as_str()).collect();
    assert_eq!(ids, ["b", "desconhecido", "a", "a"]);
    assert_eq!(r[0].ms, None);
    assert_eq!(r[1].ms, None, "id fora da lista não vira endereço");
    assert!(r[2].ms.is_some() && r[3].ms.is_some());
    assert_eq!(load.total.load(Ordering::SeqCst), 1);
}

#[tokio::test(flavor = "multi_thread")]
async fn ping_por_id_limita_a_quantidade() {
    let dir = tempfile::tempdir().unwrap();
    let servers = ServerDirectory::new(dir.path().join("servers-cache.json"));
    let ids: Vec<String> = (0..1000).map(|i| format!("x{i}")).collect();
    let r = fresh().ping_ids(&servers, ids).await;
    assert_eq!(r.len(), MAX_IDS);
}

#[test]
fn lista_sem_id_repetido_nem_invalido() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("servers-cache.json");
    let servers = ServerDirectory::new(path.clone());
    let mut dup = listing("a", 2);
    dup.name = "Impostor".into();
    let out = servers.store_fresh(
        vec![
            listing("a", 1),
            listing("", 1),
            listing(&"x".repeat(65), 1),
            listing("c\nd", 1),
            dup,
            listing("b", 3),
            listing("a", 4),
        ],
        10,
    );
    let ids: Vec<_> = out.iter().map(|s| s.id.as_str()).collect();
    assert_eq!(ids, ["a", "b"], "fica a primeira ocorrência");
    assert_eq!(out[0].name, "Servidor a");
    assert_eq!(
        servers.targets(&["a".into()])[0]
            .as_ref()
            .unwrap()
            .http_port,
        1
    );
    // O que foi para o disco também está limpo.
    let disk: CachedServers = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(disk.servers, out);

    // Cache antigo gravado com repetidos (versão anterior) é limpo ao ler.
    let raw = CachedServers {
        saved_at: 5,
        servers: vec![listing("z", 1), listing("z", 2), listing("y", 3)],
    };
    std::fs::write(&path, serde_json::to_vec(&raw).unwrap()).unwrap();
    let c = ServerDirectory::new(path).cached().unwrap();
    let ids: Vec<_> = c.servers.iter().map(|s| s.id.as_str()).collect();
    assert_eq!(ids, ["z", "y"]);
}

#[test]
fn lista_com_teto_e_textos_cortados() {
    let many: Vec<_> = (0..MAX_SERVERS + 50)
        .map(|i| listing(&format!("s{i}"), 1))
        .collect();
    assert_eq!(sanitize(many).len(), MAX_SERVERS);
    let mut big = listing("a", 1);
    big.name = "é".repeat(500);
    big.description = "d".repeat(5000);
    big.tags = (0..40).map(|_| "t".repeat(100)).collect();
    big.locale = "l".repeat(100);
    let s = &sanitize(vec![big])[0];
    assert_eq!(s.name.chars().count(), 128);
    assert_eq!(s.description.len(), 1024);
    assert_eq!(s.tags.len(), 16);
    assert!(s.tags.iter().all(|t| t.len() == 32));
    assert_eq!(s.locale.len(), 16);
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
fn memoria_mais_nova_que_o_disco_vence() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("servers-cache.json");
    ServerDirectory::new(path.clone()).store_fresh(vec![listing("velho", 1)], 100);
    // Gravação falha (pasta no lugar do `.tmp`), mas a lista nova fica na memória.
    std::fs::create_dir(dir.path().join("servers-cache.json.tmp")).unwrap();
    let servers = ServerDirectory::new(path);
    servers.store_fresh(vec![listing("novo", 2)], 200);
    let c = servers.cached().unwrap();
    assert_eq!(c.saved_at, 200);
    assert_eq!(c.servers[0].id, "novo");
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
