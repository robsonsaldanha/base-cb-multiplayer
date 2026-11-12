//! Localizar o Hogwarts Legacy: Steam (registro + `libraryfolders.vdf` + `appmanifest_990080.acf`), Epic (manifests
//! `.item` do launcher da Epic) ou pasta escolhida pelo usuário. Escrito do zero.
//!
//! Tudo que vem do disco (VDF, JSON da Epic) é só lido e validado: um caminho só vira [`GameInstall`] se passar em
//! [`validate_game_dir`] (executável + estrutura `Phoenix\Binaries\Win64`).

use crate::error::{AppError, AppResult};
use serde::Serialize;
use std::path::{Path, PathBuf};

/// AppID do Hogwarts Legacy na Steam.
pub const STEAM_APP_ID: &str = "990080";
/// Executável na raiz da instalação (o que a Steam/Epic abrem).
pub const GAME_EXE: &str = "HogwartsLegacy.exe";
/// Estrutura que confirma a pasta do jogo (e onde fica a pasta do mod).
pub const BINARIES: [&str; 3] = ["Phoenix", "Binaries", "Win64"];

const STEAM_USER_KEY: &str = r"Software\Valve\Steam";
const STEAM_MACHINE_KEY: &str = r"SOFTWARE\WOW6432Node\Valve\Steam";
/// Teto dos arquivos de texto lidos (VDF/ACF/`.item`): os reais têm poucos KB.
const MAX_TEXT: u64 = 1024 * 1024;
/// Teto de manifests da Epic olhados. Passou disso, nenhuma pasta não confiável é sugerida (alguém pode ter enchido a
/// pasta de manifests para esconder o verdadeiro).
pub const MAX_EPIC_ITEMS: usize = 1000;
const MAX_VDF_DEPTH: usize = 16;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Hive {
    CurrentUser,
    LocalMachine,
}

/// Leitura de valores `REG_SZ` (simulável nos testes).
pub trait Registry {
    fn read_string(&self, hive: Hive, key: &str, value: &str) -> Option<String>;
}

/// Registro do Windows (`RegGetValueW`, só leitura). Fora do Windows não acha nada.
#[derive(Debug, Default, Clone, Copy)]
pub struct SystemRegistry;

impl Registry for SystemRegistry {
    #[cfg(windows)]
    fn read_string(&self, hive: Hive, key: &str, value: &str) -> Option<String> {
        use std::os::windows::ffi::OsStringExt;
        use windows_sys::Win32::Foundation::ERROR_SUCCESS;
        use windows_sys::Win32::System::Registry::{
            RegGetValueW, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ,
        };
        let root = match hive {
            Hive::CurrentUser => HKEY_CURRENT_USER,
            Hive::LocalMachine => HKEY_LOCAL_MACHINE,
        };
        let wide = |s: &str| s.encode_utf16().chain(Some(0)).collect::<Vec<u16>>();
        let (k, v) = (wide(key), wide(value));
        let mut len: u32 = 0;
        // SAFETY: strings terminadas em zero vivas durante a chamada; primeira chamada só mede (buffer nulo).
        let r = unsafe {
            RegGetValueW(
                root,
                k.as_ptr(),
                v.as_ptr(),
                RRF_RT_REG_SZ,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                &mut len,
            )
        };
        if r != ERROR_SUCCESS || len == 0 || len > 64 * 1024 {
            return None;
        }
        let mut buf = vec![0u16; (len as usize).div_ceil(2)];
        // SAFETY: `buf` tem `len` bytes; a API escreve no máximo isso (e devolve erro se mudou de tamanho).
        let r = unsafe {
            RegGetValueW(
                root,
                k.as_ptr(),
                v.as_ptr(),
                RRF_RT_REG_SZ,
                std::ptr::null_mut(),
                buf.as_mut_ptr().cast(),
                &mut len,
            )
        };
        if r != ERROR_SUCCESS {
            return None;
        }
        let n = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
        let s = std::ffi::OsString::from_wide(&buf[..n]);
        s.into_string().ok().filter(|s| !s.trim().is_empty())
    }

    #[cfg(not(windows))]
    fn read_string(&self, _hive: Hive, _key: &str, _value: &str) -> Option<String> {
        None
    }
}

/// De onde a instalação veio (a UI mostra; o modo de abrir é o mesmo).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Store {
    Steam,
    Epic,
    Manual,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GameInstall {
    pub dir: PathBuf,
    pub exe: PathBuf,
    pub store: Store,
}

/// Pastas fora do registro onde procurar (injetáveis nos testes).
#[derive(Debug, Clone)]
pub struct Roots {
    /// `C:\ProgramData\Epic\EpicGamesLauncher\Data\Manifests`.
    pub epic_manifests: PathBuf,
}

impl Roots {
    /// Pastas reais do sistema.
    pub fn system() -> Self {
        // Pasta conhecida pela API (não pela variável de ambiente).
        #[cfg(windows)]
        let known = crate::paths::program_data();
        #[cfg(not(windows))]
        let known: Option<PathBuf> = None;
        let program_data = known.unwrap_or_else(|| PathBuf::from(r"C:\ProgramData"));
        Self {
            epic_manifests: program_data
                .join("Epic")
                .join("EpicGamesLauncher")
                .join("Data")
                .join("Manifests"),
        }
    }
}

fn game_invalid() -> AppError {
    AppError::new(
        "game_invalid",
        "Esta pasta não é do Hogwarts Legacy: faltam HogwartsLegacy.exe e Phoenix\\Binaries\\Win64.",
    )
}

/// Erro de quando nenhuma instalação foi achada (a UI oferece o seletor de pasta).
pub fn game_not_found() -> AppError {
    AppError::new(
        "game_not_found",
        "Hogwarts Legacy não encontrado. Escolha a pasta do jogo em Configurações.",
    )
}

/// Confere que `dir` é a pasta do jogo: caminho completo, `HogwartsLegacy.exe` (arquivo) e `Phoenix\Binaries\Win64`.
pub fn validate_game_dir(dir: &Path) -> AppResult<GameInstall> {
    if !dir.is_absolute() {
        return Err(game_invalid());
    }
    let exe = dir.join(GAME_EXE);
    let bins = BINARIES.iter().fold(dir.to_path_buf(), |p, s| p.join(s));
    if !exe.is_file() || !bins.is_dir() {
        return Err(game_invalid());
    }
    Ok(GameInstall {
        dir: dir.to_path_buf(),
        exe,
        store: Store::Manual,
    })
}

/// Qual verificação de confiança falhou num caminho achado pelo manifest da Epic (vai para o log).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum TrustCheck {
    /// O arquivo `.item` em `%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests`.
    Manifest,
    /// A pasta do jogo.
    GameDir,
    /// `HogwartsLegacy.exe`.
    Exe,
    /// `Phoenix\Binaries\Win64` (onde o mod é instalado e o jogo carrega DLLs).
    Binaries,
}

impl TrustCheck {
    /// Nome no log.
    pub fn label(self) -> &'static str {
        match self {
            Self::Manifest => "manifest",
            Self::GameDir => "pasta do jogo",
            Self::Exe => "executável",
            Self::Binaries => "Phoenix\\Binaries\\Win64",
        }
    }
}

fn labels(failed: &[TrustCheck]) -> String {
    failed
        .iter()
        .map(|f| f.label())
        .collect::<Vec<_>>()
        .join(", ")
}

/// Pasta válida do Hogwarts Legacy achada automaticamente (manifest da Epic) que **não** passou na verificação de
/// dono/permissões: outra conta do PC poderia tê-la plantado ou poderia alterá-la. Nunca é usada sozinha; a UI
/// mostra como sugestão e só vira `gameDir` depois de o usuário confirmar (`commands::use_detected_game`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GameCandidate {
    pub dir: PathBuf,
    pub store: Store,
    /// Verificações que falharam (só para o log; a UI não recebe).
    #[serde(skip)]
    pub failed: Vec<TrustCheck>,
}

/// Resultado da procura automática.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Detection {
    /// Instalação confiável: usada direto, como sempre.
    Trusted(GameInstall),
    /// Exatamente uma pasta achada, mas não confiável: só com confirmação do usuário.
    Candidate(GameCandidate),
    /// Mais de uma pasta não confiável (ou manifests demais para olhar todos): qualquer uma pode ter sido plantada, então
    /// nenhuma é sugerida; o usuário escolhe a pasta pelo seletor. `candidates` = pastas distintas vistas.
    Ambiguous {
        candidates: usize,
        flooded: bool,
    },
    NotFound,
}

/// Procura na Steam e depois na Epic; `None` se nada válido **e confiável** for achado (ver [`detect`]).
pub fn locate(reg: &dyn Registry, roots: &Roots) -> Option<GameInstall> {
    locate_with(reg, roots, &SystemTrust)
}

/// Como [`locate`], com a verificação de dono/permissões injetável (testes).
pub fn locate_with(reg: &dyn Registry, roots: &Roots, trust: &dyn Trust) -> Option<GameInstall> {
    match detect_with(reg, roots, trust) {
        Detection::Trusted(g) => Some(g),
        Detection::Candidate(_) | Detection::Ambiguous { .. } | Detection::NotFound => None,
    }
}

/// Procura na Steam e depois na Epic: instalação confiável, uma candidata (Epic não confiável), várias (ambíguo) ou
/// nada.
pub fn detect(reg: &dyn Registry, roots: &Roots) -> Detection {
    detect_with(reg, roots, &SystemTrust)
}

/// Como [`detect`], com a verificação de dono/permissões injetável (testes).
pub fn detect_with(reg: &dyn Registry, roots: &Roots, trust: &dyn Trust) -> Detection {
    let d = match locate_steam(reg) {
        Some(g) => Detection::Trusted(g),
        None => locate_epic(&roots.epic_manifests, trust),
    };
    match &d {
        Detection::Trusted(g) => log::info!("Hogwarts Legacy encontrado ({:?})", g.store),
        Detection::Candidate(c) => log::info!(
            "Hogwarts Legacy achado ({:?}) em {}, mas não usado sozinho (não confiável: {}); aguardando confirmação",
            c.store,
            c.dir.display(),
            labels(&c.failed)
        ),
        Detection::Ambiguous {
            candidates,
            flooded,
        } => log::warn!(
            "Hogwarts Legacy ambíguo nos manifests da Epic ({candidates} pastas não confiáveis distintas{}); nenhuma \
             sugerida, o usuário precisa escolher a pasta",
            if *flooded {
                ", manifests demais para olhar todos"
            } else {
                ""
            }
        ),
        Detection::NotFound => {}
    }
    d
}

/// Se um caminho achado automaticamente é confiável: dono e quem pode escrever nele.
///
/// `%ProgramData%` deixa qualquer usuário local criar arquivos e pastas, então um `.item` da Epic (e a pasta para
/// onde ele aponta) podem ter sido plantados por outra conta do PC para o launcher abrir o executável dela.
/// Na prática o launcher da Epic também deixa manifests e pasta do jogo graváveis por outras contas, por isso o que
/// falha aqui vira [`GameCandidate`] (sugestão que o usuário confirma), nunca instalação usada sozinha.
pub trait Trust {
    fn trusted(&self, path: &Path) -> bool;
}

/// Regra real: dono SYSTEM, Administradores, TrustedInstaller ou o usuário atual, e nenhuma outra conta com direito
/// de escrita (ver [`crate::launch::acl::trusted`]). Fora do Windows aceita tudo.
#[derive(Debug, Default, Clone, Copy)]
pub struct SystemTrust;

impl Trust for SystemTrust {
    #[cfg(windows)]
    fn trusted(&self, path: &Path) -> bool {
        use crate::launch::acl;
        match acl::current_user_sid().and_then(|me| acl::read_security(path).map(|sec| (sec, me))) {
            Ok((sec, me)) => acl::trusted(&sec, &me),
            Err(e) => {
                log::warn!(
                    "permissões ilegíveis num caminho achado automaticamente ({})",
                    e.code
                );
                false
            }
        }
    }

    #[cfg(not(windows))]
    fn trusted(&self, _path: &Path) -> bool {
        true
    }
}

fn with_store(g: GameInstall, store: Store) -> GameInstall {
    GameInstall { store, ..g }
}

/// Lê um arquivo de texto de até [`MAX_TEXT`]. Abre uma vez e limita a leitura pelo próprio handle: trocar o arquivo
/// por um enorme depois da checagem de tamanho não faz o launcher ler tudo.
fn read_text(path: &Path) -> Option<String> {
    use std::io::Read;
    let f = std::fs::File::open(path).ok()?;
    let md = f.metadata().ok()?;
    if !md.is_file() || md.len() > MAX_TEXT {
        return None;
    }
    let mut bytes = Vec::new();
    f.take(MAX_TEXT + 1).read_to_end(&mut bytes).ok()?;
    if bytes.len() as u64 > MAX_TEXT {
        return None;
    }
    Some(String::from_utf8_lossy(&bytes).into_owned())
}

/// Caractere que não deve aparecer num caminho mostrado ao usuário: controle, ou formatação/direção do Unicode (ex.
/// U+202E), que faz um caminho parecer outro na tela.
fn deceptive_char(c: char) -> bool {
    c.is_control()
        || matches!(
            c,
            '\u{00AD}'
                | '\u{061C}'
                | '\u{180E}'
                | '\u{200B}'..='\u{200F}'
                | '\u{202A}'..='\u{202E}'
                | '\u{2060}'..='\u{2064}'
                | '\u{2066}'..='\u{206F}'
                | '\u{FEFF}'
                | '\u{FFF9}'..='\u{FFFB}'
        )
}

/// O Steam grava `SteamPath` como `c:/program files (x86)/steam`.
fn native_path(s: &str) -> PathBuf {
    if cfg!(windows) {
        PathBuf::from(s.trim().replace('/', "\\"))
    } else {
        PathBuf::from(s.trim())
    }
}

/// Mesmo caminho (no Windows sem diferenciar maiúsculas nem barra no fim).
pub fn same_path(a: &Path, b: &Path) -> bool {
    if cfg!(windows) {
        let n = |p: &Path| {
            p.to_string_lossy()
                .trim_end_matches(['\\', '/'])
                .to_lowercase()
        };
        n(a) == n(b)
    } else {
        a == b
    }
}

fn locate_steam(reg: &dyn Registry) -> Option<GameInstall> {
    let mut roots: Vec<PathBuf> = Vec::new();
    for (hive, key, value) in [
        (Hive::CurrentUser, STEAM_USER_KEY, "SteamPath"),
        (Hive::LocalMachine, STEAM_MACHINE_KEY, "InstallPath"),
    ] {
        if let Some(p) = reg.read_string(hive, key, value) {
            let p = native_path(&p);
            if p.is_absolute() && !roots.iter().any(|r| same_path(r, &p)) {
                roots.push(p);
            }
        }
    }
    let mut seen: Vec<PathBuf> = Vec::new();
    for root in roots {
        for lib in steam_libraries(&root) {
            if seen.iter().any(|s| same_path(s, &lib)) {
                continue;
            }
            seen.push(lib.clone());
            if let Some(g) = steam_game_in(&lib) {
                return Some(g);
            }
        }
    }
    None
}

/// Bibliotecas da Steam: a própria pasta do Steam e as de `steamapps\libraryfolders.vdf` (formato novo com
/// `"path"` e antigo com `"1" "D:\\..."`), sem repetir.
pub fn steam_libraries(steam_root: &Path) -> Vec<PathBuf> {
    let mut libs = vec![steam_root.to_path_buf()];
    let Some(text) = read_text(&steam_root.join("steamapps").join("libraryfolders.vdf")) else {
        return libs;
    };
    let Some(vdf) = parse_vdf(&text) else {
        return libs;
    };
    let Some(Vdf::Obj(top)) = vdf_child(&vdf, "libraryfolders") else {
        return libs;
    };
    for (key, v) in top {
        if !key.bytes().all(|b| b.is_ascii_digit()) {
            continue;
        }
        let path = match v {
            Vdf::Str(s) => Some(s.as_str()),
            Vdf::Obj(_) => match vdf_child(v, "path") {
                Some(Vdf::Str(s)) => Some(s.as_str()),
                _ => None,
            },
        };
        if let Some(p) = path.map(native_path).filter(|p| p.is_absolute()) {
            if !libs.iter().any(|l| same_path(l, &p)) {
                libs.push(p);
            }
        }
    }
    libs
}

fn steam_game_in(lib: &Path) -> Option<GameInstall> {
    let apps = lib.join("steamapps");
    let text = read_text(&apps.join(format!("appmanifest_{STEAM_APP_ID}.acf")))?;
    let vdf = parse_vdf(&text)?;
    let dir = vdf_get(&vdf, &["AppState", "installdir"])?;
    // Um nome de pasta só, dentro de `common`.
    let ok = !dir.is_empty()
        && dir != "."
        && dir != ".."
        && !dir.contains(['/', '\\', ':'])
        && !dir.chars().any(char::is_control);
    if !ok {
        return None;
    }
    validate_game_dir(&apps.join("common").join(dir))
        .ok()
        .map(|g| with_store(g, Store::Steam))
}

/// Manifests da Epic. O primeiro confiável (em ordem de nome) vence. Sem nenhum confiável, as pastas válidas mas não
/// confiáveis são juntadas sem repetir: **uma** vira candidata; duas ou mais, ou mais de [`MAX_EPIC_ITEMS`] manifests,
/// viram [`Detection::Ambiguous`] (outra conta pode ter plantado um `.item` para tomar o lugar do verdadeiro, então
/// nenhuma é sugerida). O texto de um `.item` não confiável é só lido (até [`MAX_TEXT`]) e interpretado como JSON.
fn locate_epic(manifests: &Path, trust: &dyn Trust) -> Detection {
    let Ok(rd) = std::fs::read_dir(manifests) else {
        return Detection::NotFound;
    };
    let mut items: Vec<PathBuf> = rd
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.extension()
                .is_some_and(|e| e.eq_ignore_ascii_case("item"))
        })
        .take(MAX_EPIC_ITEMS + 1)
        .collect();
    let flooded = items.len() > MAX_EPIC_ITEMS;
    items.truncate(MAX_EPIC_ITEMS);
    items.sort();
    let mut candidates: Vec<GameCandidate> = Vec::new();
    for item in items {
        let Some(g) = epic_item_game(&item) else {
            continue;
        };
        // O manifest, a pasta do jogo, o executável e a pasta onde o mod é instalado (e o jogo carrega DLLs).
        let bins = BINARIES.iter().fold(g.dir.clone(), |p, s| p.join(s));
        let failed: Vec<TrustCheck> = [
            (TrustCheck::Manifest, item.as_path()),
            (TrustCheck::GameDir, g.dir.as_path()),
            (TrustCheck::Exe, g.exe.as_path()),
            (TrustCheck::Binaries, bins.as_path()),
        ]
        .into_iter()
        .filter(|(_, p)| !trust.trusted(p))
        .map(|(c, _)| c)
        .collect();
        if failed.is_empty() {
            return Detection::Trusted(with_store(g, Store::Epic));
        }
        log::warn!(
            "manifest da Epic {} (Hogwarts Legacy) com dono ou permissões não confiáveis em: {}",
            item.file_name()
                .map_or_else(String::new, |n| n.to_string_lossy().into_owned()),
            labels(&failed)
        );
        if !candidates.iter().any(|c| same_path(&c.dir, &g.dir)) {
            candidates.push(GameCandidate {
                dir: g.dir,
                store: Store::Epic,
                failed,
            });
        }
    }
    match (candidates.len(), flooded) {
        (0, false) => Detection::NotFound,
        (1, false) => Detection::Candidate(candidates.remove(0)),
        (n, flooded) => Detection::Ambiguous {
            candidates: n,
            flooded,
        },
    }
}

/// Instalação para onde um `.item` da Epic aponta, se ele é do Hogwarts Legacy e a pasta passa em
/// [`validate_game_dir`]. Sem verificação de confiança (feita por quem chama).
fn epic_item_game(item: &Path) -> Option<GameInstall> {
    let text = read_text(item)?;
    let serde_json::Value::Object(v) = serde_json::from_str::<serde_json::Value>(&text).ok()?
    else {
        return None;
    };
    let is_hl = ["DisplayName", "AppName"].iter().any(|k| {
        v.get(*k)
            .and_then(|x| x.as_str())
            .is_some_and(is_hogwarts_name)
    });
    if !is_hl {
        return None;
    }
    let loc = v.get("InstallLocation").and_then(|x| x.as_str())?;
    // Caminho que vai ser mostrado ao usuário: nada de caracteres que o disfarcem.
    if loc.chars().any(deceptive_char) {
        return None;
    }
    validate_game_dir(&native_path(loc)).ok()
}

/// "Hogwarts Legacy" ignorando caixa, espaços e pontuação.
fn is_hogwarts_name(s: &str) -> bool {
    let n: String = s
        .chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .map(|c| c.to_ascii_lowercase())
        .collect();
    n == "hogwartslegacy"
}

// ----- VDF (KeyValues do Valve, só texto) -----

/// Valor de um VDF: texto ou objeto (lista de pares, na ordem do arquivo).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Vdf {
    Str(String),
    Obj(Vec<(String, Vdf)>),
}

enum Tok {
    Str(String),
    Open,
    Close,
}

fn tokenize(text: &str) -> Option<Vec<Tok>> {
    let mut out = Vec::new();
    let mut it = text.chars().peekable();
    while let Some(c) = it.next() {
        match c {
            '{' => out.push(Tok::Open),
            '}' => out.push(Tok::Close),
            '"' => {
                let mut s = String::new();
                loop {
                    match it.next()? {
                        '"' => break,
                        '\\' => match it.next()? {
                            'n' => s.push('\n'),
                            't' => s.push('\t'),
                            other => s.push(other),
                        },
                        ch => s.push(ch),
                    }
                }
                out.push(Tok::Str(s));
            }
            '/' if it.peek() == Some(&'/') => {
                for ch in it.by_ref() {
                    if ch == '\n' {
                        break;
                    }
                }
            }
            c if c.is_whitespace() => {}
            // Token sem aspas (raro): até espaço ou chave.
            _ => {
                let mut s = String::from(c);
                while let Some(&n) = it.peek() {
                    if n.is_whitespace() || n == '{' || n == '}' || n == '"' {
                        break;
                    }
                    s.push(n);
                    it.next();
                }
                out.push(Tok::Str(s));
            }
        }
    }
    Some(out)
}

/// Interpreta um arquivo VDF inteiro como objeto raiz. Texto malformado → `None` (nunca pânico).
pub fn parse_vdf(text: &str) -> Option<Vdf> {
    let toks = tokenize(text)?;
    let mut pos = 0;
    let obj = parse_pairs(&toks, &mut pos, 0)?;
    if pos != toks.len() {
        return None;
    }
    Some(Vdf::Obj(obj))
}

fn parse_pairs(toks: &[Tok], pos: &mut usize, depth: usize) -> Option<Vec<(String, Vdf)>> {
    if depth > MAX_VDF_DEPTH {
        return None;
    }
    let mut out = Vec::new();
    while *pos < toks.len() {
        let key = match &toks[*pos] {
            Tok::Str(s) => s.clone(),
            Tok::Close => return (depth > 0).then_some(out),
            Tok::Open => return None,
        };
        *pos += 1;
        match toks.get(*pos)? {
            Tok::Str(v) => {
                out.push((key, Vdf::Str(v.clone())));
                *pos += 1;
            }
            Tok::Open => {
                *pos += 1;
                let inner = parse_pairs(toks, pos, depth + 1)?;
                match toks.get(*pos)? {
                    Tok::Close => *pos += 1,
                    _ => return None,
                }
                out.push((key, Vdf::Obj(inner)));
            }
            Tok::Close => return None,
        }
    }
    (depth == 0).then_some(out)
}

fn vdf_child<'a>(v: &'a Vdf, key: &str) -> Option<&'a Vdf> {
    match v {
        Vdf::Obj(pairs) => pairs
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(key))
            .map(|(_, v)| v),
        Vdf::Str(_) => None,
    }
}

/// Texto no caminho de chaves (sem diferenciar maiúsculas, como o Steam).
pub fn vdf_get(v: &Vdf, keys: &[&str]) -> Option<String> {
    let mut cur = v;
    for k in keys {
        cur = vdf_child(cur, k)?;
    }
    match cur {
        Vdf::Str(s) => Some(s.clone()),
        Vdf::Obj(_) => None,
    }
}

// ----- jogo aberto -----

/// Nomes dos executáveis em execução (injetável nos testes).
pub trait Processes {
    fn names(&self) -> Vec<String>;
}

/// Processos reais do sistema (`CreateToolhelp32Snapshot`).
pub struct SystemProcesses;

impl Processes for SystemProcesses {
    #[cfg(windows)]
    fn names(&self) -> Vec<String> {
        use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
        use windows_sys::Win32::System::Diagnostics::ToolHelp::{
            CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W,
            TH32CS_SNAPPROCESS,
        };
        let mut out = Vec::new();
        // SAFETY: snapshot fechado no fim; `PROCESSENTRY32W` com `dwSize` preenchido, como a API pede.
        unsafe {
            let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if snap == INVALID_HANDLE_VALUE || snap.is_null() {
                log::warn!("lista de processos indisponível");
                return out;
            }
            let mut e = PROCESSENTRY32W {
                dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32,
                ..Default::default()
            };
            let mut ok = Process32FirstW(snap, &mut e);
            while ok != 0 {
                let len = e
                    .szExeFile
                    .iter()
                    .position(|c| *c == 0)
                    .unwrap_or(e.szExeFile.len());
                out.push(String::from_utf16_lossy(&e.szExeFile[..len]));
                ok = Process32NextW(snap, &mut e);
            }
            CloseHandle(snap);
        }
        out
    }

    #[cfg(not(windows))]
    fn names(&self) -> Vec<String> {
        Vec::new()
    }
}

/// Algum processo se chama `HogwartsLegacy.exe` (sem diferenciar maiúsculas).
pub fn game_running_with(p: &dyn Processes) -> bool {
    p.names().iter().any(|n| n.eq_ignore_ascii_case(GAME_EXE))
}

/// [`game_running_with`] com os processos reais.
pub fn game_running() -> bool {
    game_running_with(&SystemProcesses)
}

/// Jogo aberto: Jogar e Remover recusam.
pub fn game_running_error() -> AppError {
    AppError::new(
        "game_running",
        "O Hogwarts Legacy está aberto. Feche o jogo e tente de novo.",
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nome_do_jogo_tolerante() {
        for ok in [
            "Hogwarts Legacy",
            "HOGWARTS LEGACY",
            "Hogwarts-Legacy",
            "hogwartslegacy",
        ] {
            assert!(is_hogwarts_name(ok), "{ok}");
        }
        for bad in ["Hogwarts", "Hogwarts Legacy 2", ""] {
            assert!(!is_hogwarts_name(bad), "{bad}");
        }
    }

    #[test]
    fn vdf_profundo_demais_e_recusado() {
        let deep = "\"a\" {".repeat(40) + &"}".repeat(40);
        assert_eq!(parse_vdf(&deep), None);
    }
}
