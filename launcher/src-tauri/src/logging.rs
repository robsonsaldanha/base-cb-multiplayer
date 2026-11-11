//! Log em arquivo: `%LOCALAPPDATA%\CBMultiplayer\logs\launcher.log`, com rotação por tamanho
//! ([`MAX_BYTES`] × [`KEEP`] arquivos: `launcher.log.1` é o mais novo dos antigos).
//!
//! **Nada de segredos no log.** O código do launcher já não registra tokens, tickets nem códigos OAuth; mesmo assim
//! toda linha (inclusive de dependências) passa por [`redact`] antes de ir para o disco: tokens do Hub (`at_`, `rt_`,
//! `sk_`), valores de `code=`/`state=`/`ticket=`/`token=`/`code_verifier=`…, `Bearer …`, tickets (`xxx.yyy` em
//! base64url) e sequências longas que parecem aleatórias viram `***`.
//!
//! Níveis: o próprio launcher em `info` (`debug` em build de debug); dependências só `warn` e acima.

use std::borrow::Cow;
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

/// Tamanho máximo de `launcher.log` antes de girar.
pub const MAX_BYTES: u64 = 2 * 1024 * 1024;
/// Arquivos antigos mantidos (`launcher.log.1` … `.N`).
pub const KEEP: usize = 5;
/// Nome do arquivo atual.
pub const FILE_NAME: &str = "launcher.log";
/// Linha mais longa gravada (o resto é cortado).
const MAX_LINE: usize = 4096;
const MASK: &str = "***";

/// Chaves cujo valor nunca vai para o log (`chave=valor`, `chave: valor`, `"chave":"valor"`), sem diferenciar
/// maiúsculas. `code` cobre o código OAuth e o código de login do Hub.
const SECRET_KEYS: [&str; 16] = [
    "code",
    "state",
    "ticket",
    "token",
    "access_token",
    "refresh_token",
    "accesstoken",
    "refreshtoken",
    "code_verifier",
    "codeverifier",
    "code_challenge",
    "codechallenge",
    "authorization",
    "password",
    "secret",
    "key",
];
/// Prefixos dos tokens do Hub (access, refresh, chave de servidor).
const TOKEN_PREFIXES: [&str; 3] = ["at_", "rt_", "sk_"];

fn is_b64url(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_' || c == '-'
}

/// Valor de uma chave secreta: até espaço, aspas, `&`, `,`, `;` ou fecha-chaves/colchetes/parênteses.
fn value_end(s: &str, from: usize) -> usize {
    s[from..]
        .find(|c: char| c.is_whitespace() || "\"'&,;)}]<>".contains(c))
        .map_or(s.len(), |i| from + i)
}

/// Mascara o valor depois de `chave=`/`chave:`/`"chave":"`.
fn redact_keys(s: &str) -> String {
    let lower = s.to_ascii_lowercase();
    let bytes = s.as_bytes();
    let mut out = String::with_capacity(s.len());
    let mut i = 0;
    'outer: while i < s.len() {
        for key in SECRET_KEYS {
            if !lower[i..].starts_with(key) {
                continue;
            }
            // Começo de palavra: antes não pode ser letra, dígito ou `_`.
            if i > 0 && (bytes[i - 1].is_ascii_alphanumeric() || bytes[i - 1] == b'_') {
                continue;
            }
            let mut j = i + key.len();
            if j < s.len() && (bytes[j].is_ascii_alphanumeric() || bytes[j] == b'_') {
                continue;
            }
            // `"chave"` → pula a aspa de fechamento.
            if j < s.len() && (bytes[j] == b'"' || bytes[j] == b'\'') {
                j += 1;
            }
            while j < s.len() && bytes[j] == b' ' {
                j += 1;
            }
            if j >= s.len() || (bytes[j] != b'=' && bytes[j] != b':') {
                continue;
            }
            j += 1;
            while j < s.len() && (bytes[j] == b' ' || bytes[j] == b'"' || bytes[j] == b'\'') {
                j += 1;
            }
            let end = value_end(s, j);
            if end == j {
                continue;
            }
            out.push_str(&s[i..j]);
            out.push_str(MASK);
            i = end;
            continue 'outer;
        }
        let c = s[i..].chars().next().unwrap();
        out.push(c);
        i += c.len_utf8();
    }
    out
}

/// `Bearer <x>` → `Bearer ***`.
fn redact_bearer(s: &str) -> String {
    let lower = s.to_ascii_lowercase();
    let mut out = String::with_capacity(s.len());
    let mut i = 0;
    while let Some(pos) = lower[i..].find("bearer ") {
        let start = i + pos + "bearer ".len();
        let end = value_end(s, start);
        out.push_str(&s[i..start]);
        if end > start {
            out.push_str(MASK);
        }
        i = end.max(start);
    }
    out.push_str(&s[i..]);
    out
}

/// Pedaço base64url que parece aleatório: longo, com letras e dígitos e não só hexadecimal (hashes sha256 em hex
/// continuam legíveis).
fn looks_random(part: &str) -> bool {
    part.len() >= 32
        && part.chars().all(is_b64url)
        && part.chars().any(|c| c.is_ascii_digit())
        && part.chars().any(|c| c.is_ascii_alphabetic())
        && !part.chars().all(|c| c.is_ascii_hexdigit())
}

/// Tokens do Hub, tickets (`base64url.base64url[.base64url]`) e sequências aleatórias longas.
fn redact_words(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while !rest.is_empty() {
        let word_len = rest
            .find(|c: char| !(is_b64url(c) || c == '.'))
            .unwrap_or(rest.len());
        if word_len == 0 {
            let c = rest.chars().next().unwrap();
            out.push(c);
            rest = &rest[c.len_utf8()..];
            continue;
        }
        let word = &rest[..word_len];
        rest = &rest[word_len..];
        let parts: Vec<&str> = word.split('.').collect();
        let ticket = parts.len() >= 2
            && parts
                .iter()
                .filter(|p| p.len() >= 16 && p.chars().all(is_b64url))
                .count()
                >= 2;
        if ticket {
            out.push_str(MASK);
            continue;
        }
        let masked: Vec<Cow<str>> = parts
            .iter()
            .map(|p| {
                if let Some(prefix) = TOKEN_PREFIXES
                    .iter()
                    .find(|pre| p.len() >= pre.len() + 8 && p.starts_with(**pre))
                {
                    Cow::Owned(format!("{prefix}{MASK}"))
                } else if looks_random(p) {
                    Cow::Borrowed(MASK)
                } else {
                    Cow::Borrowed(*p)
                }
            })
            .collect();
        out.push_str(&masked.join("."));
    }
    out
}

/// Remove segredos de uma linha de log (ver o topo do módulo). Também troca quebras de linha por `\n` literal:
/// uma mensagem não forja outra linha.
pub fn redact(line: &str) -> String {
    let flat = line.replace('\r', "\\r").replace('\n', "\\n");
    redact_words(&redact_bearer(&redact_keys(&flat)))
}

// ----- data/hora UTC sem dependência -----

/// `AAAA-MM-DDTHH:MM:SSZ` (UTC).
pub fn utc_timestamp(t: SystemTime) -> String {
    let secs = t.duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs()) as i64;
    let (days, rem) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    // Algoritmo "civil_from_days" (Howard Hinnant).
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!(
        "{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}Z",
        rem / 3600,
        (rem % 3600) / 60,
        rem % 60
    )
}

// ----- arquivo com rotação -----

struct Sink {
    file: Option<File>,
    size: u64,
    /// O arquivo atual não pôde ser renomeado (outro processo o segura): não tenta girar de novo nesta sessão, para
    /// não apagar o histórico girando a cadeia a cada linha.
    rotation_blocked: bool,
}

/// Logger em arquivo com rotação. Use [`init`] no app; os testes criam o seu com [`FileLogger::open`].
pub struct FileLogger {
    dir: PathBuf,
    max_bytes: u64,
    keep: usize,
    own_level: log::LevelFilter,
    deps_level: log::LevelFilter,
    sink: Mutex<Sink>,
}

impl FileLogger {
    /// Abre (ou cria) `dir\launcher.log`. Gira na hora se o arquivo já passou do limite.
    pub fn open(dir: &Path, max_bytes: u64, keep: usize) -> std::io::Result<Self> {
        std::fs::create_dir_all(dir)?;
        let logger = Self {
            dir: dir.to_path_buf(),
            max_bytes,
            keep,
            own_level: if cfg!(debug_assertions) {
                log::LevelFilter::Debug
            } else {
                log::LevelFilter::Info
            },
            deps_level: log::LevelFilter::Warn,
            sink: Mutex::new(Sink {
                file: None,
                size: 0,
                rotation_blocked: false,
            }),
        };
        {
            let mut sink = logger.sink.lock().unwrap_or_else(|p| p.into_inner());
            logger.reopen(&mut sink)?;
            if sink.size >= max_bytes {
                logger.rotate(&mut sink);
            }
        }
        Ok(logger)
    }

    pub fn path(&self) -> PathBuf {
        self.dir.join(FILE_NAME)
    }

    fn rotated(&self, n: usize) -> PathBuf {
        self.dir.join(format!("{FILE_NAME}.{n}"))
    }

    fn reopen(&self, sink: &mut Sink) -> std::io::Result<()> {
        let f = OpenOptions::new()
            .create(true)
            .append(true)
            .open(self.path())?;
        sink.size = f.metadata().map_or(0, |m| m.len());
        sink.file = Some(f);
        Ok(())
    }

    /// `launcher.log` → `.1` → `.2` … (o mais velho além de `keep` some). Primeiro tira o arquivo atual do caminho
    /// (`.rotating`); só se isso der certo a cadeia anda. Se não der (arquivo preso por outro processo), os antigos
    /// ficam intactos, o atual continua sendo usado e a rotação para nesta sessão. Nunca derruba o app.
    fn rotate(&self, sink: &mut Sink) {
        sink.file = None; // fecha antes de renomear (Windows)
        let pending = self.dir.join(format!("{FILE_NAME}.rotating"));
        let moved = if self.keep == 0 {
            std::fs::remove_file(self.path()).is_ok()
        } else {
            std::fs::rename(self.path(), &pending).is_ok()
        };
        if moved && self.keep > 0 {
            let _ = std::fs::remove_file(self.rotated(self.keep));
            for n in (1..self.keep).rev() {
                let _ = std::fs::rename(self.rotated(n), self.rotated(n + 1));
            }
            let _ = std::fs::rename(&pending, self.rotated(1));
        }
        sink.rotation_blocked = !moved;
        let _ = self.reopen(sink);
    }

    fn level_for(&self, target: &str) -> log::LevelFilter {
        if target.starts_with("cbmp_launcher") {
            self.own_level
        } else {
            self.deps_level
        }
    }

    /// Grava uma linha já formatada (redigida aqui).
    pub fn write_line(&self, level: log::Level, target: &str, message: &str) {
        let mut text = redact(message);
        if text.len() > MAX_LINE {
            let mut cut = MAX_LINE;
            while !text.is_char_boundary(cut) {
                cut -= 1;
            }
            text.truncate(cut);
            text.push('…');
        }
        let line = format!(
            "{} {:<5} {}: {}\n",
            utc_timestamp(SystemTime::now()),
            level,
            target,
            text
        );
        let mut sink = self.sink.lock().unwrap_or_else(|p| p.into_inner());
        if !sink.rotation_blocked && sink.size > 0 && sink.size + line.len() as u64 > self.max_bytes
        {
            self.rotate(&mut sink);
        }
        if let Some(f) = sink.file.as_mut() {
            if f.write_all(line.as_bytes()).is_ok() {
                sink.size += line.len() as u64;
            }
        }
        if cfg!(debug_assertions) {
            eprint!("{line}");
        }
    }
}

impl log::Log for FileLogger {
    fn enabled(&self, metadata: &log::Metadata) -> bool {
        metadata.level() <= self.level_for(metadata.target())
    }

    fn log(&self, record: &log::Record) {
        if self.enabled(record.metadata()) {
            self.write_line(record.level(), record.target(), &record.args().to_string());
        }
    }

    fn flush(&self) {
        let mut sink = self.sink.lock().unwrap_or_else(|p| p.into_inner());
        if let Some(f) = sink.file.as_mut() {
            let _ = f.flush();
        }
    }
}

/// Instala o logger global em `dir` e registra pânicos nele. Sem pasta gravável o app segue sem log.
pub fn init(dir: &Path) {
    let Ok(logger) = FileLogger::open(dir, MAX_BYTES, KEEP) else {
        return;
    };
    let max = logger.own_level.max(logger.deps_level);
    // Vive até o fim do processo (o logger global é `&'static`).
    if log::set_logger(Box::leak(Box::new(logger))).is_ok() {
        log::set_max_level(max);
        let prev = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            log::error!("pânico: {info}");
            log::logger().flush();
            prev(info);
        }));
        log::info!(
            "CB Multiplayer iniciado{}",
            if crate::profile::LOCAL_TEST {
                " (Teste local)"
            } else if crate::profile::ONLINE_TEST {
                " (Teste)"
            } else {
                ""
            }
        );
    }
}
