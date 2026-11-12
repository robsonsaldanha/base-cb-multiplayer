//! Nega direitos ao usuário atual numa pasta/arquivo de teste (`icacls /deny`), tirados no `drop`. Simula a pasta do
//! jogo sem permissão de gravação (ex.: Epic em Arquivos de Programas) sem tocar em nenhuma pasta real. Incluído por
//! `#[path = "common/deny.rs"] mod deny;`.
#![allow(dead_code)]
#![cfg(windows)]

use std::path::{Path, PathBuf};

pub struct Deny {
    path: PathBuf,
    sid: String,
}

impl Deny {
    /// `rights` no formato do `icacls` (ex.: `"(W)"`, `"(D,DC)"`).
    pub fn new(path: &Path, rights: &str) -> Self {
        let sid = cbmp_launcher_lib::launch::acl::current_user_sid().unwrap();
        let out = std::process::Command::new("icacls")
            .arg(path)
            .args(["/deny", &format!("*{sid}:{rights}")])
            .output()
            .unwrap();
        assert!(out.status.success(), "{out:?}");
        Self {
            path: path.to_path_buf(),
            sid,
        }
    }
}

impl Drop for Deny {
    fn drop(&mut self) {
        let _ = std::process::Command::new("icacls")
            .arg(&self.path)
            .args(["/remove:d", &format!("*{}", self.sid)])
            .output();
    }
}

/// Junção (`mklink /J`, sem privilégio).
pub fn junction(link: &Path, target: &Path) {
    let out = std::process::Command::new("cmd")
        .args(["/C", "mklink", "/J"])
        .arg(link)
        .arg(target)
        .output()
        .unwrap();
    assert!(out.status.success(), "{out:?}");
}
