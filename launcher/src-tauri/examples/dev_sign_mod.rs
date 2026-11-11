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
//! Assinatura em `common/signing.rs` (a mesma do assinador do workflow de release, `launcher/release-signer`) e
//! conferência em `common/verify.rs` (a mesma do `release_sign`).

#[path = "common/signing.rs"]
mod signing;
#[path = "common/verify.rs"]
mod verify;

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
    signing::check_version(&version)?;
    Ok(Args {
        key: key.ok_or("--key é obrigatório")?,
        protocol: protocol.ok_or("--protocol é obrigatório")?,
        version,
        out: out.ok_or("--out é obrigatório")?,
    })
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
    let password = std::env::var("CBMP_KEY_PASSWORD").unwrap_or_default();
    let sk = signing::load_secret_key(&key_b64, &password)
        .map_err(|e| format!("{e} (senha: CBMP_KEY_PASSWORD)"))?;

    let zip = test_zip(a.protocol, &a.version);
    let (sig_b64, sha) = signing::sign_mod(&sk, &zip, a.protocol, &a.version)?;
    // Confere com a chave pública embutida e com a mesma leitura do launcher.
    verify::verify_mod(
        cbmp_launcher_lib::update::pubkey(),
        &zip,
        &sig_b64,
        a.protocol,
        &a.version,
    )
    .map_err(|e| format!("{e} (a chave privada é o par de keys/update.pub?)"))?;

    std::fs::create_dir_all(&a.out).map_err(|e| e.to_string())?;
    let name = signing::mod_zip_name(a.protocol, &a.version);
    let write = |n: String, data: &[u8]| {
        std::fs::write(a.out.join(&n), data).map_err(|e| format!("gravar {n}: {e}"))
    };
    write(name.clone(), &zip)?;
    write(format!("{name}.sig"), sig_b64.as_bytes())?;
    write(
        format!("{name}.sha256"),
        signing::sha256_line(&sha, &name).as_bytes(),
    )?;
    println!(
        "{name} assinado ({})",
        signing::mod_comment(a.protocol, &a.version, &sha)
    );
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
