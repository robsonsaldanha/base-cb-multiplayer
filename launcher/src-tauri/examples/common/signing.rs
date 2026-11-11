//! Assinatura de releases: núcleo do assinador do workflow de release (`launcher/release-signer`, o ÚNICO código que
//! recebe a chave `CBMP_UPDATE_KEY`) e do *example* `dev_sign_mod` (mod de teste, chave dev).
//!
//! Este arquivo **não pode** usar `cbmp_launcher_lib` nem nada além de `minisign`, `base64`, `sha2` e `semver`: o
//! `launcher/release-signer` o inclui por `#[path]` e só tem essas dependências (nenhum build script do launcher, do
//! Tauri ou do npm roda perto da chave). A conferência "como o launcher confere" fica em `verify.rs` e roda em outro
//! job, sem a chave. Aqui a autoconferência usa só o `minisign` (o par da chave privada é `keys/update.pub`?).
//!
//! Nenhuma função imprime ou grava a chave privada; erros nunca trazem o conteúdo dela.

// Cada usuário usa só uma parte.
#![allow(dead_code)]

use base64::Engine;
use sha2::{Digest, Sha256};
use std::io::{Cursor, Write};
use std::path::Path;

/// Key id (minisign) da chave de DESENVOLVIMENTO que está em `keys/update.pub` até a troca pela de produção.
/// Release real assinado com ela é recusado.
pub const DEV_KEY_ID: &str = "BDCB478091E91629";

fn b64() -> base64::engine::GeneralPurpose {
    base64::engine::general_purpose::STANDARD
}

fn b64_text(s: &str) -> Option<String> {
    b64()
        .decode(s.trim())
        .ok()
        .and_then(|b| String::from_utf8(b).ok())
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
    let text = b64_text(pubkey_tauri)
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
    let text = b64_text(key_tauri)
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

/// Nome do instalador no release `launcher-vX.Y.Z` (sem espaço: o GitHub troca espaços em nomes de asset).
pub fn launcher_installer_name(version: &str) -> String {
    format!("CBMultiplayer_{version}_x64-setup.exe")
}

/// Comentário confiável do instalador, no formato do `tauri build` com `requireSignedVersion`
/// (`timestamp:<unix>\tfile:<nome>\tversion:X.Y.Z`); o launcher exige o campo `version:`.
pub fn launcher_comment(timestamp: u64, file_name: &str, version: &str) -> String {
    format!("timestamp:{timestamp}\tfile:{file_name}\tversion:{version}")
}

/// Versão `X.Y.Z` (semver sem pré-release nem build, como as tags do Hub).
pub fn check_version(version: &str) -> Result<(), String> {
    let v = semver::Version::parse(version).map_err(|_| format!("versão inválida: {version}"))?;
    if !v.pre.is_empty() || !v.build.is_empty() || v.to_string() != version {
        return Err(format!("versão deve ser X.Y.Z: {version}"));
    }
    Ok(())
}

/// Protocolo como as tags `mod-pN-...` do Hub: inteiro sem zeros à esquerda.
pub fn parse_protocol(v: &str) -> Result<u32, String> {
    let ok = !v.is_empty()
        && v.len() <= 9
        && v.bytes().all(|b| b.is_ascii_digit())
        && (v == "0" || !v.starts_with('0'));
    ok.then(|| v.parse().ok())
        .flatten()
        .ok_or_else(|| format!("protocolo inválido: {v}"))
}

fn sign_b64(sk: &minisign::SecretKey, data: &[u8], comment: &str) -> Result<String, String> {
    let sig = minisign::sign(None, sk, Cursor::new(data), Some(comment), None)
        .map_err(|e| format!("falha ao assinar: {e}"))?;
    Ok(b64().encode(sig.to_string()))
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
    let sig = sign_b64(sk, zip, &mod_comment(protocol, version, &sha))?;
    Ok((sig, sha))
}

/// Confere `sig_b64` (formato do Tauri) com `pubkey_tauri` só com o crate `minisign` e devolve o comentário
/// confiável. Serve para o assinador recusar uma chave privada que não é o par de `keys/update.pub` antes de gravar;
/// a conferência completa (mesmas funções do launcher) é `verify.rs`, em outro passo.
pub fn self_check(pubkey_tauri: &str, data: &[u8], sig_b64: &str) -> Result<String, String> {
    let pk = b64_text(pubkey_tauri)
        .and_then(|t| minisign::PublicKeyBox::from_string(&t).ok())
        .and_then(|b| b.into_public_key().ok())
        .ok_or("chave pública inválida")?;
    let sig = b64_text(sig_b64)
        .and_then(|t| minisign::SignatureBox::from_string(&t).ok())
        .ok_or("assinatura fora do formato do Tauri")?;
    minisign::verify(&pk, &sig, Cursor::new(data), true, false, false).map_err(|_| {
        "assinatura não confere com keys/update.pub (a chave privada não é o par dela?)"
    })?;
    sig.trusted_comment()
        .map_err(|_| "assinatura sem comentário confiável".into())
}

/// Grava `data` em `path` só se o arquivo ainda não existir (nunca sobrescreve um artefato do release).
fn write_new(path: &Path, data: &[u8]) -> Result<(), String> {
    let mut f = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|e| format!("gravar {}: {e}", path.display()))?;
    f.write_all(data)
        .and_then(|()| f.sync_all())
        .map_err(|e| format!("gravar {}: {e}", path.display()))
}

/// Assina `zip` e grava os três arquivos do release em `out` (zip, `.sig`, `.sha256`); confere com `pubkey` antes
/// de gravar. Devolve o comentário confiável.
pub fn sign_mod_release(
    pubkey: &str,
    sk: &minisign::SecretKey,
    zip: &[u8],
    protocol: u32,
    version: &str,
    out: &Path,
) -> Result<String, String> {
    refuse_dev_key(pubkey)?;
    check_version(version)?;
    if !zip.starts_with(b"PK\x03\x04") {
        return Err("o arquivo do mod não é um zip".into());
    }
    let (sig, sha) = sign_mod(sk, zip, protocol, version)?;
    let comment = mod_comment(protocol, version, &sha);
    if self_check(pubkey, zip, &sig)? != comment {
        return Err("comentário confiável gravado difere do esperado".into());
    }
    std::fs::create_dir_all(out).map_err(|e| format!("criar {}: {e}", out.display()))?;
    let name = mod_zip_name(protocol, version);
    write_new(&out.join(&name), zip)?;
    write_new(&out.join(format!("{name}.sig")), sig.as_bytes())?;
    write_new(
        &out.join(format!("{name}.sha256")),
        sha256_line(&sha, &name).as_bytes(),
    )?;
    Ok(comment)
}

/// Assina o instalador `installer` (que tem de se chamar `CBMultiplayer_X.Y.Z_x64-setup.exe`) e grava
/// `<installer>.sig` ao lado, no formato do updater do Tauri (`version:X.Y.Z` no comentário confiável); confere com
/// `pubkey` antes de gravar. Devolve o comentário confiável.
pub fn sign_launcher_release(
    pubkey: &str,
    sk: &minisign::SecretKey,
    installer: &Path,
    version: &str,
    timestamp: u64,
) -> Result<String, String> {
    refuse_dev_key(pubkey)?;
    check_version(version)?;
    let name = launcher_installer_name(version);
    if installer.file_name().and_then(|n| n.to_str()) != Some(name.as_str()) {
        return Err(format!(
            "o instalador tem de se chamar {name} (veio {})",
            installer.display()
        ));
    }
    let bytes = std::fs::read(installer)
        .map_err(|e| format!("não foi possível ler {}: {e}", installer.display()))?;
    if !bytes.starts_with(b"MZ") {
        return Err(format!("{name} não é um executável"));
    }
    let comment = launcher_comment(timestamp, &name, version);
    let sig = sign_b64(sk, &bytes, &comment)?;
    if self_check(pubkey, &bytes, &sig)? != comment {
        return Err("comentário confiável gravado difere do esperado".into());
    }
    let mut sig_path = installer.as_os_str().to_owned();
    sig_path.push(".sig");
    write_new(Path::new(&sig_path), sig.as_bytes())?;
    Ok(comment)
}

/// Linha do arquivo `.sha256` (formato do `sha256sum`).
pub fn sha256_line(sha: &str, name: &str) -> String {
    format!("{sha}  {name}\n")
}
