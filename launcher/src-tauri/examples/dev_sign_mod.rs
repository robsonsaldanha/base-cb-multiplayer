//! Gera um mod de TESTE assinado (usado por `tools/dev-env.mjs`; nunca para publicação).
//!
//! ```text
//! cargo run --example dev_sign_mod -- --key <arquivo da chave privada do Tauri> --protocol 1 --version 0.1.0 --out <pasta>
//! ```
//!
//! Grava em `<pasta>`: `cbmp-mod-pN-vX.Y.Z.zip` (só um LEIA-ME de texto, sem código), `.zip.sig` (formato do Tauri:
//! base64 do `.sig` do minisign) com o comentário confiável exato que o launcher exige
//! (`cbmp-mod protocol=<N> version=<X.Y.Z> sha256=<hex>`), e `.zip.sha256`. Antes de sair confere a assinatura com
//! `keys/update.pub` (a chave pública embutida no launcher): chave privada que não é o par dela é recusada.
//!
//! A chave privada é lida do arquivo indicado (fora do repositório) e nunca é copiada nem impressa. Senha:
//! `CBMP_KEY_PASSWORD` (vazia se ausente, como a chave dev).
//!
//! É um *example* (não um binário do app) para usar o crate `minisign`, que é só dependência de desenvolvimento.

use base64::Engine;
use sha2::{Digest, Sha256};
use std::io::{Cursor, Write};
use std::path::PathBuf;
use std::process::ExitCode;

struct Args {
    key: PathBuf,
    protocol: u32,
    version: String,
    out: PathBuf,
}

fn parse_args() -> Result<Args, String> {
    let mut key = None;
    let mut protocol = None;
    let mut version = None;
    let mut out = None;
    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        let mut val = || it.next().ok_or_else(|| format!("{a} sem valor"));
        match a.as_str() {
            "--key" => key = Some(PathBuf::from(val()?)),
            "--protocol" => {
                protocol = Some(val()?.parse::<u32>().map_err(|_| "--protocol inválido")?)
            }
            "--version" => version = Some(val()?),
            "--out" => out = Some(PathBuf::from(val()?)),
            other => return Err(format!("argumento desconhecido: {other}")),
        }
    }
    let version = version.ok_or("--version é obrigatório")?;
    semver::Version::parse(&version).map_err(|_| "--version deve ser X.Y.Z")?;
    Ok(Args {
        key: key.ok_or("--key é obrigatório")?,
        protocol: protocol.ok_or("--protocol é obrigatório")?,
        version,
        out: out.ok_or("--out é obrigatório")?,
    })
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn test_zip(protocol: u32, version: &str) -> Vec<u8> {
    let mut w = zip::ZipWriter::new(Cursor::new(Vec::new()));
    let opts = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    w.start_file("LEIA-ME.txt", opts).unwrap();
    write!(
        w,
        "Mod de TESTE do CB Multiplayer (protocolo {protocol}, versão {version}).\r\n\
         Gerado por tools/dev-env.mjs para testar download, assinatura e instalação.\r\n\
         Não contém código.\r\n"
    )
    .unwrap();
    w.finish().unwrap().into_inner()
}

fn run() -> Result<(), String> {
    let a = parse_args()?;
    let key_b64 = std::fs::read_to_string(&a.key)
        .map_err(|e| format!("não foi possível ler a chave em {}: {e}", a.key.display()))?;
    let key_text = base64::engine::general_purpose::STANDARD
        .decode(key_b64.trim())
        .ok()
        .and_then(|b| String::from_utf8(b).ok())
        .ok_or("a chave não está no formato do Tauri (base64 de uma chave minisign)")?;
    let password = std::env::var("CBMP_KEY_PASSWORD").unwrap_or_default();
    let sk = minisign::SecretKeyBox::from_string(&key_text)
        .and_then(|b| b.into_secret_key(Some(password)))
        .map_err(|_| "chave privada inválida ou senha errada (CBMP_KEY_PASSWORD)")?;

    let zip = test_zip(a.protocol, &a.version);
    let sha = hex(&Sha256::digest(&zip));
    let comment = format!(
        "cbmp-mod protocol={} version={} sha256={sha}",
        a.protocol, a.version
    );
    let sig = minisign::sign(None, &sk, Cursor::new(&zip), Some(&comment), None)
        .map_err(|e| format!("falha ao assinar: {e}"))?;
    let sig_b64 = base64::engine::general_purpose::STANDARD.encode(sig.to_string());

    // Confere com a chave pública embutida e com a mesma leitura do launcher.
    let got = cbmp_launcher_lib::modpkg::verify_minisign(
        &zip,
        &sig_b64,
        cbmp_launcher_lib::update::pubkey(),
    )
    .map_err(|_| "a chave privada não é o par de keys/update.pub")?;
    let signed = cbmp_launcher_lib::modpkg::parse_mod_comment(&got)
        .ok_or("comentário confiável fora do formato exigido")?;
    if signed.sha256 != sha
        || signed.protocol != a.protocol
        || signed.version.to_string() != a.version
    {
        return Err("assinatura não confere".into());
    }

    std::fs::create_dir_all(&a.out).map_err(|e| e.to_string())?;
    let name = format!("cbmp-mod-p{}-v{}.zip", a.protocol, a.version);
    let write = |n: String, data: &[u8]| {
        std::fs::write(a.out.join(&n), data).map_err(|e| format!("gravar {n}: {e}"))
    };
    write(name.clone(), &zip)?;
    write(format!("{name}.sig"), sig_b64.as_bytes())?;
    write(
        format!("{name}.sha256"),
        format!("{sha}  {name}\n").as_bytes(),
    )?;
    println!("{name} assinado ({comment})");
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("dev_sign_mod: {e}");
            ExitCode::FAILURE
        }
    }
}
