//! Ping dos servidores: `GET http://host:httpPort/info`, prazo de [`DEADLINE`] por servidor. Sem resposta 2xx dentro
//! do prazo (lento, recusado, endereço recusado) → `ms: None`.
//!
//! O endereço vem do Hub (não confiável), então o ping não pode virar um gerador de pedidos controlado por ele:
//! - a UI pede por id ([`Pinger::ping_ids`]); host e porta vêm da lista do Hub guardada no [`ServerDirectory`];
//! - endereços passam pela regra de `content::server_base_url`/[`ServerResolver`] (sem link-local, multicast etc.;
//!   loopback e rede privada só em debug/teste local);
//! - ids que apontam para o mesmo `host:porta` geram um pedido só;
//! - no máximo [`MAX_CONCURRENT`] pedidos em voo no launcher inteiro (semáforo único do [`Pinger`]);
//! - o mesmo endereço é pingado no máximo uma vez a cada [`MIN_INTERVAL`] (chamadas próximas reaproveitam o resultado).

use crate::content::{server_base_url, ServerResolver};
use crate::server_cache::ServerDirectory;
use serde::Serialize;
use std::collections::HashMap;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};
use tokio::sync::Semaphore;

pub const DEADLINE: Duration = Duration::from_secs(2);
pub const MAX_CONCURRENT: usize = 16;
/// Intervalo mínimo entre dois pings reais do mesmo endereço (a UI pinga a cada 30 s).
pub const MIN_INTERVAL: Duration = Duration::from_secs(25);
/// Teto de ids por chamada (a UI só pinga o que está na tela e corta no mesmo valor).
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
    pub min_interval: Duration,
}

impl Default for PingOptions {
    fn default() -> Self {
        Self {
            deadline: DEADLINE,
            max_concurrent: MAX_CONCURRENT,
            min_interval: MIN_INTERVAL,
        }
    }
}

/// Um por launcher (estado gerenciado do Tauri): semáforo, cliente e últimos resultados compartilhados entre chamadas.
pub struct Pinger {
    opts: PingOptions,
    client: reqwest::Client,
    gate: Arc<Semaphore>,
    /// Endereço normalizado (`http://host:porta/`) → (quando terminou, resultado).
    recent: Mutex<HashMap<String, (Instant, Option<u32>)>>,
}

impl Default for Pinger {
    fn default() -> Self {
        Self::new(PingOptions::default())
    }
}

impl Pinger {
    pub fn new(opts: PingOptions) -> Self {
        Self {
            opts,
            client: client(opts.deadline),
            gate: Arc::new(Semaphore::new(opts.max_concurrent.max(1))),
            recent: Mutex::new(HashMap::new()),
        }
    }

    fn recent(&self) -> MutexGuard<'_, HashMap<String, (Instant, Option<u32>)>> {
        self.recent.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Pinga os alvos (resultado na ordem da entrada). Endereço inválido/recusado → `None` sem pedido.
    pub async fn ping(&self, targets: Vec<PingTarget>) -> Vec<PingResult> {
        // Um pedido por endereço; resultado ainda fresco é reaproveitado.
        let keys: Vec<Option<String>> = targets
            .iter()
            .map(|t| {
                server_base_url(&t.host, t.http_port)
                    .ok()
                    .map(|u| u.to_string())
            })
            .collect();
        let mut known: HashMap<String, Option<u32>> = HashMap::new();
        let mut todo: Vec<String> = Vec::new();
        {
            let now = Instant::now();
            let mut recent = self.recent();
            recent.retain(|_, (at, _)| now.duration_since(*at) < self.opts.min_interval);
            for k in keys.iter().flatten() {
                if known.contains_key(k) || todo.contains(k) {
                    continue;
                }
                match recent.get(k) {
                    Some((_, ms)) => {
                        known.insert(k.clone(), *ms);
                    }
                    None => todo.push(k.clone()),
                }
            }
        }

        let mut set = tokio::task::JoinSet::new();
        for k in todo {
            let (client, gate, deadline) =
                (self.client.clone(), self.gate.clone(), self.opts.deadline);
            set.spawn(async move {
                let Ok(_permit) = gate.acquire_owned().await else {
                    return (k, None);
                };
                let ms = ping_one(&client, &k, deadline).await;
                (k, ms)
            });
        }
        while let Some(r) = set.join_next().await {
            if let Ok((k, ms)) = r {
                self.recent().insert(k.clone(), (Instant::now(), ms));
                known.insert(k, ms);
            }
        }

        targets
            .into_iter()
            .zip(keys)
            .map(|(t, k)| PingResult {
                id: t.id,
                ms: k.and_then(|k| known.get(&k).copied().flatten()),
            })
            .collect()
    }

    /// Pinga os ids pedidos pela UI (no máx. [`MAX_IDS`]); id fora da lista conhecida → `None`.
    pub async fn ping_ids(&self, dir: &ServerDirectory, mut ids: Vec<String>) -> Vec<PingResult> {
        ids.truncate(MAX_IDS);
        let targets = dir.targets(&ids);
        let known: Vec<PingTarget> = targets.iter().flatten().cloned().collect();
        let mut results = self.ping(known).await.into_iter();
        ids.into_iter()
            .zip(targets)
            .map(|(id, t)| match t {
                Some(_) => results.next().unwrap_or(PingResult { id, ms: None }),
                None => PingResult { id, ms: None },
            })
            .collect()
    }
}

fn client(deadline: Duration) -> reqwest::Client {
    if rustls::crypto::CryptoProvider::get_default().is_none() {
        let _ = rustls::crypto::ring::default_provider().install_default();
    }
    reqwest::Client::builder()
        .connect_timeout(deadline)
        .timeout(deadline)
        .redirect(reqwest::redirect::Policy::none())
        // Sem proxy do sistema: a resolução (e a regra de endereços) fica sempre aqui.
        .no_proxy()
        .dns_resolver(ServerResolver::default())
        .user_agent(concat!("CBMultiplayer/", env!("CARGO_PKG_VERSION")))
        .build()
        .expect("cliente HTTP")
}

async fn ping_one(client: &reqwest::Client, base: &str, deadline: Duration) -> Option<u32> {
    let url = url::Url::parse(base).ok()?.join("info").ok()?;
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
