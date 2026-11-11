//! Conferência do workflow de release (`.github/workflows/release.yml`), SEM a chave privada.
//!
//! ```text
//! cargo run --example release_sign -- check-pubkey
//! cargo run --example release_sign -- verify-mod --dir <pasta> --protocol N --version X.Y.Z
//! cargo run --example release_sign -- verify-launcher --installer <setup.exe> --sig <setup.exe.sig> --version X.Y.Z
//! ```
//!
//! - Todos os comandos recusam se `keys/update.pub` (a chave pública embutida no launcher) ainda for a chave de
//!   desenvolvimento (key id `BDCB478091E91629`), lida dos bytes da chave e não do comentário.
//! - `verify-*` conferem como o launcher confere (mesmas funções da biblioteca, `common/verify.rs`), antes de publicar.
//! - **Assinar não é daqui.** Quem assina é `launcher/release-signer` (crate próprio, só `minisign`/`base64`/`sha2`/
//!   `semver`), num job que não roda npm nem compila o launcher: este example liga a biblioteca do launcher (Tauri e
//!   ~400 crates com build scripts) e por isso nunca roda com a chave no ambiente. O núcleo do assinador é
//!   `common/signing.rs`, e os testes daqui conferem o que ele grava com as funções do launcher.
//!
//! É um *example* para usar o crate `minisign` (dependência só de desenvolvimento); `test = true` no Cargo.toml faz
//! o `cargo test` rodar os testes daqui, com pares de chaves gerados no próprio teste.

#[path = "common/signing.rs"]
mod signing;
#[path = "common/verify.rs"]
mod verify;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

const USAGE: &str = "uso: release_sign check-pubkey | verify-mod --dir <pasta> --protocol N --version X.Y.Z                      | verify-launcher --installer <exe> --sig <sig> --version X.Y.Z";

#[derive(Default)]
struct Opts {
    dir: Option<PathBuf>,
    installer: Option<PathBuf>,
    sig: Option<PathBuf>,
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
            "--dir" => o.dir = Some(v.into()),
            "--installer" => o.installer = Some(v.into()),
            "--sig" => o.sig = Some(v.into()),
            "--protocol" => o.protocol = Some(signing::parse_protocol(v)?),
            "--version" => o.version = Some(v.clone()),
            other => {
                return Err(format!(
                    "argumento desconhecido: {other}
{USAGE}"
                ))
            }
        }
    }
    Ok((cmd, o))
}

fn need<T>(v: Option<T>, name: &str) -> Result<T, String> {
    v.ok_or_else(|| {
        format!(
            "{name} é obrigatório
{USAGE}"
        )
    })
}

fn read(p: &Path) -> Result<Vec<u8>, String> {
    std::fs::read(p).map_err(|e| format!("não foi possível ler {}: {e}", p.display()))
}

/// Confere os três arquivos do mod em `dir` (zip, `.sig`, `.sha256`) como o launcher e o Hub os leem.
fn verify_mod_release(
    pubkey: &str,
    dir: &Path,
    protocol: u32,
    version: &str,
) -> Result<(), String> {
    signing::refuse_dev_key(pubkey)?;
    signing::check_version(version)?;
    let name = signing::mod_zip_name(protocol, version);
    let zip = read(&dir.join(&name))?;
    let sig = String::from_utf8(read(&dir.join(format!("{name}.sig")))?)
        .map_err(|_| "o .sig não é texto")?;
    verify::verify_mod(pubkey, &zip, sig.trim(), protocol, version)?;
    let sha_file = String::from_utf8(read(&dir.join(format!("{name}.sha256")))?)
        .map_err(|_| "o .sha256 não é texto")?;
    if sha_file != signing::sha256_line(&signing::sha256_hex(&zip), &name) {
        return Err(format!("{name}.sha256 não confere com o zip"));
    }
    Ok(())
}

fn verify_launcher_release(
    pubkey: &str,
    installer: &Path,
    sig: &Path,
    version: &str,
) -> Result<(), String> {
    signing::refuse_dev_key(pubkey)?;
    let bytes = read(installer)?;
    let sig = String::from_utf8(read(sig)?).map_err(|_| "o .sig não é texto")?;
    verify::verify_launcher(pubkey, &bytes, &sig, version)
}

fn run(args: &[String], pubkey: &str) -> Result<String, String> {
    let (cmd, o) = parse(args)?;
    match cmd.as_str() {
        "check-pubkey" => {
            signing::refuse_dev_key(pubkey)?;
            Ok(format!(
                "keys/update.pub: chave {} (não é a de desenvolvimento)",
                signing::key_id(pubkey)?
            ))
        }
        "verify-mod" => {
            let (protocol, version) = (
                need(o.protocol, "--protocol")?,
                need(o.version, "--version")?,
            );
            verify_mod_release(pubkey, &need(o.dir, "--dir")?, protocol, &version)?;
            Ok("mod conferido".into())
        }
        "verify-launcher" => {
            let version = need(o.version, "--version")?;
            verify_launcher_release(
                pubkey,
                &need(o.installer, "--installer")?,
                &need(o.sig, "--sig")?,
                &version,
            )?;
            Ok(format!("instalador {version} conferido"))
        }
        _ => Err(USAGE.into()),
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args, cbmp_launcher_lib::update::pubkey()) {
        Ok(msg) => {
            println!("release_sign: {msg}");
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("release_sign: ERRO: {e}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use base64::Engine;
    use std::io::{Cursor, Write};

    fn b64(s: impl AsRef<[u8]>) -> String {
        base64::engine::general_purpose::STANDARD.encode(s)
    }

    struct Pair {
        pubkey: String,
        sk: minisign::SecretKey,
    }

    fn pair() -> Pair {
        let kp = minisign::KeyPair::generate_unencrypted_keypair().unwrap();
        Pair {
            pubkey: b64(kp.pk.to_box().unwrap().to_string()),
            sk: kp.sk,
        }
    }

    fn zip() -> Vec<u8> {
        let mut w = zip::ZipWriter::new(Cursor::new(Vec::new()));
        w.start_file("mod.txt", zip::write::SimpleFileOptions::default())
            .unwrap();
        w.write_all(b"mod de teste").unwrap();
        w.finish().unwrap().into_inner()
    }

    fn trusted_comment(sig_b64: &str) -> String {
        let text = String::from_utf8(
            base64::engine::general_purpose::STANDARD
                .decode(sig_b64.trim())
                .unwrap(),
        )
        .unwrap();
        text.lines()
            .find_map(|l| l.strip_prefix("trusted comment: "))
            .unwrap()
            .to_string()
    }

    /// Chave pública (formato do Tauri) com o número de chave trocado pelo da chave dev.
    fn with_dev_keynum(pubkey: &str) -> String {
        let text = String::from_utf8(
            base64::engine::general_purpose::STANDARD
                .decode(pubkey)
                .unwrap(),
        )
        .unwrap();
        let line = text.lines().nth(1).unwrap();
        let mut raw = base64::engine::general_purpose::STANDARD
            .decode(line)
            .unwrap();
        let id = u64::from_str_radix(signing::DEV_KEY_ID, 16).unwrap();
        raw[2..10].copy_from_slice(&id.to_le_bytes());
        b64(format!(
            "untrusted comment: minisign public key: qualquer\n{}\n",
            b64(raw)
        ))
    }

    #[test]
    fn a_chave_do_repositorio_ainda_e_a_dev_e_e_recusada() {
        let repo_pub = cbmp_launcher_lib::update::pubkey();
        assert_eq!(signing::key_id(repo_pub).unwrap(), signing::DEV_KEY_ID);
        let e = signing::refuse_dev_key(repo_pub).unwrap_err();
        assert!(e.contains("chave de desenvolvimento"), "{e}");
        // Todos os comandos param antes de qualquer outra coisa.
        let tmp = tempfile::tempdir().unwrap();
        for args in [
            vec!["check-pubkey"],
            vec![
                "verify-mod",
                "--dir",
                ".",
                "--protocol",
                "1",
                "--version",
                "1.0.0",
            ],
            vec![
                "verify-launcher",
                "--installer",
                "x",
                "--sig",
                "x.sig",
                "--version",
                "1.0.0",
            ],
        ] {
            let args: Vec<String> = args.into_iter().map(String::from).collect();
            let e = run(&args, repo_pub).unwrap_err();
            assert!(e.contains(signing::DEV_KEY_ID), "{args:?}: {e}");
        }
        let p = pair();
        let e =
            signing::sign_mod_release(repo_pub, &p.sk, &zip(), 1, "1.0.0", tmp.path()).unwrap_err();
        assert!(e.contains(signing::DEV_KEY_ID), "{e}");
        assert_eq!(std::fs::read_dir(tmp.path()).unwrap().count(), 0);
        let exe = tmp.path().join(signing::launcher_installer_name("1.0.0"));
        std::fs::write(&exe, b"MZ instalador").unwrap();
        let e = signing::sign_launcher_release(repo_pub, &p.sk, &exe, "1.0.0", 1).unwrap_err();
        assert!(e.contains(signing::DEV_KEY_ID), "{e}");
        assert_eq!(std::fs::read_dir(tmp.path()).unwrap().count(), 1);
    }

    #[test]
    fn key_id_vem_dos_bytes_e_nao_do_comentario() {
        let p = pair();
        assert_ne!(signing::key_id(&p.pubkey).unwrap(), signing::DEV_KEY_ID);
        signing::refuse_dev_key(&p.pubkey).unwrap();
        // Comentário sem o id dev, bytes com ele: recusada.
        assert!(signing::refuse_dev_key(&with_dev_keynum(&p.pubkey)).is_err());
        // Comentário com o id dev, bytes de outra chave: aceita (o comentário não é confiável).
        let text = String::from_utf8(
            base64::engine::general_purpose::STANDARD
                .decode(&p.pubkey)
                .unwrap(),
        )
        .unwrap();
        let line = text.lines().nth(1).unwrap();
        let fake = b64(format!(
            "untrusted comment: minisign public key: {}\n{line}\n",
            signing::DEV_KEY_ID
        ));
        signing::refuse_dev_key(&fake).unwrap();
        assert!(signing::key_id("lixo").is_err());
        assert!(signing::key_id(&b64("untrusted comment: x\nAAAA\n")).is_err());
    }

    #[test]
    fn mod_assinado_com_o_comentario_exato_e_conferido() {
        let p = pair();
        let tmp = tempfile::tempdir().unwrap();
        let z = zip();
        let comment =
            signing::sign_mod_release(&p.pubkey, &p.sk, &z, 3, "1.4.2", tmp.path()).unwrap();
        let sha = signing::sha256_hex(&z);
        assert_eq!(
            comment,
            format!("cbmp-mod protocol=3 version=1.4.2 sha256={sha}")
        );
        let name = "cbmp-mod-p3-v1.4.2.zip";
        assert_eq!(std::fs::read(tmp.path().join(name)).unwrap(), z);
        let sig = std::fs::read_to_string(tmp.path().join(format!("{name}.sig"))).unwrap();
        assert_eq!(trusted_comment(&sig), comment);
        assert_eq!(
            std::fs::read_to_string(tmp.path().join(format!("{name}.sha256"))).unwrap(),
            format!("{sha}  {name}\n")
        );
        verify_mod_release(&p.pubkey, tmp.path(), 3, "1.4.2").unwrap();
        // O launcher aceita o mesmo .sig.
        let got = cbmp_launcher_lib::modpkg::verify_minisign(&z, &sig, &p.pubkey).unwrap();
        let signed = cbmp_launcher_lib::modpkg::parse_mod_comment(&got).unwrap();
        assert_eq!((signed.protocol, signed.sha256.as_str()), (3, sha.as_str()));

        // Outro protocolo/versão pedido na conferência: recusado.
        assert!(verify_mod_release(&p.pubkey, tmp.path(), 4, "1.4.2").is_err());
        // Zip trocado depois de assinado: recusado.
        let mut bad = z.clone();
        bad.extend_from_slice(b"x");
        std::fs::write(tmp.path().join(name), &bad).unwrap();
        assert!(verify_mod_release(&p.pubkey, tmp.path(), 3, "1.4.2").is_err());
        std::fs::write(tmp.path().join(name), &z).unwrap();
        // .sha256 adulterado: recusado.
        std::fs::write(
            tmp.path().join(format!("{name}.sha256")),
            format!("{}  {name}\n", "0".repeat(64)),
        )
        .unwrap();
        assert!(verify_mod_release(&p.pubkey, tmp.path(), 3, "1.4.2").is_err());
    }

    #[test]
    fn chave_privada_que_nao_e_o_par_da_publica_nao_grava_nada() {
        let (a, b) = (pair(), pair());
        let tmp = tempfile::tempdir().unwrap();
        let e = signing::sign_mod_release(&a.pubkey, &b.sk, &zip(), 1, "1.0.0", tmp.path())
            .unwrap_err();
        assert!(e.contains("não é o par"), "{e}");
        assert_eq!(std::fs::read_dir(tmp.path()).unwrap().count(), 0);
    }

    #[test]
    fn comentario_fora_do_formato_e_recusado() {
        let p = pair();
        let z = zip();
        let sha = signing::sha256_hex(&z);
        for comment in [
            format!("cbmp-mod protocol=1 version=1.0.0 sha256={sha} extra"),
            format!("cbmp-mod  protocol=1 version=1.0.0 sha256={sha}"),
            format!("cbmp-mod version=1.0.0 protocol=1 sha256={sha}"),
            format!(
                "cbmp-mod protocol=1 version=1.0.0 sha256={}",
                sha.to_uppercase()
            ),
            format!(
                "cbmp-mod protocol=1 version=1.0.0 sha256={}",
                "a".repeat(64)
            ),
            "timestamp:1\tfile:x.zip".to_string(),
        ] {
            let sig = minisign::sign(None, &p.sk, Cursor::new(&z), Some(&comment), None).unwrap();
            let sig = b64(sig.to_string());
            assert!(
                verify::verify_mod(&p.pubkey, &z, &sig, 1, "1.0.0").is_err(),
                "aceitou \"{comment}\""
            );
        }
    }

    #[test]
    fn instalador_do_launcher_conferido_pela_versao_assinada() {
        let p = pair();
        let tmp = tempfile::tempdir().unwrap();
        let exe = tmp.path().join("CBMultiplayer_1.2.0_x64-setup.exe");
        std::fs::write(&exe, b"MZ instalador de teste").unwrap();
        let sign = |comment: &str| {
            let s = minisign::sign(
                None,
                &p.sk,
                Cursor::new(std::fs::read(&exe).unwrap()),
                Some(comment),
                None,
            )
            .unwrap();
            let path = tmp.path().join("x.sig");
            std::fs::write(&path, b64(s.to_string())).unwrap();
            path
        };
        // Formato do `tauri build`/`tauri signer sign --app-version`.
        let sig = sign("timestamp:1\tfile:CBMultiplayer_1.2.0_x64-setup.exe\tversion:1.2.0");
        verify_launcher_release(&p.pubkey, &exe, &sig, "1.2.0").unwrap();
        assert!(verify_launcher_release(&p.pubkey, &exe, &sig, "1.2.1").is_err());
        let sig = sign("timestamp:1\tfile:CBMultiplayer_1.2.0_x64-setup.exe");
        assert!(verify_launcher_release(&p.pubkey, &exe, &sig, "1.2.0").is_err());
        let sig = sign("timestamp:1\tversion:v1.2.0");
        assert!(verify_launcher_release(&p.pubkey, &exe, &sig, "1.2.0").is_err());
        // Assinado por outra chave.
        let other = pair();
        assert!(
            verify_launcher_release(&other.pubkey, &exe, &sign("version:1.2.0"), "1.2.0").is_err()
        );
    }

    /// O assinador do workflow (`launcher/release-signer`, núcleo em `common/signing.rs`) grava um `.sig` que o
    /// launcher aceita: mesmo formato do `tauri build` com `requireSignedVersion`.
    #[test]
    fn instalador_assinado_pelo_assinador_do_release_e_aceito_pelo_launcher() {
        let p = pair();
        let tmp = tempfile::tempdir().unwrap();
        let exe = tmp.path().join("CBMultiplayer_1.2.0_x64-setup.exe");
        std::fs::write(&exe, b"MZ instalador de teste").unwrap();
        let comment =
            signing::sign_launcher_release(&p.pubkey, &p.sk, &exe, "1.2.0", 1_700_000_000).unwrap();
        assert_eq!(
            comment,
            "timestamp:1700000000	file:CBMultiplayer_1.2.0_x64-setup.exe	version:1.2.0"
        );
        let sig_path = tmp.path().join("CBMultiplayer_1.2.0_x64-setup.exe.sig");
        let sig = std::fs::read_to_string(&sig_path).unwrap();
        assert_eq!(trusted_comment(&sig), comment);
        verify_launcher_release(&p.pubkey, &exe, &sig_path, "1.2.0").unwrap();
        // Mesma leitura do plugin/launcher.
        let bytes = std::fs::read(&exe).unwrap();
        cbmp_launcher_lib::update::verify_launcher_package(&bytes, &sig, &p.pubkey, "1.2.0")
            .unwrap();
        assert!(cbmp_launcher_lib::update::verify_launcher_package(
            &bytes, &sig, &p.pubkey, "1.2.1"
        )
        .is_err());
        // Nunca sobrescreve um .sig existente.
        let e = signing::sign_launcher_release(&p.pubkey, &p.sk, &exe, "1.2.0", 2).unwrap_err();
        assert!(e.contains(".sig"), "{e}");
        assert_eq!(std::fs::read_to_string(&sig_path).unwrap(), sig);

        // Nome que não é o da versão, versão fora de X.Y.Z, arquivo que não é executável: nada gravado.
        let other = tmp.path().join("CBMultiplayer_1.2.1_x64-setup.exe");
        std::fs::write(&other, b"MZ outro").unwrap();
        assert!(signing::sign_launcher_release(&p.pubkey, &p.sk, &other, "1.2.0", 1).is_err());
        assert!(signing::sign_launcher_release(&p.pubkey, &p.sk, &other, "v1.2.1", 1).is_err());
        let not_exe = tmp.path().join("CBMultiplayer_1.3.0_x64-setup.exe");
        std::fs::write(&not_exe, b"nao e exe").unwrap();
        assert!(signing::sign_launcher_release(&p.pubkey, &p.sk, &not_exe, "1.3.0", 1).is_err());
        // Chave privada que não é o par da pública: recusada antes de gravar.
        let q = pair();
        let e = signing::sign_launcher_release(&p.pubkey, &q.sk, &other, "1.2.1", 1).unwrap_err();
        assert!(e.contains("não é o par"), "{e}");
        assert_eq!(std::fs::read_dir(tmp.path()).unwrap().count(), 4);
    }

    #[test]
    fn mod_nao_sobrescreve_arquivos_existentes() {
        let p = pair();
        let tmp = tempfile::tempdir().unwrap();
        signing::sign_mod_release(&p.pubkey, &p.sk, &zip(), 1, "1.0.0", tmp.path()).unwrap();
        assert!(
            signing::sign_mod_release(&p.pubkey, &p.sk, &zip(), 1, "1.0.0", tmp.path()).is_err()
        );
        verify_mod_release(&p.pubkey, tmp.path(), 1, "1.0.0").unwrap();
    }

    #[test]
    fn chave_no_formato_do_tauri_com_senha() {
        let kp =
            minisign::KeyPair::generate_encrypted_keypair(Some("senha-de-teste".into())).unwrap();
        let key = b64(kp.sk.to_box(None).unwrap().to_string());
        let pubkey = b64(kp.pk.to_box().unwrap().to_string());
        let e = signing::load_secret_key(&key, "errada").unwrap_err();
        assert!(!e.contains(&key) && !e.contains("senha-de-teste"), "{e}");
        let sk = signing::load_secret_key(&key, "senha-de-teste").unwrap();
        let tmp = tempfile::tempdir().unwrap();
        signing::sign_mod_release(&pubkey, &sk, &zip(), 1, "0.2.0", tmp.path()).unwrap();
        verify_mod_release(&pubkey, tmp.path(), 1, "0.2.0").unwrap();
        assert!(signing::load_secret_key("nao-e-base64!", "").is_err());
    }

    #[test]
    fn versao_e_protocolo_validados() {
        for v in ["1.2.3", "0.0.0", "10.20.30"] {
            signing::check_version(v).unwrap();
        }
        for v in [
            "1.2",
            "v1.2.3",
            "1.2.3-rc.1",
            "1.2.3+build",
            "01.2.3",
            "",
            "1.2.3 ",
        ] {
            assert!(signing::check_version(v).is_err(), "{v}");
        }
        assert_eq!(signing::parse_protocol("0").unwrap(), 0);
        assert_eq!(signing::parse_protocol("12").unwrap(), 12);
        for v in ["", "01", "-1", "1a", "1234567890"] {
            assert!(signing::parse_protocol(v).is_err(), "{v}");
        }
        let p = pair();
        let tmp = tempfile::tempdir().unwrap();
        assert!(
            signing::sign_mod_release(&p.pubkey, &p.sk, b"nao e zip", 1, "1.0.0", tmp.path())
                .is_err()
        );
    }
}
