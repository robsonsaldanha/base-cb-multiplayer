//! Assinatura e conferência de releases, comum aos *examples* `dev_sign_mod` (mod de teste, chave dev) e
//! `release_sign` (workflow de release, chave do secret `CBMP_UPDATE_KEY`).
//!
//! A conferência usa as mesmas funções do launcher (`modpkg::verify_minisign`, `modpkg::parse_mod_comment`,
//! `update::verify_launcher_package`): o que passa aqui é exatamente o que o launcher aceita.
//!
//! Nenhuma função imprime ou grava a chave privada; erros nunca trazem o conteúdo dela.

// Cada example usa só uma parte.
#![allow(dead_code)]

use base64::Engine;
use sha2::{Digest, Sha256};
use std::io::Cursor;

/// Key id (minisign) da chave de DESENVOLVIMENTO que está em `keys/update.pub` até a troca pela de produção.
/// Release real assinado com ela é recusado.
pub const DEV_KEY_ID: &str = "BDCB478091E91629";

fn b64() -> base64::engine::GeneralPurpose {
    base64::engine::general_purpose::STANDARD
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn sha256_hex(data: &[u8]) -> String {
    hex(&Sha256::digest(data))
}

/// Key id de uma chave pública no formato do Tauri (base64 do arquivo `.pub` do minisign), lido dos bytes da chave
/// (não do comentário, que não é confiável): `Ed` + 8 bytes do número da chave (little-endian) + 32 bytes.
pub fn key_id(pubkey_tauri: &str) -> Result<String, String> {
    let text = b64()
        .decode(pubkey_tauri.trim())
        .ok()
        .and_then(|b| String::from_utf8(b).ok())
        .ok_or("chave pública fora do formato do Tauri (base64 de um .pub do minisign)")?;
    let line = text
        .lines()
        .map(str::trim)
        .find(|l| !l.is_empty() && !l.starts_with("untrusted comment:"))
        .ok_or("chave pública sem a linha da chave")?;
    let raw = b64()
        .decode(line)
        .map_err(|_| "linha da chave pública não é base64")?;
    if raw.len() != 42 || &raw[..2] != b"Ed" {
        return Err("chave pública minisign inválida".into());
    }
    let mut n = [0u8; 8];
    n.copy_from_slice(&raw[2..10]);
    Ok(format!("{:016X}", u64::from_le_bytes(n)))
}

/// Recusa a chave dev: um release real precisa da chave de produção em `keys/update.pub`.
pub fn refuse_dev_key(pubkey_tauri: &str) -> Result<(), String> {
    let id = key_id(pubkey_tauri)?;
    if id == DEV_KEY_ID {
        return Err(format!(
            "keys/update.pub ainda é a chave de desenvolvimento ({DEV_KEY_ID}). Gere o par de produção, troque a \
             pública no repositório e guarde a privada só no secret CBMP_UPDATE_KEY (launcher/README.md, \"Chaves\")."
        ));
    }
    Ok(())
}

/// Chave privada no formato do Tauri (base64 de uma chave secreta do minisign), com a senha (vazia se não tiver).
pub fn load_secret_key(key_tauri: &str, password: &str) -> Result<minisign::SecretKey, String> {
    let text = b64()
        .decode(key_tauri.trim())
        .ok()
        .and_then(|b| String::from_utf8(b).ok())
        .ok_or("a chave privada não está no formato do Tauri (base64 de uma chave minisign)")?;
    minisign::SecretKeyBox::from_string(&text)
        .and_then(|b| b.into_secret_key(Some(password.to_string())))
        .map_err(|_| "chave privada inválida ou senha errada".into())
}

/// Comentário confiável exato do mod (o launcher recusa qualquer outro).
pub fn mod_comment(protocol: u32, version: &str, sha256: &str) -> String {
    format!("cbmp-mod protocol={protocol} version={version} sha256={sha256}")
}

/// Nome do zip do mod no release `mod-pN-vX.Y.Z` (o Hub procura exatamente este nome).
pub fn mod_zip_name(protocol: u32, version: &str) -> String {
    format!("cbmp-mod-p{protocol}-v{version}.zip")
}

/// Versão `X.Y.Z` (semver sem pré-release nem build, como as tags do Hub).
pub fn check_version(version: &str) -> Result<(), String> {
    let v = semver::Version::parse(version).map_err(|_| format!("versão inválida: {version}"))?;
    if !v.pre.is_empty() || !v.build.is_empty() || v.to_string() != version {
        return Err(format!("versão deve ser X.Y.Z: {version}"));
    }
    Ok(())
}

/// Assina o zip do mod. Devolve (`.sig` em base64 no formato do Tauri, sha256 do zip).
pub fn sign_mod(
    sk: &minisign::SecretKey,
    zip: &[u8],
    protocol: u32,
    version: &str,
) -> Result<(String, String), String> {
    check_version(version)?;
    let sha = sha256_hex(zip);
    let comment = mod_comment(protocol, version, &sha);
    let sig = minisign::sign(None, sk, Cursor::new(zip), Some(&comment), None)
        .map_err(|e| format!("falha ao assinar: {e}"))?;
    Ok((b64().encode(sig.to_string()), sha))
}

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
    let sha = sha256_hex(zip);
    if comment != mod_comment(protocol, version, &sha)
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
    check_version(version)?;
    cbmp_launcher_lib::update::verify_launcher_package(installer, sig_b64.trim(), pubkey, version)
        .map_err(|_| {
            format!("assinatura do instalador não confere com a chave pública ou não é da versão {version}")
        })?;
    // O launcher só exige `version:`; aqui também que ela seja exatamente a pedida (sem `v`).
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

/// Linha do arquivo `.sha256` (formato do `sha256sum`).
pub fn sha256_line(sha: &str, name: &str) -> String {
    format!("{sha}  {name}\n")
}
