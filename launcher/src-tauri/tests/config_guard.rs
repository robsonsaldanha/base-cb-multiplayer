//! Guarda de segurança da configuração: lê `tauri.conf.json` e `capabilities/*.json` de verdade e falha se a UI
//! ganhar acesso a fs/shell/http/opener, se a CSP afrouxar ou se a configuração exigida mudar.

use serde_json::Value;
use std::path::{Path, PathBuf};

const REQUIRED_CSP: &str =
    "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; connect-src ipc: http://ipc.localhost";

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn read_json(p: &Path) -> Value {
    let text = std::fs::read_to_string(p).unwrap_or_else(|e| panic!("ler {}: {e}", p.display()));
    serde_json::from_str(&text).unwrap_or_else(|e| panic!("JSON inválido em {}: {e}", p.display()))
}

fn conf() -> Value {
    read_json(&root().join("tauri.conf.json"))
}

fn capability_files() -> Vec<(String, Value)> {
    let dir = root().join("capabilities");
    let mut out: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.is_file())
        .map(|p| {
            let ext = p.extension().and_then(|e| e.to_str()).unwrap_or("");
            assert_eq!(
                ext,
                "json",
                "capability fora de JSON não é verificada: {}",
                p.display()
            );
            (
                p.file_name().unwrap().to_string_lossy().into_owned(),
                read_json(&p),
            )
        })
        .collect();
    out.sort_by(|a, b| a.0.cmp(&b.0));
    out
}

// ----- verificador -----

const FORBIDDEN_PREFIXES: [&str; 4] = ["fs:", "shell:", "http:", "opener:"];
const FORBIDDEN_PLUGINS: [&str; 4] = ["fs", "shell", "http", "opener"];

/// Identificadores de permissão de uma capability (string ou objeto `{identifier, allow, deny}`).
fn permission_ids(cap: &Value) -> Vec<String> {
    cap.get("permissions")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .map(|p| match p {
                    Value::String(s) => s.clone(),
                    Value::Object(o) => o
                        .get("identifier")
                        .and_then(Value::as_str)
                        .unwrap_or("<sem identifier>")
                        .into(),
                    other => format!("<permissão inválida {other}>"),
                })
                .collect()
        })
        .unwrap_or_default()
}

/// Todas as violações encontradas na configuração (vazio = ok).
fn violations(conf: &Value, caps: &[(String, Value)]) -> Vec<String> {
    let mut v = Vec::new();
    let security = &conf["app"]["security"];

    for key in ["csp", "devCsp"] {
        let csp = &security[key];
        let text = match csp {
            Value::Null => continue,
            Value::String(s) => s.clone(),
            other => other.to_string(),
        };
        if text.contains("unsafe-") {
            v.push(format!("{key} contém unsafe-: {text}"));
        }
    }
    if security["csp"].as_str() != Some(REQUIRED_CSP) {
        v.push(format!("csp diferente da exigida: {}", security["csp"]));
    }
    if !security["dangerousDisableAssetCspModification"].is_null() {
        v.push("dangerousDisableAssetCspModification definido".into());
    }
    if conf["app"]["withGlobalTauri"].as_bool() == Some(true) {
        v.push("withGlobalTauri ligado".into());
    }

    if let Some(plugins) = conf["plugins"].as_object() {
        for name in FORBIDDEN_PLUGINS {
            if plugins.contains_key(name) {
                v.push(format!("plugin proibido configurado: {name}"));
            }
        }
    }

    // Capabilities inline no tauri.conf.json também contam.
    let mut all: Vec<(String, Value)> = caps.to_vec();
    if let Some(inline) = security["capabilities"].as_array() {
        for (i, c) in inline.iter().enumerate() {
            match c {
                Value::Object(_) => {
                    all.push((format!("tauri.conf.json#capabilities[{i}]"), c.clone()))
                }
                Value::String(id) => {
                    if !caps
                        .iter()
                        .any(|(_, cap)| cap["identifier"].as_str() == Some(id))
                    {
                        v.push(format!("capability {id} referenciada sem arquivo"));
                    }
                }
                other => v.push(format!("capability inline inválida: {other}")),
            }
        }
    }

    for (name, cap) in &all {
        if !cap["remote"].is_null() {
            v.push(format!("{name}: capability com acesso remoto"));
        }
        for id in permission_ids(cap) {
            if FORBIDDEN_PREFIXES.iter().any(|p| id.starts_with(p)) {
                v.push(format!("{name}: permissão proibida {id}"));
            }
        }
    }
    v
}

// ----- testes do verificador (casos sintéticos) -----

fn good_conf() -> Value {
    serde_json::json!({
        "app": { "security": { "csp": REQUIRED_CSP } },
        "plugins": { "updater": {} }
    })
}

fn cap(perms: Value) -> Vec<(String, Value)> {
    vec![(
        "x.json".into(),
        serde_json::json!({ "identifier": "x", "windows": ["main"], "permissions": perms }),
    )]
}

#[test]
fn verificador_aceita_configuracao_minima() {
    assert_eq!(
        violations(&good_conf(), &cap(serde_json::json!(["core:default"]))),
        Vec::<String>::new()
    );
}

#[test]
fn verificador_pega_permissoes_proibidas() {
    for p in [
        "fs:default",
        "shell:allow-open",
        "http:default",
        "opener:allow-open-url",
        "fs:allow-read-file",
    ] {
        let caps = cap(serde_json::json!(["core:default", p]));
        assert_eq!(violations(&good_conf(), &caps).len(), 1, "{p}");
        let caps = cap(serde_json::json!([{ "identifier": p, "allow": [{ "path": "**" }] }]));
        assert_eq!(violations(&good_conf(), &caps).len(), 1, "{p} em objeto");
    }
}

#[test]
fn verificador_pega_csp_frouxa() {
    let mut c = good_conf();
    c["app"]["security"]["csp"] =
        Value::String(format!("{REQUIRED_CSP}; style-src 'self' 'unsafe-inline'"));
    assert!(violations(&c, &[]).iter().any(|e| e.contains("unsafe-")));

    let mut c = good_conf();
    c["app"]["security"]["devCsp"] = Value::String("script-src 'self' 'unsafe-eval'".into());
    assert!(violations(&c, &[]).iter().any(|e| e.contains("devCsp")));

    let mut c = good_conf();
    c["app"]["security"]["csp"] =
        serde_json::json!({ "default-src": "'self'", "script-src": ["'unsafe-eval'"] });
    assert!(violations(&c, &[]).iter().any(|e| e.contains("unsafe-")));

    let mut c = good_conf();
    c["app"]["security"]["csp"] = Value::Null;
    assert!(violations(&c, &[])
        .iter()
        .any(|e| e.contains("csp diferente")));
}

#[test]
fn verificador_pega_capability_inline_remota_e_plugins() {
    let mut c = good_conf();
    c["app"]["security"]["capabilities"] =
        serde_json::json!([{ "identifier": "i", "permissions": ["shell:default"] }]);
    assert_eq!(violations(&c, &[]).len(), 1);

    let caps = vec![(
        "r.json".to_string(),
        serde_json::json!({ "identifier": "r", "remote": { "urls": ["https://*"] }, "permissions": [] }),
    )];
    assert_eq!(violations(&good_conf(), &caps).len(), 1);

    let mut c = good_conf();
    c["plugins"]["fs"] = serde_json::json!({});
    assert_eq!(violations(&c, &[]).len(), 1);

    let mut c = good_conf();
    c["app"]["withGlobalTauri"] = Value::Bool(true);
    assert_eq!(violations(&c, &[]).len(), 1);
}

// ----- os arquivos reais -----

#[test]
fn configuracao_real_passa_no_guarda() {
    let caps = capability_files();
    assert!(!caps.is_empty(), "nenhuma capability encontrada");
    assert_eq!(violations(&conf(), &caps), Vec::<String>::new());
}

#[test]
fn capability_real_so_tem_o_permitido() {
    let caps = capability_files();
    assert_eq!(caps.len(), 1, "capability nova precisa ser revisada aqui");
    let (_, c) = &caps[0];
    assert_eq!(c["windows"], serde_json::json!(["main"]));
    let mut perms = permission_ids(c);
    perms.sort();
    assert_eq!(
        perms,
        [
            "allow-app-version",
            "allow-get-settings",
            "allow-set-settings",
            "core:default"
        ]
    );
}

#[test]
fn tauri_conf_tem_os_valores_exigidos() {
    let c = conf();
    assert_eq!(c["identifier"], "br.com.castelobruxo.cbmp");
    assert_eq!(c["productName"], "CB Multiplayer");
    assert_eq!(c["bundle"]["targets"], serde_json::json!(["nsis"]));
    // Build comum sem chave privada; só o workflow de release liga e assina.
    assert_eq!(c["bundle"]["createUpdaterArtifacts"], false);

    let w = &c["app"]["windows"][0];
    assert_eq!(w["label"], "main");
    assert_eq!(
        (w["width"].as_u64(), w["height"].as_u64()),
        (Some(1100), Some(700))
    );
    assert_eq!(
        (w["minWidth"].as_u64(), w["minHeight"].as_u64()),
        (Some(900), Some(600))
    );

    assert_eq!(
        c["plugins"]["deep-link"]["desktop"]["schemes"],
        serde_json::json!(["cbmp"])
    );

    let up = &c["plugins"]["updater"];
    let pubkey = std::fs::read_to_string(root().join("keys/update.pub")).unwrap();
    assert_eq!(
        up["pubkey"].as_str(),
        Some(pubkey.trim()),
        "pubkey do tauri.conf.json difere de keys/update.pub"
    );
    let endpoints = up["endpoints"].as_array().unwrap();
    assert_eq!(endpoints.len(), 1);
    let ep = endpoints[0].as_str().unwrap();
    assert!(ep.starts_with("https://"), "{ep}");
    assert!(
        ep.ends_with("/v1/launcher/update/{{target}}/{{arch}}/{{current_version}}"),
        "{ep}"
    );
    // Mesmo endpoint que o Rust monta para o Hub padrão (o `Url` guarda `{{ }}` codificado).
    let built =
        cbmp_launcher_lib::update::endpoint(cbmp_launcher_lib::settings::DEFAULT_HUB_URL).unwrap();
    assert_eq!(ep, built.as_str().replace("%7B", "{").replace("%7D", "}"));
}

#[test]
fn comandos_do_build_rs_batem_com_a_capability() {
    let build = std::fs::read_to_string(root().join("build.rs")).unwrap();
    let caps = capability_files();
    let perms = permission_ids(&caps[0].1);
    for cmd in ["get_settings", "set_settings", "app_version"] {
        assert!(
            build.contains(&format!("\"{cmd}\"")),
            "{cmd} fora do build.rs"
        );
        let perm = format!("allow-{}", cmd.replace('_', "-"));
        assert!(perms.contains(&perm), "{perm} fora da capability");
    }
}
