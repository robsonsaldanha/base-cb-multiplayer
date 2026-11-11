//! Conferência de releases com as mesmas funções do launcher (`modpkg::verify_minisign`, `modpkg::parse_mod_comment`,
//! `update::verify_launcher_package`): o que passa aqui é exatamente o que o launcher aceita.
//!
//! Usa `cbmp_launcher_lib`, então nunca roda no passo que tem a chave privada (veja `signing.rs`). Requer o módulo
//! `signing` como irmão (`#[path = "common/signing.rs"] mod signing;` no example).

// Cada example usa só uma parte.
#![allow(dead_code)]

use crate::signing;

/// Confere o mod como o launcher: assinatura da chave `pubkey`, comentário no formato exato e com o mesmo
/// protocolo, versão e sha256 do zip.
pub fn verify_mod(
    pubkey: &str,
    zip: &[u8],
    sig_b64: &str,
    protocol: u32,
    version: &str,
) -> Result<(), String> {
    let comment = cbmp_launcher_lib::modpkg::verify_minisign(zip, sig_b64, pubkey)
        .map_err(|_| "assinatura do mod não confere com a chave pública")?;
    let signed = cbmp_launcher_lib::modpkg::parse_mod_comment(&comment)
        .ok_or("comentário confiável do mod fora do formato exigido")?;
    let sha = signing::sha256_hex(zip);
    if comment != signing::mod_comment(protocol, version, &sha)
        || signed.sha256 != sha
        || signed.protocol != protocol
        || signed.version.to_string() != version
    {
        return Err(format!(
            "comentário confiável do mod não confere: \"{comment}\""
        ));
    }
    Ok(())
}

/// Confere o instalador do launcher como o launcher (chave `pubkey` e `version:X.Y.Z` no comentário confiável).
pub fn verify_launcher(
    pubkey: &str,
    installer: &[u8],
    sig_b64: &str,
    version: &str,
) -> Result<(), String> {
    signing::check_version(version)?;
    cbmp_launcher_lib::update::verify_launcher_package(installer, sig_b64.trim(), pubkey, version)
        .map_err(|_| {
            format!("assinatura do instalador não confere com a chave pública ou não é da versão {version}")
        })?;
    // O launcher só exige `version:`; aqui também que ela seja exatamente a pedida (sem `v`). A assinatura é conferida
    // de novo só porque `verify_launcher_package` não devolve o comentário confiável; é o mesmo par chave/arquivo.
    let comment = cbmp_launcher_lib::modpkg::verify_minisign(installer, sig_b64.trim(), pubkey)
        .map_err(|_| "assinatura do instalador não confere")?;
    if !comment
        .split('\t')
        .any(|f| f == format!("version:{version}"))
    {
        return Err(format!(
            "comentário confiável do instalador sem version:{version}"
        ));
    }
    Ok(())
}
