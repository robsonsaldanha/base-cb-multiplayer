//! Ping dos servidores: `GET http://host:httpPort/info` em paralelo (no máx. [`MAX_CONCURRENT`]), prazo de
//! [`DEADLINE`] por servidor. Sem resposta 2xx dentro do prazo (lento, recusado, endereço inválido) → `ms: None`.
//!
//! A UI pede por id ([`ping_ids`]); host e porta vêm da lista do Hub guardada no [`ServerDirectory`].

use crate::content::server_base_url;
use crate::server_cache::ServerDirectory;
use serde::Serialize;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::Semaphore;

pub const DEADLINE: Duration = Duration::from_secs(2);
pub const MAX_CONCURRENT: usize = 16;
/// Teto de ids por chamada (a UI só pinga o que está na tela).
pub const MAX_IDS: usize = 200;
/// Corpo do `/info` lido no máximo até aqui (a resposta é um JSON pequeno).
const MAX_BODY: usize = 64 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PingTarget {
    pub id: String,
    pub host: String,
    pub http_port: u16,
}

/// Resultado de `ping_servers`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PingResult {
    pub id: String,
    pub ms: Option<u32>,
}

#[derive(Debug, Clone, Copy)]
pub struct PingOptions {
    pub deadline: Duration,
    pub max_concurrent: usize,
}

impl Default for PingOptions {
    fn default() -> Self {
        Self {
            deadline: DEADLINE,
            max_concurrent: MAX_CONCURRENT,
        }
    }
}

/// Pinga com os valores do contrato (2 s, 16 simultâneos). Resultado na ordem da entrada.
pub async fn ping_all(targets: Vec<PingTarget>) -> Vec<PingResult> {
    ping_with(targets, PingOptions::default()).await
}

pub async fn ping_with(targets: Vec<PingTarget>, opts: PingOptions) -> Vec<PingResult> {
    let client = client(opts.deadline);
    let gate = Arc::new(Semaphore::new(opts.max_concurrent.max(1)));
    let mut set = tokio::task::JoinSet::new();
    for (i, t) in targets.iter().enumerate() {
        let (client, gate, t) = (client.clone(), gate.clone(), t.clone());
        set.spawn(async move {
            let Ok(_permit) = gate.acquire_owned().await else {
                return (i, None);
            };
            (i, ping_one(&client, &t, opts.deadline).await)
        });
    }
    let mut ms = vec![None; targets.len()];
    while let Some(r) = set.join_next().await {
        if let Ok((i, v)) = r {
            ms[i] = v;
        }
    }
    targets
        .into_iter()
        .zip(ms)
        .map(|(t, ms)| PingResult { id: t.id, ms })
        .collect()
}

/// Pinga os ids pedidos pela UI (no máx. [`MAX_IDS`]); id fora da lista conhecida → `None`.
pub async fn ping_ids(dir: &ServerDirectory, mut ids: Vec<String>) -> Vec<PingResult> {
    ids.truncate(MAX_IDS);
    let targets = dir.targets(&ids);
    let known: Vec<PingTarget> = targets.iter().flatten().cloned().collect();
    let mut results = ping_all(known).await.into_iter();
    ids.into_iter()
        .zip(targets)
        .map(|(id, t)| match t {
            Some(_) => results.next().unwrap_or(PingResult { id, ms: None }),
            None => PingResult { id, ms: None },
        })
        .collect()
}

fn client(deadline: Duration) -> reqwest::Client {
    if rustls::crypto::CryptoProvider::get_default().is_none() {
        let _ = rustls::crypto::ring::default_provider().install_default();
    }
    reqwest::Client::builder()
        .connect_timeout(deadline)
        .timeout(deadline)
        .redirect(reqwest::redirect::Policy::none())
        .user_agent(concat!("CBMultiplayer/", env!("CARGO_PKG_VERSION")))
        .build()
        .expect("cliente HTTP")
}

async fn ping_one(client: &reqwest::Client, t: &PingTarget, deadline: Duration) -> Option<u32> {
    let url = server_base_url(&t.host, t.http_port)
        .ok()?
        .join("info")
        .ok()?;
    let started = Instant::now();
    let fetch = async {
        let mut resp = client.get(url).send().await.ok()?;
        if !resp.status().is_success() {
            return None;
        }
        let mut read = 0usize;
        while let Some(chunk) = resp.chunk().await.ok()? {
            read += chunk.len();
            if read > MAX_BODY {
                return None;
            }
        }
        Some(())
    };
    tokio::time::timeout(deadline, fetch).await.ok()??;
    Some(u32::try_from(started.elapsed().as_millis()).unwrap_or(u32::MAX))
}
