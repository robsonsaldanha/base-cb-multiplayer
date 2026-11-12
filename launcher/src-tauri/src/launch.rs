//! Arquivo de conexão, ponteiro do mod e abertura do jogo.
//!
//! `run\connect.json` = `{host, port, ticket, serverId, contentDir, protocol, contentReadOnly, launchId}`:
//! - `host` é IP literal (resolvido e conferido pelo launcher); `launchId` (32 hex aleatórios) é o mesmo do ponteiro;
//! - criado com DACL protegida só do usuário atual (`SetNamedSecurityInfoW`) **antes** de receber o ticket, e
//!   trocado por rename (o arquivo final já nasce restrito);
//! - o mod lê e apaga; o launcher apaga depois de [`CONNECT_TTL`] se ainda for o mesmo conteúdo;
//! - `contentDir` (`servers\<id>`) é **somente leitura** para o mod: os arquivos são hard links do cache verificado,
//!   então escrever neles alteraria o cache de todos os servidores (`contentReadOnly: true` deixa isso explícito).
//!
//! Ponteiro `Win64\cbmp\cbmp-run.json` = `{v: 1, runDir, launchId, writtenAt, launcher}` (sem segredo): diz ao mod
//! onde está o `run\` do launcher que abriu o jogo. Gravado depois do `connect.json` (temporário + rename) e apagado
//! junto com ele no fim do prazo, se ainda for o nosso (com ou sem `connect.json`).
//!
//! O ticket nunca vai para linha de comando nem variável de ambiente: o executável é aberto sem argumentos
//! (`CreateProcessW` via `std::process::Command`), com a pasta do jogo como diretório atual. No modo simular nada
//! é aberto e a UI recebe o conteúdo sem o ticket.

use crate::error::{AppError, AppResult};
use crate::game::{game_not_found, GameInstall};
use serde::Serialize;
use std::fmt;
use std::path::{Path, PathBuf};
use std::time::Duration;

pub const CONNECT_FILE: &str = "connect.json";
/// Depois disso o launcher apaga o arquivo (se o mod ainda não o leu) e o ponteiro (se ainda for o nosso). O mod
/// pode iniciar duas vezes (~33 s entre elas) e só resgata depois de 45 s: o prazo cobre os dois.
pub const CONNECT_TTL: Duration = Duration::from_secs(240);
/// Ponteiro em `Win64\cbmp\`.
pub const POINTER_FILE: &str = "cbmp-run.json";
/// Versão do formato do ponteiro.
pub const POINTER_VERSION: u32 = 1;
/// Ponteiro maior que isto não é o nosso (o mod recusa acima de 4 KiB).
const MAX_POINTER_BYTES: u64 = 4096;

/// Identificador desta abertura: 16 bytes aleatórios do sistema em 32 hex minúsculos.
pub fn new_launch_id() -> String {
    let mut raw = [0u8; 16];
    getrandom::fill(&mut raw).expect("gerador aleatório do sistema");
    raw.iter().map(|b| format!("{b:02x}")).collect()
}

/// Dados de conexão gravados para o mod.
#[derive(Clone, PartialEq, Eq)]
pub struct ConnectInfo {
    pub host: String,
    pub port: u16,
    pub ticket: String,
    pub server_id: String,
    pub content_dir: PathBuf,
    pub protocol: u32,
    /// Mesmo valor do ponteiro.
    pub launch_id: String,
}

impl fmt::Debug for ConnectInfo {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ConnectInfo")
            .field("host", &self.host)
            .field("port", &self.port)
            .field("ticket", &"<oculto>")
            .field("server_id", &self.server_id)
            .field("content_dir", &self.content_dir)
            .field("protocol", &self.protocol)
            .field("launch_id", &self.launch_id)
            .finish()
    }
}

/// O que a UI mostra no modo simular: o arquivo de conexão sem o ticket.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ConnectPreview {
    pub host: String,
    pub port: u16,
    pub server_id: String,
    pub content_dir: PathBuf,
    pub protocol: u32,
}

impl ConnectInfo {
    pub fn preview(&self) -> ConnectPreview {
        ConnectPreview {
            host: self.host.clone(),
            port: self.port,
            server_id: self.server_id.clone(),
            content_dir: self.content_dir.clone(),
            protocol: self.protocol,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(tag = "kind", content = "preview", rename_all = "camelCase")]
pub enum LaunchOutcome {
    Started,
    Simulated(ConnectPreview),
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ConnectFileBody<'a> {
    host: &'a str,
    port: u16,
    ticket: &'a str,
    server_id: &'a str,
    content_dir: &'a Path,
    protocol: u32,
    content_read_only: bool,
    launch_id: &'a str,
}

/// Bytes exatos de `connect.json` para `info`.
pub fn connect_json(info: &ConnectInfo) -> Vec<u8> {
    serde_json::to_vec_pretty(&ConnectFileBody {
        host: &info.host,
        port: info.port,
        ticket: &info.ticket,
        server_id: &info.server_id,
        content_dir: &info.content_dir,
        protocol: info.protocol,
        content_read_only: true,
        launch_id: &info.launch_id,
    })
    .expect("connect.json sempre serializa")
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct PointerBody<'a> {
    v: u32,
    run_dir: &'a Path,
    launch_id: &'a str,
    written_at: u64,
    launcher: &'a str,
}

/// Bytes exatos do ponteiro (`v`, `runDir`, `launchId`, `writtenAt` em segundos Unix, `launcher`). Sem ticket nem
/// endereço.
pub fn pointer_json(run_dir: &Path, launch_id: &str, written_at: u64, launcher: &str) -> Vec<u8> {
    serde_json::to_vec_pretty(&PointerBody {
        v: POINTER_VERSION,
        run_dir,
        launch_id,
        written_at,
        launcher,
    })
    .expect("ponteiro sempre serializa")
}

/// Grava `<container>\cbmp-run.json` (temporário + rename, por cima do anterior). Devolve o caminho. Erro de disco
/// sai como [`AppError::io`] (quem chama converte permissão negada em `mod_folder_protected`).
pub fn write_pointer(
    container: &Path,
    run_dir: &Path,
    launch_id: &str,
    written_at: u64,
    launcher: &str,
) -> AppResult<PathBuf> {
    use std::io::Write;
    std::fs::create_dir_all(container).map_err(|e| AppError::io(container, &e))?;
    let path = container.join(POINTER_FILE);
    let tmp = container.join(format!("{POINTER_FILE}.tmp"));
    let _ = std::fs::remove_file(&tmp);
    let written = (|| {
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&tmp)?;
        f.write_all(&pointer_json(run_dir, launch_id, written_at, launcher))?;
        f.sync_all()?;
        drop(f);
        std::fs::rename(&tmp, &path)
    })();
    written.map(|()| path.clone()).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        AppError::io(&path, &e)
    })
}

/// Apaga `<container>\cbmp-run.json` só se o `launchId` dele for `launch_id` (um play mais novo pode ter regravado).
/// Ausente, ilegível ou de outra abertura: fica.
pub fn remove_pointer_if_ours(container: &Path, launch_id: &str) {
    use std::io::Read;
    let path = container.join(POINTER_FILE);
    let mut bytes = Vec::new();
    let read = std::fs::File::open(&path)
        .and_then(|f| f.take(MAX_POINTER_BYTES + 1).read_to_end(&mut bytes));
    if read.is_err() || bytes.len() as u64 > MAX_POINTER_BYTES {
        return;
    }
    let ours = serde_json::from_slice::<serde_json::Value>(&bytes)
        .ok()
        .is_some_and(|v| v["launchId"].as_str() == Some(launch_id));
    if ours {
        match std::fs::remove_file(&path) {
            Ok(()) => log::info!("ponteiro do mod apagado no fim do prazo"),
            Err(e) => log::warn!("ponteiro do mod não apagado: {e}"),
        }
    }
}

/// Grava `<run_dir>\connect.json` com ACL só do usuário atual. Devolve o caminho.
pub fn write_connect_file(run_dir: &Path, info: &ConnectInfo) -> AppResult<PathBuf> {
    use std::io::Write;
    std::fs::create_dir_all(run_dir).map_err(|e| AppError::io(run_dir, &e))?;
    // A própria pasta `run\` também só do usuário (herdado pelo que nascer nela): o `.tmp` já nasce restrito,
    // antes mesmo da ACL explícita abaixo.
    acl::restrict_dir_to_current_user(run_dir)?;
    let path = run_dir.join(CONNECT_FILE);
    let tmp = run_dir.join(format!("{CONNECT_FILE}.tmp"));
    let _ = std::fs::remove_file(&tmp);
    let written = (|| -> AppResult<()> {
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&tmp)
            .map_err(|e| AppError::io(&tmp, &e))?;
        // ACL antes do conteúdo: o ticket nunca fica num arquivo com a ACL herdada da pasta.
        acl::restrict_to_current_user(&tmp)?;
        f.write_all(&connect_json(info))
            .and_then(|()| f.sync_all())
            .map_err(|e| AppError::io(&tmp, &e))?;
        drop(f);
        std::fs::rename(&tmp, &path).map_err(|e| AppError::io(&path, &e))
    })();
    if written.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    written.map(|()| path)
}

/// Apaga `path` se ainda tiver exatamente `written` (um play mais novo pode ter regravado). Ausente não é erro.
pub fn remove_if_unchanged(path: &Path, written: &[u8]) {
    match std::fs::read(path) {
        Ok(cur) if cur == written => {
            if let Err(e) = std::fs::remove_file(path) {
                log::warn!("connect.json não apagado: {e}");
            }
        }
        _ => {}
    }
}

/// Espera `delay` (relógio do tokio: pausável nos testes), apaga `connect` se ainda tiver `written` (o mod não leu) e
/// o ponteiro em `container` se ainda for desta abertura (com ou sem `connect.json`). `container` nulo: simulado.
pub async fn expire_launch(
    connect: PathBuf,
    written: Vec<u8>,
    container: Option<PathBuf>,
    launch_id: String,
    delay: Duration,
) {
    tokio::time::sleep(delay).await;
    remove_if_unchanged(&connect, &written);
    if let Some(c) = container {
        remove_pointer_if_ours(&c, &launch_id);
    }
}

/// Na abertura do launcher: `connect.json` que sobrou de uma execução anterior (ticket velho) sai.
pub fn remove_stale(run_dir: &Path) {
    for name in [CONNECT_FILE.to_string(), format!("{CONNECT_FILE}.tmp")] {
        let p = run_dir.join(name);
        match std::fs::remove_file(&p) {
            Ok(()) => log::info!("connect.json antigo apagado"),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => log::warn!("connect.json antigo não apagado: {e}"),
        }
    }
}

/// Comando que abre o jogo: o executável, sem argumentos, na pasta do jogo, sem stdio herdado.
pub fn command_for(game: &GameInstall) -> std::process::Command {
    let mut c = std::process::Command::new(&game.exe);
    c.current_dir(&game.dir)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    c
}

/// Abre o jogo, ou no modo simular devolve o preview (sem ticket). O arquivo de conexão já deve estar gravado.
pub fn launch(
    game: Option<&GameInstall>,
    simulate: bool,
    info: &ConnectInfo,
) -> AppResult<LaunchOutcome> {
    if simulate {
        return Ok(LaunchOutcome::Simulated(info.preview()));
    }
    let game = game.ok_or_else(game_not_found)?;
    if !game.exe.is_file() {
        return Err(game_not_found());
    }
    match command_for(game).spawn() {
        Ok(child) => {
            // Não esperamos o jogo: o processo segue sozinho (Steam/Epic cuidam do DRM).
            drop(child);
            log::info!("jogo aberto ({:?})", game.store);
            Ok(LaunchOutcome::Started)
        }
        Err(e) => Err(AppError::new(
            "launch_failed",
            format!("Não foi possível abrir o jogo: {e}"),
        )),
    }
}

/// Segurança de arquivos (Windows): restringir ao usuário atual, ler dono e DACL de volta.
pub mod acl {
    use crate::error::{AppError, AppResult};
    use std::path::Path;

    /// SYSTEM.
    pub const SID_SYSTEM: &str = "S-1-5-18";
    /// BUILTIN\Administrators.
    pub const SID_ADMINS: &str = "S-1-5-32-544";
    /// NT SERVICE\TrustedInstaller (dono de pastas do Windows/Program Files).
    pub const SID_TRUSTED_INSTALLER: &str =
        "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
    /// OWNER RIGHTS (vale para o dono, seja quem for).
    pub const SID_OWNER_RIGHTS: &str = "S-1-3-4";
    /// ACE só herdada pelos filhos (não vale para o próprio objeto).
    pub const INHERIT_ONLY_ACE: u8 = 0x08;
    /// Direitos que deixam alterar o conteúdo/ACL: gravar/criar arquivo, criar pasta, apagar filho, apagar,
    /// trocar DACL/dono, GENERIC_ALL/GENERIC_WRITE.
    pub const WRITE_RIGHTS: u32 = 0x0000_0002
        | 0x0000_0004
        | 0x0000_0040
        | 0x0001_0000
        | 0x0004_0000
        | 0x0008_0000
        | 0x1000_0000
        | 0x4000_0000;

    /// Uma entrada da DACL.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct Ace {
        pub sid: String,
        pub allowed: bool,
        pub mask: u32,
        /// `ACE_HEADER.AceFlags` (herança).
        pub flags: u8,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct Dacl {
        /// Não herda entradas da pasta.
        pub protected: bool,
        pub aces: Vec<Ace>,
    }

    /// Dono + DACL.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct Security {
        pub owner: String,
        pub dacl: Dacl,
    }

    /// Dono confiável (SYSTEM, Administradores, TrustedInstaller ou `me`) e nenhuma ACE de permissão de escrita
    /// para outra conta. Função pura (testável com SIDs falsos).
    pub fn trusted(sec: &Security, me: &str) -> bool {
        let ok_sid =
            |s: &str| s == me || s == SID_SYSTEM || s == SID_ADMINS || s == SID_TRUSTED_INSTALLER;
        ok_sid(&sec.owner)
            && sec.dacl.aces.iter().all(|a| {
                !a.allowed
                    || a.flags & INHERIT_ONLY_ACE != 0
                    || a.mask & WRITE_RIGHTS == 0
                    || ok_sid(&a.sid)
                    || a.sid == SID_OWNER_RIGHTS
            })
    }

    fn acl_error(what: &str, code: u32) -> AppError {
        AppError::new(
            "permission_denied",
            format!(
                "Não foi possível ler ou proteger as permissões do arquivo ({what}: erro {code})."
            ),
        )
    }

    #[cfg(windows)]
    mod win {
        use super::*;
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::Foundation::{
            CloseHandle, GetLastError, LocalFree, ERROR_SUCCESS, HANDLE, HLOCAL,
        };
        use windows_sys::Win32::Security::Authorization::{
            ConvertSidToStringSidW, GetNamedSecurityInfoW, SetEntriesInAclW, SetNamedSecurityInfoW,
            EXPLICIT_ACCESS_W, NO_MULTIPLE_TRUSTEE, SET_ACCESS, SE_FILE_OBJECT, TRUSTEE_IS_SID,
            TRUSTEE_IS_USER, TRUSTEE_W,
        };
        use windows_sys::Win32::Security::{
            GetAce, GetSecurityDescriptorControl, GetTokenInformation, TokenUser,
            ACCESS_ALLOWED_ACE, ACE_HEADER, ACL, DACL_SECURITY_INFORMATION, NO_INHERITANCE,
            OWNER_SECURITY_INFORMATION, PROTECTED_DACL_SECURITY_INFORMATION, PSECURITY_DESCRIPTOR,
            PSID, SE_DACL_PROTECTED, SUB_CONTAINERS_AND_OBJECTS_INHERIT, TOKEN_QUERY, TOKEN_USER,
        };
        use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

        const GENERIC_ALL: u32 = 0x1000_0000;
        const ACCESS_ALLOWED_ACE_TYPE: u8 = 0;
        const ACCESS_DENIED_ACE_TYPE: u8 = 1;

        fn wide(p: &Path) -> Vec<u16> {
            p.as_os_str().encode_wide().chain(Some(0)).collect()
        }

        /// `TOKEN_USER` do processo (buffer que contém o SID).
        fn token_user() -> AppResult<Vec<u8>> {
            let mut token: HANDLE = std::ptr::null_mut();
            // SAFETY: pseudo-handle do processo atual; `token` recebe um handle que fechamos abaixo.
            if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) } == 0 {
                return Err(acl_error("token", unsafe { GetLastError() }));
            }
            let mut len = 0u32;
            // SAFETY: primeira chamada só mede.
            unsafe { GetTokenInformation(token, TokenUser, std::ptr::null_mut(), 0, &mut len) };
            let mut buf = vec![0u8; len as usize];
            // SAFETY: `buf` tem `len` bytes.
            let ok = unsafe {
                GetTokenInformation(token, TokenUser, buf.as_mut_ptr().cast(), len, &mut len)
            };
            let err = unsafe { GetLastError() };
            // SAFETY: handle aberto acima.
            unsafe { CloseHandle(token) };
            if ok == 0 || buf.len() < std::mem::size_of::<TOKEN_USER>() {
                return Err(acl_error("token", err));
            }
            Ok(buf)
        }

        fn user_sid(buf: &[u8]) -> PSID {
            // SAFETY: `buf` veio de GetTokenInformation(TokenUser) e tem pelo menos um TOKEN_USER (lido sem
            // referência alinhada).
            unsafe {
                std::ptr::read_unaligned(buf.as_ptr().cast::<TOKEN_USER>())
                    .User
                    .Sid
            }
        }

        fn sid_string(sid: PSID) -> AppResult<String> {
            let mut s: windows_sys::core::PWSTR = std::ptr::null_mut();
            // SAFETY: `sid` válido; `s` recebe buffer LocalAlloc que liberamos.
            if unsafe { ConvertSidToStringSidW(sid, &mut s) } == 0 {
                return Err(acl_error("sid", unsafe { GetLastError() }));
            }
            // SAFETY: string UTF-16 terminada em zero.
            let len = unsafe { (0..).take_while(|&i| *s.add(i) != 0).count() };
            let out = String::from_utf16_lossy(unsafe { std::slice::from_raw_parts(s, len) });
            unsafe { LocalFree(s as HLOCAL) };
            Ok(out)
        }

        pub fn current_user_sid() -> AppResult<String> {
            let buf = token_user()?;
            sid_string(user_sid(&buf))
        }

        pub fn restrict(path: &Path, inherit: bool) -> AppResult<()> {
            let buf = token_user()?;
            let sid = user_sid(&buf);
            let ea = EXPLICIT_ACCESS_W {
                grfAccessPermissions: GENERIC_ALL,
                grfAccessMode: SET_ACCESS,
                grfInheritance: if inherit {
                    SUB_CONTAINERS_AND_OBJECTS_INHERIT
                } else {
                    NO_INHERITANCE
                },
                Trustee: TRUSTEE_W {
                    pMultipleTrustee: std::ptr::null_mut(),
                    MultipleTrusteeOperation: NO_MULTIPLE_TRUSTEE,
                    TrusteeForm: TRUSTEE_IS_SID,
                    TrusteeType: TRUSTEE_IS_USER,
                    ptstrName: sid.cast(),
                },
            };
            let mut acl: *mut ACL = std::ptr::null_mut();
            // SAFETY: uma entrada válida; ACL nova alocada pela API (LocalFree abaixo).
            let r = unsafe { SetEntriesInAclW(1, &ea, std::ptr::null(), &mut acl) };
            if r != ERROR_SUCCESS {
                return Err(acl_error("acl", r));
            }
            let w = wide(path);
            // SAFETY: caminho terminado em zero; só a DACL (protegida: sem herança da pasta).
            let r = unsafe {
                SetNamedSecurityInfoW(
                    w.as_ptr(),
                    SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    acl,
                    std::ptr::null(),
                )
            };
            // SAFETY: alocada por SetEntriesInAclW; `buf` (o SID) vive até aqui.
            unsafe { LocalFree(acl as HLOCAL) };
            drop(buf);
            if r != ERROR_SUCCESS {
                return Err(acl_error("dacl", r));
            }
            Ok(())
        }

        pub fn read_security(path: &Path) -> AppResult<Security> {
            let w = wide(path);
            let mut owner: PSID = std::ptr::null_mut();
            let mut dacl: *mut ACL = std::ptr::null_mut();
            let mut sd: PSECURITY_DESCRIPTOR = std::ptr::null_mut();
            // SAFETY: caminho terminado em zero; `sd` alocado pela API (LocalFree abaixo); `owner` e `dacl` apontam
            // para dentro dele.
            let r = unsafe {
                GetNamedSecurityInfoW(
                    w.as_ptr(),
                    SE_FILE_OBJECT,
                    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                    &mut owner,
                    std::ptr::null_mut(),
                    &mut dacl,
                    std::ptr::null_mut(),
                    &mut sd,
                )
            };
            if r != ERROR_SUCCESS {
                return Err(acl_error("leitura", r));
            }
            let result = (|| {
                let mut control: u16 = 0;
                let mut rev: u32 = 0;
                // SAFETY: `sd` válido.
                if unsafe { GetSecurityDescriptorControl(sd, &mut control, &mut rev) } == 0 {
                    return Err(acl_error("controle", unsafe { GetLastError() }));
                }
                let owner = if owner.is_null() {
                    String::new()
                } else {
                    sid_string(owner)?
                };
                let mut aces = Vec::new();
                if !dacl.is_null() {
                    // SAFETY: `dacl` válido enquanto `sd` vive.
                    let count = unsafe { (*dacl).AceCount };
                    for i in 0..count as u32 {
                        let mut ace: *mut core::ffi::c_void = std::ptr::null_mut();
                        // SAFETY: índice dentro de AceCount.
                        if unsafe { GetAce(dacl, i, &mut ace) } == 0 {
                            return Err(acl_error("ace", unsafe { GetLastError() }));
                        }
                        // SAFETY: toda ACE começa com ACE_HEADER.
                        let header = unsafe { std::ptr::read_unaligned(ace.cast::<ACE_HEADER>()) };
                        let allowed = match header.AceType {
                            ACCESS_ALLOWED_ACE_TYPE => true,
                            ACCESS_DENIED_ACE_TYPE => false,
                            other => {
                                // Tipo que não interpretamos (callback/objeto): conta como permissão total de um
                                // SID desconhecido, para quem verifica confiança recusar.
                                aces.push(Ace {
                                    sid: format!("<tipo {other}>"),
                                    allowed: true,
                                    mask: u32::MAX,
                                    flags: header.AceFlags,
                                });
                                continue;
                            }
                        };
                        // Permitir/negar têm o mesmo layout: header, máscara, SID a partir de SidStart.
                        let a = ace.cast::<ACCESS_ALLOWED_ACE>();
                        // SAFETY: layout de ACCESS_ALLOWED_ACE/ACCESS_DENIED_ACE.
                        let mask =
                            unsafe { std::ptr::read_unaligned(std::ptr::addr_of!((*a).Mask)) };
                        let sid_ptr = unsafe { std::ptr::addr_of_mut!((*a).SidStart) }.cast();
                        aces.push(Ace {
                            sid: sid_string(sid_ptr)?,
                            allowed,
                            mask,
                            flags: header.AceFlags,
                        });
                    }
                } else {
                    // DACL nula = acesso total para todos.
                    aces.push(Ace {
                        sid: "S-1-1-0".into(),
                        allowed: true,
                        mask: u32::MAX,
                        flags: 0,
                    });
                }
                Ok(Security {
                    owner,
                    dacl: Dacl {
                        protected: control & SE_DACL_PROTECTED != 0,
                        aces,
                    },
                })
            })();
            // SAFETY: alocado por GetNamedSecurityInfoW.
            unsafe { LocalFree(sd as HLOCAL) };
            result
        }
    }

    /// SID do usuário do processo (`S-1-5-...`).
    #[cfg(windows)]
    pub fn current_user_sid() -> AppResult<String> {
        win::current_user_sid()
    }

    /// Troca a DACL de `path` por uma protegida com só o usuário atual (acesso total).
    #[cfg(windows)]
    pub fn restrict_to_current_user(path: &Path) -> AppResult<()> {
        win::restrict(path, false)
    }

    /// Como [`restrict_to_current_user`], para uma pasta: a entrada é herdada por tudo que for criado dentro.
    #[cfg(windows)]
    pub fn restrict_dir_to_current_user(path: &Path) -> AppResult<()> {
        win::restrict(path, true)
    }

    /// Lê dono e DACL de `path` (`GetNamedSecurityInfoW`).
    #[cfg(windows)]
    pub fn read_security(path: &Path) -> AppResult<Security> {
        win::read_security(path)
    }

    /// Lê a DACL de `path` (`GetNamedSecurityInfoW`).
    #[cfg(windows)]
    pub fn read_dacl(path: &Path) -> AppResult<Dacl> {
        win::read_security(path).map(|s| s.dacl)
    }

    /// Fora do Windows (não suportado): permissão 0600.
    #[cfg(not(windows))]
    pub fn restrict_to_current_user(path: &Path) -> AppResult<()> {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600))
                .map_err(|e| AppError::io(path, &e))?;
        }
        let _ = (path, acl_error);
        Ok(())
    }

    /// Fora do Windows (não suportado): permissão 0700.
    #[cfg(not(windows))]
    pub fn restrict_dir_to_current_user(path: &Path) -> AppResult<()> {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700))
                .map_err(|e| AppError::io(path, &e))?;
        }
        let _ = path;
        Ok(())
    }

    #[cfg(test)]
    mod tests {
        use super::*;

        const ME: &str = "S-1-5-21-1-2-3-1001";
        const OTHER: &str = "S-1-5-21-1-2-3-1002";
        const USERS: &str = "S-1-5-32-545";

        fn ace(sid: &str, mask: u32, flags: u8) -> Ace {
            Ace {
                sid: sid.into(),
                allowed: true,
                mask,
                flags,
            }
        }

        fn sec(owner: &str, aces: Vec<Ace>) -> Security {
            Security {
                owner: owner.into(),
                dacl: Dacl {
                    protected: false,
                    aces,
                },
            }
        }

        #[test]
        fn confianca_por_dono_e_escrita_de_terceiros() {
            let read = 0x0012_00A9;
            let base = vec![
                ace(SID_SYSTEM, 0x001F_01FF, 0),
                ace(SID_ADMINS, 0x001F_01FF, 0),
                ace(USERS, read, 0),
            ];
            for owner in [ME, SID_SYSTEM, SID_ADMINS, SID_TRUSTED_INSTALLER] {
                assert!(trusted(&sec(owner, base.clone()), ME), "{owner}");
            }
            assert!(
                !trusted(&sec(OTHER, base.clone()), ME),
                "dono é outro usuário"
            );
            // Outra conta com direito de escrita/criação/troca de ACL.
            for mask in [
                0x2,
                0x4,
                0x40,
                0x0001_0000,
                0x0004_0000,
                0x0008_0000,
                0x1000_0000,
                0x4000_0000,
            ] {
                let mut aces = base.clone();
                aces.push(ace(USERS, mask, 0));
                assert!(!trusted(&sec(SID_ADMINS, aces), ME), "{mask:#x}");
            }
            // Só herdada pelos filhos, ou negação: não conta.
            let mut aces = base.clone();
            aces.push(ace(USERS, 0x001F_01FF, INHERIT_ONLY_ACE));
            aces.push(Ace {
                allowed: false,
                ..ace(OTHER, 0x001F_01FF, 0)
            });
            assert!(trusted(&sec(SID_ADMINS, aces), ME));
        }
    }
}
