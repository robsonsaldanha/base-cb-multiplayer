//! Zip do mod no layout 2 (o de Global Constraints, com bytes de teste nas DLLs) e assinatura como a do release,
//! com par minisign gerado no próprio teste. Incluído por `#[path = "common/modzip.rs"] mod modzip;`.
#![allow(dead_code)]

use base64::engine::general_purpose::STANDARD;
use base64::Engine as _;
use sha2::{Digest, Sha256};
use std::io::{Cursor, Write};

/// Bytes de teste das DLLs do UE4SS e do mod.
pub const UE4SS_DLL: &[u8] = b"MZ-ue4ss-de-teste";
pub const MAIN_DLL: &[u8] = b"MZ-main-de-teste";

/// Par minisign só do teste (nunca a chave de dev nem a de release).
pub struct Keys {
    pub pk: minisign::PublicKey,
    pub sk: minisign::SecretKey,
}

pub fn keys() -> Keys {
    let kp = minisign::KeyPair::generate_unencrypted_keypair().unwrap();
    Keys {
        pk: kp.pk,
        sk: kp.sk,
    }
}

impl Clone for Keys {
    fn clone(&self) -> Self {
        Keys {
            pk: self.pk.clone(),
            sk: self.sk.clone(),
        }
    }
}

/// Chave pública no formato do Tauri (`keys/update.pub`): base64 do arquivo `.pub` do minisign.
pub fn pubkey(k: &Keys) -> String {
    STANDARD.encode(k.pk.to_box().unwrap().to_string())
}

pub fn sha_hex(data: &[u8]) -> String {
    Sha256::digest(data)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// Assinatura de mod como a do release: base64 do `.sig`, comentário confiável
/// `cbmp-mod protocol=<N> version=<X.Y.Z> sha256=<hex do zip>`.
pub fn sign(k: &Keys, zip: &[u8], protocol: u32, version: &str) -> String {
    let comment = format!(
        "cbmp-mod protocol={protocol} version={version} sha256={}",
        sha_hex(zip)
    );
    let sig = minisign::sign(Some(&k.pk), &k.sk, Cursor::new(zip), Some(&comment), None)
        .unwrap()
        .to_string();
    STANDARD.encode(sig)
}

/// `cbmp-mod.json` do layout 2.
pub fn meta_json(version: &str, protocol: u32) -> String {
    format!(
        r#"{{"layout":2,"version":"{version}","protocol":{protocol},"ue4ss":"3.0.1+4fb9b615","minLauncher":"0.2.0"}}"#
    )
}

/// Entradas do zip do layout 2 (ordem do pacote real).
pub fn entries(version: &str, protocol: u32, loader: &[u8]) -> Vec<(String, Vec<u8>)> {
    let e = |n: &str, d: &[u8]| (n.to_string(), d.to_vec());
    vec![
        e("cbmp-mod.json", meta_json(version, protocol).as_bytes()),
        e("loader/dwmapi.dll", loader),
        e("ue4ss/UE4SS.dll", UE4SS_DLL),
        e(
            "ue4ss/UE4SS-settings.ini",
            b"[Debug]\r\nConsoleEnabled = 0\r\nGuiConsoleEnabled = 0\r\nGuiConsoleVisible = 0\r\n",
        ),
        e("ue4ss/Mods/mods.txt", b"cb-mp : 1\r\nKeybinds : 0\r\n"),
        e("ue4ss/Mods/cb-mp/enabled.txt", b""),
        e("ue4ss/Mods/cb-mp/dlls/main.dll", MAIN_DLL),
        e("licenses/UE4SS-LICENSE.txt", b"MIT"),
        e("licenses/THIRD-PARTY-NOTICES.txt", b"avisos"),
        e("LEIA-ME.txt", b"CB Multiplayer"),
    ]
}

/// Zip (deflate) com estas entradas, na ordem.
pub fn zip_of(entries: &[(String, Vec<u8>)]) -> Vec<u8> {
    let mut w = zip::ZipWriter::new(Cursor::new(Vec::new()));
    let opts = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    for (name, data) in entries {
        w.start_file(name.as_str(), opts).unwrap();
        w.write_all(data).unwrap();
    }
    w.finish().unwrap().into_inner()
}

/// Zip v2 completo.
pub fn build(version: &str, protocol: u32, loader: &[u8]) -> Vec<u8> {
    zip_of(&entries(version, protocol, loader))
}
