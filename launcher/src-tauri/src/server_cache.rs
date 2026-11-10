//! Lista de servidores conhecida pelo launcher: a última resposta de `GET /v1/servers`, em memória e em
//! `servers-cache.json` (lida quando o Hub está fora; a UI marca como "desatualizada").
//!
//! O ping usa só os endereços desta lista: a UI manda ids, nunca host/porta.

use crate::error::{AppError, AppResult};
use crate::hub::ServerListing;
use crate::ping::PingTarget;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};

/// Teto do arquivo (o mesmo do corpo da lista no Hub).
const MAX_FILE: u64 = 4 * 1024 * 1024;

/// Conteúdo de `servers-cache.json`. `saved_at` em segundos Unix.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CachedServers {
    pub saved_at: i64,
    pub servers: Vec<ServerListing>,
}

pub struct ServerDirectory {
    path: PathBuf,
    known: Mutex<Vec<ServerListing>>,
}

impl ServerDirectory {
    pub fn new(path: PathBuf) -> Self {
        Self {
            path,
            known: Mutex::new(Vec::new()),
        }
    }

    fn known(&self) -> MutexGuard<'_, Vec<ServerListing>> {
        self.known.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Lista recém-vinda do Hub: passa a valer para o ping e é gravada no disco. Falha ao gravar só é registrada
    /// (a lista do Hub continua valendo).
    pub fn store_fresh(&self, servers: Vec<ServerListing>, now: i64) {
        let file = CachedServers {
            saved_at: now,
            servers,
        };
        if let Err(e) = save(&self.path, &file) {
            log::warn!("lista de servidores não gravada em disco: {}", e.code);
        }
        *self.known() = file.servers;
    }

    /// Última lista gravada (Hub fora). Também passa a valer para o ping. `None` se não houver ou estiver ilegível.
    pub fn cached(&self) -> Option<CachedServers> {
        let c = load(&self.path)?;
        *self.known() = c.servers.clone();
        Some(c)
    }

    /// Endereço de cada id (na ordem pedida); `None` para id que não está na lista conhecida.
    pub fn targets(&self, ids: &[String]) -> Vec<Option<PingTarget>> {
        let known = self.known();
        ids.iter()
            .map(|id| {
                known.iter().find(|s| &s.id == id).map(|s| PingTarget {
                    id: s.id.clone(),
                    host: s.host.clone(),
                    http_port: s.http_port,
                })
            })
            .collect()
    }
}

fn load(path: &Path) -> Option<CachedServers> {
    let meta = std::fs::metadata(path).ok()?;
    if !meta.is_file() || meta.len() > MAX_FILE {
        return None;
    }
    let bytes = std::fs::read(path).ok()?;
    match serde_json::from_slice(&bytes) {
        Ok(c) => Some(c),
        Err(_) => {
            log::warn!("servers-cache.json ilegível; ignorado");
            None
        }
    }
}

/// Gravação atômica: `.tmp` + sync + rename.
fn save(path: &Path, c: &CachedServers) -> AppResult<()> {
    use std::io::Write;
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| AppError::io(parent, &e))?;
    }
    let json = serde_json::to_vec(c).expect("lista sempre serializa");
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".tmp");
    let tmp = PathBuf::from(tmp);
    let written = (|| {
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
