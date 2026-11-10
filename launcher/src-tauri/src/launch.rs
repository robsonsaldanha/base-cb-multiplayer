//! Arquivo de conexão e abertura do jogo.
//!
//! `run\connect.json` = `{host, port, ticket, serverId, contentDir, protocol, contentReadOnly}`:
//! - criado com DACL protegida só do usuário atual (`SetNamedSecurityInfoW`) **antes** de receber o ticket, e
//!   trocado por rename (o arquivo final já nasce restrito);
//! - o mod lê e apaga; o launcher apaga depois de [`CONNECT_TTL`] se ainda for o mesmo conteúdo;
//! - `contentDir` (`servers\<id>`) é **somente leitura** para o mod: os arquivos são hard links do cache verificado,
//!   então escrever neles alteraria o cache de todos os servidores (`contentReadOnly: true` deixa isso explícito).
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
/// Depois disso o launcher apaga o arquivo se o mod ainda não o leu.
pub const CONNECT_TTL: Duration = Duration::from_secs(60);

/// Dados de conexão gravados para o mod.
#[derive(Clone, PartialEq, Eq)]
pub struct ConnectInfo {
    pub host: String,
    pub port: u16,
    pub ticket: String,
    pub server_id: String,
    pub content_dir: PathBuf,
    pub protocol: u32,
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
    })
    .expect("connect.json sempre serializa")
}

/// Grava `<run_dir>\connect.json` com ACL só do usuário atual. Devolve o caminho.
pub fn write_connect_file(run_dir: &Path, info: &ConnectInfo) -> AppResult<PathBuf> {
    use std::io::Write;
    std::fs::create_dir_all(run_dir).map_err(|e| AppError::io(run_dir, &e))?;
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

/// Espera `delay` (relógio do tokio: pausável nos testes) e apaga o arquivo se o mod não o leu.
pub async fn remove_after(path: PathBuf, written: Vec<u8>, delay: Duration) {
    tokio::time::sleep(delay).await;
    remove_if_unchanged(&path, &written);
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

/// DACL de arquivos (Windows): restringir ao usuário atual e ler de volta.
pub mod acl {
    use crate::error::{AppError, AppResult};
    use std::path::Path;

    /// Uma entrada da DACL.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct Ace {
        pub sid: String,
        pub allowed: bool,
        pub mask: u32,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct Dacl {
        /// Não herda entradas da pasta.
        pub protected: bool,
        pub aces: Vec<Ace>,
    }

    fn acl_error(what: &str, code: u32) -> AppError {
        AppError::new(
            "permission_denied",
            format!("Não foi possível proteger o arquivo de conexão ({what}: erro {code})."),
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
            PROTECTED_DACL_SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, PSID, SE_DACL_PROTECTED,
            TOKEN_QUERY, TOKEN_USER,
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
            // SAFETY: `buf` veio de GetTokenInformation(TokenUser) e tem pelo menos um TOKEN_USER (alinhamento:
            // lido sem referência alinhada).
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

        pub fn restrict_to_current_user(path: &Path) -> AppResult<()> {
            let buf = token_user()?;
            let sid = user_sid(&buf);
            let ea = EXPLICIT_ACCESS_W {
                grfAccessPermissions: GENERIC_ALL,
                grfAccessMode: SET_ACCESS,
                grfInheritance: NO_INHERITANCE,
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

        pub fn read_dacl(path: &Path) -> AppResult<Dacl> {
            let w = wide(path);
            let mut dacl: *mut ACL = std::ptr::null_mut();
            let mut sd: PSECURITY_DESCRIPTOR = std::ptr::null_mut();
            // SAFETY: caminho terminado em zero; `sd` alocado pela API (LocalFree abaixo); `dacl` aponta para dentro
            // dele.
            let r = unsafe {
                GetNamedSecurityInfoW(
                    w.as_ptr(),
                    SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION,
                    std::ptr::null_mut(),
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
                                aces.push(Ace {
                                    sid: format!("<tipo {other}>"),
                                    allowed: false,
                                    mask: 0,
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
                        });
                    }
                }
                Ok(Dacl {
                    protected: control & SE_DACL_PROTECTED != 0,
                    aces,
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
        win::restrict_to_current_user(path)
    }

    /// Lê a DACL de `path` (`GetNamedSecurityInfoW`).
    #[cfg(windows)]
    pub fn read_dacl(path: &Path) -> AppResult<Dacl> {
        win::read_dacl(path)
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
}
