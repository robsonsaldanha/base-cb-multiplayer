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
    known: Mutex<Option<CachedServers>>,
}

/// Teto de servidores guardados/mostrados (a lista real é muito menor).
pub const MAX_SERVERS: usize = 1000;
const MAX_ID: usize = 64;
const MAX_NAME: usize = 128;
const MAX_DESCRIPTION: usize = 1024;
const MAX_TAGS: usize = 16;
const MAX_TAG: usize = 32;
const MAX_LOCALE: usize = 16;

fn cut(s: &mut String, max: usize) {
    if let Some((i, _)) = s.char_indices().nth(max) {
        s.truncate(i);
    }
}

/// Lista do Hub (não confiável) pronta para a UI e o ping: id inválido (vazio, > 64, controle) sai; id repetido fica só
/// a primeira ocorrência (a UI usa o id como chave); no máximo [`MAX_SERVERS`]; textos cortados num tamanho sensato.
pub fn sanitize(servers: Vec<ServerListing>) -> Vec<ServerListing> {
    let mut seen = std::collections::HashSet::new();
    servers
        .into_iter()
        .filter(|s| !s.id.is_empty() && s.id.len() <= MAX_ID && !s.id.chars().any(char::is_control))
        .filter(|s| seen.insert(s.id.clone()))
        .take(MAX_SERVERS)
        .map(|mut s| {
            cut(&mut s.name, MAX_NAME);
            cut(&mut s.description, MAX_DESCRIPTION);
            cut(&mut s.locale, MAX_LOCALE);
            s.tags.truncate(MAX_TAGS);
            for t in &mut s.tags {
                cut(t, MAX_TAG);
            }
            s
        })
        .collect()
}

impl ServerDirectory {
    pub fn new(path: PathBuf) -> Self {
        Self {
            path,
            known: Mutex::new(None),
        }
    }

    fn known(&self) -> MutexGuard<'_, Option<CachedServers>> {
        self.known.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Lista recém-vinda do Hub: passa por [`sanitize`], vale para o ping e é gravada no disco. Falha ao gravar só é
    /// registrada (a lista do Hub continua valendo). Devolve a lista limpa (a que a UI recebe).
    pub fn store_fresh(&self, servers: Vec<ServerListing>, now: i64) -> Vec<ServerListing> {
        let file = CachedServers {
            saved_at: now,
            servers: sanitize(servers),
        };
        if let Err(e) = save(&self.path, &file) {
            log::warn!("lista de servidores não gravada em disco: {}", e.code);
        }
        let out = file.servers.clone();
        *self.known() = Some(file);
        out
    }

    /// Lista para o Hub fora: a mais nova entre a do disco e a da memória (se a gravação falhou, a da memória é mais
    /// recente). Também passa a valer para o ping. `None` se não houver nenhuma.
    pub fn cached(&self) -> Option<CachedServers> {
        let disk = load(&self.path).map(|c| CachedServers {
            saved_at: c.saved_at,
            servers: sanitize(c.servers),
        });
        let mut known = self.known();
        let best = match (known.take(), disk) {
            (Some(m), Some(d)) => Some(if d.saved_at > m.saved_at { d } else { m }),
            (m, d) => m.or(d),
        };
        known.clone_from(&best);
        best
    }

    /// Endereço de cada id (na ordem pedida); `None` para id que não está na lista conhecida.
    pub fn targets(&self, ids: &[String]) -> Vec<Option<PingTarget>> {
        let known = self.known();
        let list = known.as_ref().map_or(&[][..], |c| &c.servers[..]);
        ids.iter()
            .map(|id| {
                list.iter().find(|s| &s.id == id).map(|s| PingTarget {
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
