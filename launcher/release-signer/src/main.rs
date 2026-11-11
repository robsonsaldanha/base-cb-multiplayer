//! Assinador do workflow de release (`.github/workflows/release.yml`, job `sign`).
//!
//! ```text
//! cbmp-release-signer sign-launcher --installer <CBMultiplayer_X.Y.Z_x64-setup.exe> --version X.Y.Z
//! cbmp-release-signer sign-mod --zip <arquivo.zip> --protocol N --version X.Y.Z --out <pasta>
//! ```
//!
//! - É o único programa que recebe a chave privada: só da variável `CBMP_UPDATE_KEY` (formato do Tauri) e a senha de
//!   `CBMP_UPDATE_KEY_PASSWORD` (vazia se ausente); nunca de argumento, nunca impressa.
//! - Crate próprio com dependências mínimas (`minisign`, `base64`, `sha2`, `semver`): o job que tem a chave só roda
//!   este binário, compilado em outro job sem secrets. Nada do launcher (Tauri, ~400 crates) nem do npm.
//! - Recusa se `keys/update.pub` (embutida aqui, a mesma do launcher) for a chave de desenvolvimento.
//! - `sign-launcher` grava `<instalador>.sig` (formato do updater do Tauri, `version:X.Y.Z` no comentário confiável).
//!   `sign-mod` grava `cbmp-mod-pN-vX.Y.Z.zip`, `.zip.sig` (comentário exato
//!   `cbmp-mod protocol=N version=X.Y.Z sha256=<hex>`) e `.zip.sha256`. Os dois conferem a assinatura com a pública
//!   antes de gravar e nunca sobrescrevem arquivos.
//! - A conferência "como o launcher confere" é `examples/release_sign.rs` (`verify-*`), em outro job, sem a chave.
//!
//! O núcleo é `launcher/src-tauri/examples/common/signing.rs` (o mesmo do `dev_sign_mod`), testado também pelo
//! `cargo test` do launcher contra as funções de conferência do próprio launcher.

#[path = "../../src-tauri/examples/common/signing.rs"]
mod signing;

use std::path::PathBuf;
use std::process::ExitCode;

/// `keys/update.pub` do launcher (a chave pública que o launcher embute).
const PUBKEY: &str = include_str!("../../src-tauri/keys/update.pub");

const USAGE: &str = "uso: cbmp-release-signer sign-launcher --installer <exe> --version X.Y.Z \
                     | sign-mod --zip <zip> --protocol N --version X.Y.Z --out <pasta>";

#[derive(Default, Debug)]
struct Opts {
    zip: Option<PathBuf>,
    out: Option<PathBuf>,
    installer: Option<PathBuf>,
    protocol: Option<u32>,
    version: Option<String>,
}

fn parse(args: &[String]) -> Result<(String, Opts), String> {
    let mut it = args.iter();
    let cmd = it.next().ok_or(USAGE)?.clone();
    let mut o = Opts::default();
    while let Some(a) = it.next() {
        let v = it.next().ok_or_else(|| format!("{a} sem valor"))?;
        match a.as_str() {
            "--zip" => o.zip = Some(v.into()),
            "--out" => o.out = Some(v.into()),
            "--installer" => o.installer = Some(v.into()),
            "--protocol" => o.protocol = Some(signing::parse_protocol(v)?),
            "--version" => o.version = Some(v.clone()),
            other => return Err(format!("argumento desconhecido: {other}\n{USAGE}")),
        }
    }
    Ok((cmd, o))
}

fn need<T>(v: Option<T>, name: &str) -> Result<T, String> {
    v.ok_or_else(|| format!("{name} é obrigatório\n{USAGE}"))
}

type Env<'a> = &'a dyn Fn(&str) -> Option<String>;

/// Chave privada do ambiente (só depois de conferir a pública e os argumentos).
fn secret_key(env: Env) -> Result<minisign::SecretKey, String> {
    let key = env("CBMP_UPDATE_KEY")
        .filter(|k| !k.trim().is_empty())
        .ok_or("CBMP_UPDATE_KEY ausente (secret do environment release)")?;
    let password = env("CBMP_UPDATE_KEY_PASSWORD").unwrap_or_default();
    signing::load_secret_key(&key, &password).map_err(|e| format!("CBMP_UPDATE_KEY: {e}"))
}

fn run(args: &[String], pubkey: &str, env: Env) -> Result<String, String> {
    let (cmd, o) = parse(args)?;
    match cmd.as_str() {
        "sign-launcher" => {
            let version = need(o.version, "--version")?;
            let installer = need(o.installer, "--installer")?;
            // Antes de tocar na chave: com a pública dev nem tenta.
            signing::refuse_dev_key(pubkey)?;
            signing::check_version(&version)?;
            let sk = secret_key(env)?;
            let now = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map_err(|_| "relógio do sistema antes de 1970")?
                .as_secs();
            let comment = signing::sign_launcher_release(pubkey, &sk, &installer, &version, now)?;
            Ok(format!(
                "instalador assinado ({})",
                comment.replace('\t', " ")
            ))
        }
        "sign-mod" => {
            let (protocol, version) = (
                need(o.protocol, "--protocol")?,
                need(o.version, "--version")?,
            );
            let zip_path = need(o.zip, "--zip")?;
            let out = need(o.out, "--out")?;
            signing::refuse_dev_key(pubkey)?;
            signing::check_version(&version)?;
            let zip = std::fs::read(&zip_path)
                .map_err(|e| format!("não foi possível ler {}: {e}", zip_path.display()))?;
            let sk = secret_key(env)?;
            let comment = signing::sign_mod_release(pubkey, &sk, &zip, protocol, &version, &out)?;
            Ok(format!("mod assinado ({comment})"))
        }
        _ => Err(USAGE.into()),
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args, PUBKEY.trim(), &|k| std::env::var(k).ok()) {
        Ok(msg) => {
            println!("cbmp-release-signer: {msg}");
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("cbmp-release-signer: ERRO: {e}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use base64::Engine;

    fn b64(s: impl AsRef<[u8]>) -> String {
        base64::engine::general_purpose::STANDARD.encode(s)
    }

    struct Pair {
        pubkey: String,
        key: String,
    }

    fn pair(password: &str) -> Pair {
        let kp = minisign::KeyPair::generate_encrypted_keypair(Some(password.into())).unwrap();
        Pair {
            pubkey: b64(kp.pk.to_box().unwrap().to_string()),
            key: b64(kp.sk.to_box(None).unwrap().to_string()),
        }
    }

    fn args(a: &[&str]) -> Vec<String> {
        a.iter().map(|s| s.to_string()).collect()
    }

    /// Pasta temporária própria do teste (sem dependência extra).
    fn tmpdir(name: &str) -> PathBuf {
        let d =
            std::env::temp_dir().join(format!("cbmp-release-signer-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn a_chave_embutida_e_a_dev_e_nada_le_a_chave_privada() {
        assert_eq!(signing::key_id(PUBKEY).unwrap(), signing::DEV_KEY_ID);
        let read = std::cell::Cell::new(false);
        let env = |_: &str| {
            read.set(true);
            None
        };
        for a in [
            args(&[
                "sign-launcher",
                "--installer",
                "x.exe",
                "--version",
                "1.0.0",
            ]),
            args(&[
                "sign-mod",
                "--zip",
                "x.zip",
                "--protocol",
                "1",
                "--version",
                "1.0.0",
                "--out",
                "o",
            ]),
        ] {
            let e = run(&a, PUBKEY.trim(), &env).unwrap_err();
            assert!(e.contains(signing::DEV_KEY_ID), "{a:?}: {e}");
        }
        assert!(!read.get(), "leu a chave privada com a pública dev");
    }

    #[test]
    fn assina_instalador_e_mod_com_chave_do_ambiente() {
        let p = pair("senha-de-teste");
        let dir = tmpdir("ok");
        let env = |k: &str| match k {
            "CBMP_UPDATE_KEY" => Some(p.key.clone()),
            "CBMP_UPDATE_KEY_PASSWORD" => Some("senha-de-teste".into()),
            _ => None,
        };
        let exe = dir.join("CBMultiplayer_2.0.1_x64-setup.exe");
        std::fs::write(&exe, b"MZ teste").unwrap();
        let msg = run(
            &args(&[
                "sign-launcher",
                "--installer",
                exe.to_str().unwrap(),
                "--version",
                "2.0.1",
            ]),
            &p.pubkey,
            &env,
        )
        .unwrap();
        assert!(msg.contains("version:2.0.1"), "{msg}");
        let sig =
            std::fs::read_to_string(dir.join("CBMultiplayer_2.0.1_x64-setup.exe.sig")).unwrap();
        let comment = signing::self_check(&p.pubkey, b"MZ teste", &sig).unwrap();
        assert!(comment.starts_with("timestamp:"), "{comment}");
        assert!(
            comment.ends_with("\tfile:CBMultiplayer_2.0.1_x64-setup.exe\tversion:2.0.1"),
            "{comment}"
        );

        let zip = dir.join("in.zip");
        std::fs::write(&zip, b"PK\x03\x04 zip de teste").unwrap();
        let out = dir.join("out");
        run(
            &args(&[
                "sign-mod",
                "--zip",
                zip.to_str().unwrap(),
                "--protocol",
                "2",
                "--version",
                "0.3.0",
                "--out",
                out.to_str().unwrap(),
            ]),
            &p.pubkey,
            &env,
        )
        .unwrap();
        let sig = std::fs::read_to_string(out.join("cbmp-mod-p2-v0.3.0.zip.sig")).unwrap();
        let sha = signing::sha256_hex(b"PK\x03\x04 zip de teste");
        assert_eq!(
            signing::self_check(&p.pubkey, b"PK\x03\x04 zip de teste", &sig).unwrap(),
            signing::mod_comment(2, "0.3.0", &sha)
        );
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn chave_ausente_senha_errada_ou_de_outro_par_sao_recusadas() {
        let (p, q) = (pair("a"), pair("b"));
        let dir = tmpdir("ruim");
        let exe = dir.join("CBMultiplayer_1.0.0_x64-setup.exe");
        std::fs::write(&exe, b"MZ").unwrap();
        let a = args(&[
            "sign-launcher",
            "--installer",
            exe.to_str().unwrap(),
            "--version",
            "1.0.0",
        ]);
        let e = run(&a, &p.pubkey, &|_| None).unwrap_err();
        assert!(e.contains("CBMP_UPDATE_KEY ausente"), "{e}");
        let e = run(&a, &p.pubkey, &|k| match k {
            "CBMP_UPDATE_KEY" => Some(p.key.clone()),
            _ => Some("errada".into()),
        })
        .unwrap_err();
        assert!(!e.contains(&p.key) && e.contains("senha errada"), "{e}");
        let e = run(&a, &p.pubkey, &|k| match k {
            "CBMP_UPDATE_KEY" => Some(q.key.clone()),
            _ => Some("b".into()),
        })
        .unwrap_err();
        assert!(e.contains("não é o par"), "{e}");
        // Só o instalador; nenhum .sig gravado.
        assert_eq!(std::fs::read_dir(&dir).unwrap().count(), 1);
        assert!(run(&args(&["assinar"]), &p.pubkey, &|_| None).is_err());
        assert!(
            run(&args(&["sign-mod", "--protocol", "01"]), &p.pubkey, &|_| {
                None
            })
            .is_err()
        );
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
