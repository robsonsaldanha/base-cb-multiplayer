//! Instalação com permissão de administrador (uma vez) para pastas do jogo em que o launcher não grava (ex.: Epic em
//! `C:\Program Files`).
//!
//! O launcher normal, ao esbarrar em `mod_needs_admin`, roda **o próprio executável** com o verbo `runas` (UAC) e
//! `--mod-admin-setup` ([`request`]). O processo elevado ([`entry`], chamado em `main.rs` antes do Tauri) faz o mínimo
//! e sai:
//!
//! - **Sem janela, sem instância única, sem rede, sem log em arquivo** (o log do launcher fica numa pasta do jogador;
//!   gravar nela como administrador seria um alvo de link). Não aceita ticket, `cbmp://`, nem nome de arquivo solto: só
//!   `--mod-admin-setup --game-dir <p> --user-sid <S-1-5-21-…> --package-dir <p> [--confirm-conflict]
//!   [--replace-loader <sha256>]` ([`parse_args`]), cada um uma vez, caminhos completos com letra de unidade.
//! - **Revalida tudo** ([`validate`]): pasta do jogo de verdade (`HogwartsLegacy.exe` + `Phoenix\Binaries\Win64`), fora de
//!   `%SystemRoot%`, nenhum componente do caminho até `Win64` (nem do pacote) é ponto de reanálise (link/junção), SID de
//!   usuário (`S-1-5-21-a-b-c-d`). Depois **segura** cada pasta do caminho aberta sem compartilhar a exclusão
//!   ([`hold_path`]): ninguém renomeia nem troca uma pasta por junção enquanto o processo trabalha.
//! - **Confere o pacote de novo** com a chave embutida, pelo mesmo handle que é lido como zip (aberto sem permitir
//!   gravação/exclusão por outros): assinatura, sha256 assinado, zip inteiro e `cbmp-mod.json`
//!   ([`crate::modpkg::verify_package`]). Nada do launcher sem elevação é confiado além de caminhos revalidados; o
//!   `--replace-loader` só diz qual `dwmapi.dll` existente é um carregador nosso anterior (ver "Limites" abaixo).
//! - **Grava só** `Win64\cbmp\` (criada, com ACE de Modificar herdável para o SID — [`crate::launch::acl::grant_modify`]),
//!   `Win64\dwmapi.dll` (o carregador lido do zip assinado) e `Win64\override.txt`, pelas regras de
//!   [`crate::modact::activate_from`]: ausente → temporário novo + rename sem substituir; nosso anterior → substitui;
//!   estranho → só com `--confirm-conflict`, **movido** para `Win64\cbmp-backup\` com o manifesto
//!   `cbmp-backup\cbmp-backup.json` gravado antes e depois de mover (o mesmo que o launcher normal adota). Não instala o
//!   pacote em `cbmp\atual`, não grava ponteiro, não toca no registro do launcher. Conflito sem confirmação ou UE4SS
//!   antigo → nada é gravado.
//! - Código de saída ([`exit_code`]): 0 ok; 2 argumentos; 3 assinatura; 4 conflito; 5 outros.
//!
//! Limites: `--replace-loader` é uma escolha do launcher sem elevação (o hash de um carregador nosso anterior vem do
//! registro dele). Quem já roda como o jogador poderia passar o hash de outro `dwmapi.dll` daquela pasta: o arquivo
//! seria substituído pelo nosso carregador assinado em vez de ir para `cbmp-backup\` — o mesmo poder do
//! `--confirm-conflict`, que já tira esse arquivo do lugar, sem guardar a cópia. Nada fora da pasta do jogo validada.

use crate::error::{AppError, AppResult};
use crate::game::{validate_game_dir, GameInstall};
use crate::modact::{self, Activation, LoaderSource};
use crate::modpkg::{self, Installed, LAYOUT};
use std::ffi::OsString;
use std::path::{Component, Path, PathBuf, Prefix};
use std::time::Duration;

/// Primeiro argumento do modo elevado.
pub const FLAG: &str = "--mod-admin-setup";
/// Quanto o launcher espera o processo elevado (com o UAC aberto).
pub const WAIT: Duration = Duration::from_secs(120);

const GAME_DIR: &str = "--game-dir";
const USER_SID: &str = "--user-sid";
const PACKAGE_DIR: &str = "--package-dir";
const CONFIRM: &str = "--confirm-conflict";
const REPLACE: &str = "--replace-loader";

/// Argumentos do modo elevado.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AdminArgs {
    pub game_dir: PathBuf,
    pub user_sid: String,
    pub package_dir: PathBuf,
    pub confirm_conflict: bool,
    /// sha256 do `Win64\dwmapi.dll` atual quando ele é um carregador nosso de versão anterior (substituído, não
    /// movido para o backup).
    pub replace_loader: Option<String>,
}

fn invalid_args() -> AppError {
    AppError::new(
        "invalid_admin_args",
        "Argumentos inválidos para a instalação com permissão de administrador.",
    )
}

fn activation_failed() -> AppError {
    AppError::new(
        "mod_activation_failed",
        "Não foi possível ativar o mod na pasta do jogo.",
    )
}

/// `args[1]` é exatamente [`FLAG`].
pub fn wants_admin_setup(args: &[String]) -> bool {
    args.get(1).is_some_and(|a| a == FLAG)
}

/// Caminho completo com letra de unidade (`C:\…`), só com componentes normais (sem `.`, `..`, UNC, `\\?\`, `\\.\`).
fn plain_abs(p: &Path) -> bool {
    let s = p.as_os_str().to_string_lossy();
    if s.starts_with("\\\\") || s.starts_with("//") || s.chars().any(char::is_control) {
        return false;
    }
    // `components()` engole `.` no meio do caminho: confere os segmentos crus.
    if s.split(['\\', '/']).any(|seg| seg == "." || seg == "..") {
        return false;
    }
    let mut c = p.components();
    matches!(c.next(), Some(Component::Prefix(px)) if matches!(px.kind(), Prefix::Disk(_)))
        && matches!(c.next(), Some(Component::RootDir))
        && c.all(|x| matches!(x, Component::Normal(_)))
}

fn valid_sha(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f'))
}

/// `^S-1-5-21(-\d{1,10}){4}$` (conta de usuário local ou de domínio).
fn valid_user_sid(s: &str) -> bool {
    let Some(rest) = s.strip_prefix("S-1-5-21-") else {
        return false;
    };
    let parts: Vec<&str> = rest.split('-').collect();
    parts.len() == 4
        && parts
            .iter()
            .all(|p| (1..=10).contains(&p.len()) && p.bytes().all(|b| b.is_ascii_digit()))
}

/// Lê `--mod-admin-setup --game-dir <p> --user-sid <s> --package-dir <p> [--confirm-conflict]
/// [--replace-loader <sha256>]`: cada opção uma vez, nada a mais, valores presentes, caminhos completos.
pub fn parse_args(args: &[String]) -> Result<AdminArgs, String> {
    if !wants_admin_setup(args) {
        return Err(format!("o primeiro argumento tem de ser {FLAG}"));
    }
    let (mut game, mut sid, mut pkg, mut replace) = (None, None, None, None);
    let mut confirm = false;
    let mut it = args[2..].iter();
    while let Some(a) = it.next() {
        let slot = match a.as_str() {
            CONFIRM => {
                if confirm {
                    return Err(format!("{CONFIRM} repetido"));
                }
                confirm = true;
                continue;
            }
            GAME_DIR => &mut game,
            USER_SID => &mut sid,
            PACKAGE_DIR => &mut pkg,
            REPLACE => &mut replace,
            other => return Err(format!("argumento não aceito: {other:?}")),
        };
        if slot.is_some() {
            return Err(format!("{a} repetido"));
        }
        let v = it
            .next()
            .filter(|v| !v.is_empty() && !v.starts_with("--"))
            .ok_or_else(|| format!("{a} sem valor"))?;
        *slot = Some(v.clone());
    }
    let path = |v: Option<String>, name: &str| -> Result<PathBuf, String> {
        let v = v.ok_or_else(|| format!("falta {name}"))?;
        let p = PathBuf::from(v);
        if plain_abs(&p) {
            Ok(p)
        } else {
            Err(format!("{name} precisa ser um caminho completo"))
        }
    };
    let replace_loader = match replace {
        Some(r) if valid_sha(&r) => Some(r),
        Some(_) => return Err(format!("{REPLACE} inválido")),
        None => None,
    };
    Ok(AdminArgs {
        game_dir: path(game, GAME_DIR)?,
        user_sid: sid.ok_or_else(|| format!("falta {USER_SID}"))?,
        package_dir: path(pkg, PACKAGE_DIR)?,
        confirm_conflict: confirm,
        replace_loader,
    })
}

/// Um argumento com as aspas e barras do `CommandLineToArgvW`/runtime do MSVC.
fn quote(arg: &str) -> String {
    let mut out = String::from('"');
    let mut slashes = 0usize;
    for c in arg.chars() {
        match c {
            '\\' => slashes += 1,
            '"' => {
                out.extend(std::iter::repeat_n('\\', slashes * 2 + 1));
                out.push('"');
                slashes = 0;
            }
            _ => {
                out.extend(std::iter::repeat_n('\\', slashes));
                out.push(c);
                slashes = 0;
            }
        }
    }
    out.extend(std::iter::repeat_n('\\', slashes * 2));
    out.push('"');
    out
}

/// Parâmetros para o `ShellExecuteExW` (sem o executável), cada um entre aspas.
pub fn to_command_line(a: &AdminArgs) -> String {
    let mut v: Vec<String> = vec![
        FLAG.into(),
        GAME_DIR.into(),
        a.game_dir.to_string_lossy().into_owned(),
        USER_SID.into(),
        a.user_sid.clone(),
        PACKAGE_DIR.into(),
        a.package_dir.to_string_lossy().into_owned(),
    ];
    if a.confirm_conflict {
        v.push(CONFIRM.into());
    }
    if let Some(r) = &a.replace_loader {
        v.push(REPLACE.into());
        v.push(r.clone());
    }
    v.iter().map(|s| quote(s)).collect::<Vec<_>>().join(" ")
}

/// `path` está dentro de `root` (ou é ela), comparando componente a componente sem diferenciar caixa.
fn inside(path: &Path, root: &Path) -> bool {
    let norm = |p: &Path| -> Vec<String> {
        p.components()
            .filter(|c| !matches!(c, Component::CurDir))
            .map(|c| c.as_os_str().to_string_lossy().to_lowercase())
            .collect()
    };
    let (p, r) = (norm(path), norm(root));
    !r.is_empty() && p.len() >= r.len() && p[..r.len()] == r[..]
}

/// Nenhum componente de `path` (da raiz até ele) é ponto de reanálise; todos existem.
fn no_reparse(path: &Path) -> bool {
    path.ancestors()
        .all(|p| std::fs::symlink_metadata(p).is_ok_and(|m| m.is_dir() && !modact::is_reparse(&m)))
}

/// Revalida os argumentos no processo elevado (ver o topo do módulo). Falha → `invalid_admin_args`.
pub fn validate(a: &AdminArgs, system_root: &Path) -> AppResult<GameInstall> {
    let bad = |why: &str| {
        log::warn!("modo elevado: {why}");
        invalid_args()
    };
    if !valid_user_sid(&a.user_sid) {
        return Err(bad("SID não é de usuário"));
    }
    if !plain_abs(&a.game_dir) || !plain_abs(&a.package_dir) {
        return Err(bad("caminho que não é completo"));
    }
    if a.replace_loader.as_deref().is_some_and(|r| !valid_sha(r)) {
        return Err(bad("hash do carregador inválido"));
    }
    if inside(&a.game_dir, system_root) {
        return Err(bad("pasta do jogo dentro do Windows"));
    }
    let game = validate_game_dir(&a.game_dir).map_err(|_| bad("não é a pasta do jogo"))?;
    if !no_reparse(&modpkg::win64_dir(&game)) {
        return Err(bad("link ou junção no caminho do jogo"));
    }
    if !no_reparse(&a.package_dir) {
        return Err(bad("pacote ausente, ou link ou junção no caminho dele"));
    }
    Ok(game)
}

/// Pastas de um caminho abertas sem compartilhar a exclusão: enquanto vive, nenhuma delas pode ser renomeada,
/// apagada ou trocada por uma junção.
#[derive(Debug)]
pub struct HeldPath {
    _handles: Vec<std::fs::File>,
}

/// Abre cada pasta de `path` (da raiz até ele) sem seguir link, confere que é pasta e não ponto de reanálise, e as
/// mantém abertas (leitura, compartilhando só leitura e gravação). Falha → `invalid_admin_args`.
pub fn hold_path(path: &Path) -> AppResult<HeldPath> {
    let mut chain: Vec<&Path> = path.ancestors().collect();
    chain.reverse();
    let mut handles = Vec::with_capacity(chain.len());
    for p in chain {
        let mut opts = std::fs::OpenOptions::new();
        opts.read(true);
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt;
            use windows_sys::Win32::Storage::FileSystem::{
                FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT, FILE_SHARE_READ,
                FILE_SHARE_WRITE,
            };
            opts.share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE)
                .custom_flags(FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
        }
        let f = opts.open(p).map_err(|e| {
            log::warn!(
                "modo elevado: pasta {} não pôde ser presa: {e}",
                p.display()
            );
            invalid_args()
        })?;
        let ok = f
            .metadata()
            .is_ok_and(|m| m.is_dir() && !modact::is_reparse(&m));
        if !ok {
            log::warn!("modo elevado: {} não é uma pasta comum", p.display());
            return Err(invalid_args());
        }
        handles.push(f);
    }
    Ok(HeldPath { _handles: handles })
}

/// `%SystemRoot%` pelo sistema (não pela variável de ambiente).
#[cfg(windows)]
pub fn system_root() -> AppResult<PathBuf> {
    use std::os::windows::ffi::OsStringExt;
    use windows_sys::Win32::System::SystemInformation::GetSystemWindowsDirectoryW;
    let mut buf = vec![0u16; 1024];
    // SAFETY: buffer com o tamanho informado.
    let n = unsafe { GetSystemWindowsDirectoryW(buf.as_mut_ptr(), buf.len() as u32) } as usize;
    if n == 0 || n >= buf.len() {
        return Err(activation_failed());
    }
    Ok(PathBuf::from(OsString::from_wide(&buf[..n])))
}

#[cfg(not(windows))]
pub fn system_root() -> AppResult<PathBuf> {
    Ok(PathBuf::from("/"))
}

/// `Win64\cbmp`: criada se faltar; existente tem de ser pasta comum.
fn make_container(container: &Path) -> AppResult<()> {
    match std::fs::create_dir(container) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
            let ok = std::fs::symlink_metadata(container)
                .is_ok_and(|m| m.is_dir() && !modact::is_reparse(&m));
            if ok {
                Ok(())
            } else {
                log::warn!("modo elevado: cbmp não é uma pasta comum");
                Err(activation_failed())
            }
        }
        Err(e) => {
            log::warn!("modo elevado: cbmp não criada: {e}");
            Err(activation_failed())
        }
    }
}

/// O trabalho elevado (ver o topo do módulo). `pubkey` é a chave embutida do launcher.
pub fn run(a: &AdminArgs, pubkey: &str) -> AppResult<()> {
    let game = validate(a, &system_root()?)?;
    let win64 = modpkg::win64_dir(&game);
    let _held_game = hold_path(&win64)?;
    let _held_pkg = hold_path(&a.package_dir)?;
    let (mut zip, sig) = modpkg::open_package(&a.package_dir)?;
    let launcher =
        semver::Version::parse(env!("CARGO_PKG_VERSION")).expect("versão do pacote é semver");
    let pkg = modpkg::verify_package(&mut zip, &sig, pubkey, &launcher)?;
    // Registro só em memória: o carregador do pacote é o nosso; o `--replace-loader` é o anterior.
    let mut rec = Installed {
        version: pkg.signed.version.to_string(),
        protocol: pkg.signed.protocol,
        game_dir: game.dir.clone(),
        sha256: pkg.signed.sha256.clone(),
        files: Vec::new(),
        layout: LAYOUT,
        loader: Some(pkg.loader_ref.clone()),
        loader_history: a.replace_loader.iter().cloned().collect(),
        backups: Vec::new(),
        removed: false,
        other_folders: Vec::new(),
    };
    let stop_early = match modact::inspect(&win64, &rec)? {
        Activation::LegacyUe4ss => true,
        Activation::Conflict { .. } => !a.confirm_conflict,
        Activation::Active | Activation::Missing { .. } => false,
    };
    if stop_early {
        // Devolve o erro de conflito sem gravar nada (a ativação confere antes de mexer).
        return modact::activate_from(
            &win64,
            LoaderSource::Bytes(&pkg.loader),
            &mut rec,
            false,
            &mut |_| Err(activation_failed()),
        );
    }
    let container = modpkg::container_dir(&game);
    make_container(&container)?;
    crate::launch::acl::grant_modify(&container, &a.user_sid)?;
    let _held_container = hold_path(&container)?;
    rec.backups = modact::read_manifest(&win64);
    modact::activate_from(
        &win64,
        LoaderSource::Bytes(&pkg.loader),
        &mut rec,
        a.confirm_conflict,
        &mut |r| modact::write_manifest(&win64, &r.backups),
    )
}

/// Código de saída do processo elevado.
pub fn exit_code(r: &AppResult<()>) -> i32 {
    match r {
        Ok(()) => 0,
        Err(e) => match e.code {
            "invalid_admin_args" => 2,
            "mod_signature_invalid" => 3,
            "mod_conflict" => 4,
            _ => 5,
        },
    }
}

/// Chamado por `main.rs` antes do Tauri: `None` se não for o modo elevado (o launcher segue normal); senão o código de
/// saída. Argumento que não é Unicode no modo elevado → 2.
pub fn entry(args: impl IntoIterator<Item = OsString>) -> Option<i32> {
    let raw: Vec<OsString> = args.into_iter().collect();
    if raw.get(1).is_none_or(|a| a != FLAG) {
        return None;
    }
    let Some(args) = raw
        .into_iter()
        .map(|a| a.into_string().ok())
        .collect::<Option<Vec<String>>>()
    else {
        return Some(2);
    };
    let r = parse_args(&args)
        .map_err(|why| {
            log::warn!("modo elevado: {why}");
            invalid_args()
        })
        .and_then(|a| run(&a, crate::update::pubkey()));
    Some(exit_code(&r))
}

/// Pede o UAC e roda o modo elevado (o próprio executável, janela oculta); espera até [`WAIT`] e traduz o código de
/// saída: 0 → ok; 4 → `mod_conflict`; 3 → `mod_signature_invalid`; outro → `mod_activation_failed`. UAC recusado →
/// `mod_needs_admin` (a UI mostra o caminho manual).
#[cfg(windows)]
pub fn request(a: &AdminArgs) -> AppResult<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Foundation::{
        CloseHandle, GetLastError, ERROR_CANCELLED, WAIT_OBJECT_0,
    };
    use windows_sys::Win32::System::Threading::{GetExitCodeProcess, WaitForSingleObject};
    use windows_sys::Win32::UI::Shell::{
        ShellExecuteExW, SEE_MASK_NOASYNC, SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::SW_HIDE;
    let exe = std::env::current_exe().map_err(|_| activation_failed())?;
    let wide = |s: &std::ffi::OsStr| s.encode_wide().chain(Some(0)).collect::<Vec<u16>>();
    let file = wide(exe.as_os_str());
    let verb = wide(std::ffi::OsStr::new("runas"));
    let params = wide(std::ffi::OsStr::new(&to_command_line(a)));
    // SAFETY: estrutura C sem invariantes; todos os campos não usados ficam zerados.
    let mut info: SHELLEXECUTEINFOW = unsafe { std::mem::zeroed() };
    info.cbSize = std::mem::size_of::<SHELLEXECUTEINFOW>() as u32;
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = verb.as_ptr();
    info.lpFile = file.as_ptr();
    info.lpParameters = params.as_ptr();
    info.nShow = SW_HIDE;
    // SAFETY: `info` aponta para strings terminadas em zero que vivem até o fim da chamada.
    if unsafe { ShellExecuteExW(&mut info) } == 0 {
        // SAFETY: logo após a chamada que falhou.
        let err = unsafe { GetLastError() };
        if err == ERROR_CANCELLED {
            log::info!("instalação com permissão de administrador recusada no UAC");
            return Err(modpkg::mod_needs_admin());
        }
        log::warn!("modo elevado não iniciado (erro {err})");
        return Err(activation_failed());
    }
    let process = info.hProcess;
    if process.is_null() {
        return Err(activation_failed());
    }
    // SAFETY: handle de processo devolvido por ShellExecuteExW (SEE_MASK_NOCLOSEPROCESS); fechado abaixo.
    let waited = unsafe { WaitForSingleObject(process, WAIT.as_millis() as u32) };
    let mut code = 0u32;
    let got = waited == WAIT_OBJECT_0 && unsafe { GetExitCodeProcess(process, &mut code) } != 0;
    // SAFETY: handle aberto acima.
    unsafe { CloseHandle(process) };
    if !got {
        log::warn!("modo elevado não terminou em {} s", WAIT.as_secs());
        return Err(activation_failed());
    }
    log::info!("modo elevado terminou com código {code}");
    match code {
        0 => Ok(()),
        3 => Err(AppError::new(
            "mod_signature_invalid",
            "A assinatura do mod não confere; nada foi instalado.",
        )),
        4 => Err(AppError::new(
            "mod_conflict",
            "Já existe outro carregador de mods (UE4SS) na pasta do jogo.",
        )),
        _ => Err(activation_failed()),
    }
}

#[cfg(not(windows))]
pub fn request(_a: &AdminArgs) -> AppResult<()> {
    Err(activation_failed())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn aspas_seguem_as_regras_do_msvc() {
        assert_eq!(quote(r"C:\Jogos\HL"), r#""C:\Jogos\HL""#);
        assert_eq!(quote(r"C:\Jogos\"), r#""C:\Jogos\\""#);
        assert_eq!(quote(r#"a"b"#), r#""a\"b""#);
        assert_eq!(quote(r#"a\"b"#), r#""a\\\"b""#);
        assert_eq!(quote(""), r#""""#);
    }

    #[test]
    fn sid_de_usuario() {
        assert!(valid_user_sid("S-1-5-21-1-2-3-1001"));
        assert!(valid_user_sid(
            "S-1-5-21-1234567890-1234567890-1234567890-1001"
        ));
        for bad in [
            "S-1-5-18",
            "S-1-5-21-1",
            "S-1-5-21-1-2-3",
            "S-1-5-21-1-2-3-4-5",
            "S-1-5-21-12345678901-2-3-4",
            "S-1-5-21-a-2-3-4",
            "S-1-5-32-544",
            " S-1-5-21-1-2-3-4",
        ] {
            assert!(!valid_user_sid(bad), "{bad}");
        }
    }

    #[cfg(windows)]
    #[test]
    fn caminho_completo_com_unidade() {
        for ok in [
            r"C:\Jogos\HL",
            r"D:\",
            r"C:\Program Files\Epic Games\HogwartsLegacy\",
        ] {
            assert!(plain_abs(Path::new(ok)), "{ok}");
        }
        for bad in [
            r"Jogos\HL",
            r"\Jogos",
            r"C:Jogos",
            r"\\servidor\share\HL",
            r"\\?\C:\HL",
            r"\\.\C:\HL",
            r"C:\Jogos\..\Windows",
            r"C:\Jogos\.\HL",
        ] {
            assert!(!plain_abs(Path::new(bad)), "{bad}");
        }
    }

    #[test]
    fn dentro_sem_diferenciar_caixa() {
        assert!(inside(Path::new(r"C:\WINDOWS\x"), Path::new(r"C:\Windows")));
        assert!(inside(Path::new(r"c:\windows"), Path::new(r"C:\Windows\")));
        assert!(!inside(
            Path::new(r"C:\WindowsApps\x"),
            Path::new(r"C:\Windows")
        ));
        assert!(!inside(Path::new(r"D:\Windows"), Path::new(r"C:\Windows")));
    }
}
