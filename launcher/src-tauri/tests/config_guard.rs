//! Guarda de segurança da configuração. Lê de verdade `tauri.conf.json`, `capabilities/**/*` (como o tauri-build),
//! `src/lib.rs` e `Cargo.toml`, e falha se:
//! - a UI ganhar `fs:`/`shell:`/`http:`/`opener:`, `core:default` ou permissões core perigosas (image/tray/menu,
//!   emitir eventos, alternar devtools);
//! - aparecer capability remota, em formato não verificado, ou o `assetProtocol` for ligado;
//! - a CSP mudar ou tiver `unsafe-`;
//! - um plugin fora da lista (deep-link, single-instance, updater, dialog) for registrado ou virar dependência direta;
//! - o updater ganhar opções `dangerous*` no build oficial;
//! - o overlay do build de teste local (`tauri.localtest.conf.json`) mexer em algo além de nome, identificador,
//!   janela e endpoint do updater no Hub local, ou a configuração mesclada falhar em qualquer regra acima;
//! - o overlay do build de teste online (`tauri.onlinetest.conf.json`) mexer em algo além de nome, identificador,
//!   janela e endpoint https do updater no Hub de teste, ou ligar qualquer opção `dangerous*`;
//! - a feature `local-test` ou `online-test` entrar no padrão do Cargo.

use serde_json::Value;
use std::path::{Path, PathBuf};

const REQUIRED_CSP: &str =
    "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; connect-src ipc: http://ipc.localhost";

const FORBIDDEN_PREFIXES: [&str; 7] = [
    "fs:",
    "shell:",
    "http:",
    "opener:",
    "core:image",
    "core:tray",
    "core:menu",
];
const FORBIDDEN_EXACT: [&str; 6] = [
    "core:default",
    "core:event:default",
    "core:event:allow-emit",
    "core:event:allow-emit-to",
    "core:webview:default",
    "core:webview:allow-internal-toggle-devtools",
];
const FORBIDDEN_PLUGINS: [&str; 4] = ["fs", "shell", "http", "opener"];
/// Plugins permitidos (nome do crate sem `tauri_plugin_`, com `_`).
const ALLOWED_PLUGINS: [&str; 4] = ["deep_link", "single_instance", "updater", "dialog"];

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn read(p: &Path) -> String {
    std::fs::read_to_string(p).unwrap_or_else(|e| panic!("ler {}: {e}", p.display()))
}

fn conf() -> Value {
    serde_json::from_str(&read(&root().join("tauri.conf.json"))).expect("tauri.conf.json inválido")
}

fn localtest_overlay() -> Value {
    serde_json::from_str(&read(&root().join("tauri.localtest.conf.json")))
        .expect("tauri.localtest.conf.json inválido")
}

fn onlinetest_overlay() -> Value {
    serde_json::from_str(&read(&root().join("tauri.onlinetest.conf.json")))
        .expect("tauri.onlinetest.conf.json inválido")
}

/// JSON Merge Patch (RFC 7396), como a CLI do Tauri aplica `--config`: objetos mesclados, o resto substituído.
fn merge_patch(base: &mut Value, patch: &Value) {
    match (base, patch) {
        (Value::Object(b), Value::Object(p)) => {
            for (k, v) in p {
                if v.is_null() {
                    b.remove(k);
                } else {
                    merge_patch(b.entry(k.clone()).or_insert(Value::Null), v);
                }
            }
        }
        (b, p) => *b = p.clone(),
    }
}

/// Caminhos (folhas ou subárvores inteiras) que o overlay de teste local pode definir.
const LOCALTEST_ALLOWED: [&str; 8] = [
    "$schema",
    "productName",
    "mainBinaryName",
    "identifier",
    "app.windows",
    "bundle.shortDescription",
    "plugins.updater.endpoints",
    "plugins.updater.dangerousInsecureTransportProtocol",
];

/// Caminhos definidos pelo overlay que não estão na lista.
fn overlay_violations(overlay: &Value) -> Vec<String> {
    fn walk(v: &Value, path: String, out: &mut Vec<String>) {
        if LOCALTEST_ALLOWED.contains(&path.as_str()) {
            return;
        }
        match v {
            Value::Object(o) if !o.is_empty() => {
                for (k, child) in o {
                    let p = if path.is_empty() {
                        k.clone()
                    } else {
                        format!("{path}.{k}")
                    };
                    walk(child, p, out);
                }
            }
            _ => out.push(format!("overlay de teste local define {path}")),
        }
    }
    let mut out = Vec::new();
    walk(overlay, String::new(), &mut out);
    out
}

/// Regras do updater: no build oficial nenhuma opção `dangerous*`; no de teste local só
/// `dangerousInsecureTransportProtocol`, com todos os endpoints em `http://` de loopback.
fn updater_violations(conf: &Value, local_test: bool) -> Vec<String> {
    let mut v = Vec::new();
    let up = &conf["plugins"]["updater"];
    for key in [
        "dangerousAcceptInvalidCerts",
        "dangerousAcceptInvalidHostnames",
        "allowDowngrades",
    ] {
        if !up[key].is_null() && up[key] != Value::Bool(false) {
            v.push(format!("updater.{key} ligado"));
        }
    }
    let insecure = up["dangerousInsecureTransportProtocol"] == Value::Bool(true);
    if insecure && !local_test {
        v.push("updater.dangerousInsecureTransportProtocol no build oficial".into());
    }
    for ep in up["endpoints"].as_array().into_iter().flatten() {
        let ep = ep.as_str().unwrap_or("");
        let ok = ep.starts_with("https://")
            || (local_test
                && (ep.starts_with("http://127.0.0.1:") || ep.starts_with("http://[::1]:")));
        if !ok {
            v.push(format!("endpoint do updater recusado: {ep}"));
        }
    }
    v
}

// ----- leitura das capabilities (mesmo alcance do tauri-build: `capabilities/**/*`) -----

/// Capabilities encontradas e problemas (arquivo em formato que o guarda não verifica, ou ilegível).
fn load_capabilities(dir: &Path) -> (Vec<(String, Value)>, Vec<String>) {
    let mut caps = Vec::new();
    let mut problems = Vec::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for entry in std::fs::read_dir(&d).unwrap() {
            let p = entry.unwrap().path();
            if p.is_dir() {
                stack.push(p);
                continue;
            }
            let name = p
                .strip_prefix(dir)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/");
            let text = read(&p);
            let parsed: Result<Value, String> =
                match p.extension().and_then(|e| e.to_str()).unwrap_or("") {
                    "json" => serde_json::from_str(&text).map_err(|e| e.to_string()),
                    "toml" => toml::from_str(&text).map_err(|e| e.to_string()),
                    other => Err(format!("formato não verificado pelo guarda (.{other})")),
                };
            match parsed {
                // Um arquivo pode ter uma capability ou uma lista delas.
                Ok(Value::Array(list)) => {
                    caps.extend(list.into_iter().map(|c| (name.clone(), c)));
                }
                Ok(Value::Object(mut o)) if o.contains_key("capabilities") => {
                    if let Some(Value::Array(list)) = o.remove("capabilities") {
                        caps.extend(list.into_iter().map(|c| (name.clone(), c)));
                    }
                }
                Ok(v) => caps.push((name, v)),
                Err(e) => problems.push(format!("{name}: {e}")),
            }
        }
    }
    caps.sort_by(|a, b| a.0.cmp(&b.0));
    (caps, problems)
}

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

fn forbidden_permission(id: &str) -> bool {
    FORBIDDEN_PREFIXES.iter().any(|p| id.starts_with(p)) || FORBIDDEN_EXACT.contains(&id)
}

// ----- verificadores -----

/// Violações na configuração e nas capabilities (vazio = ok).
fn violations(conf: &Value, caps: &[(String, Value)]) -> Vec<String> {
    let mut v = Vec::new();
    let security = &conf["app"]["security"];

    for key in ["csp", "devCsp"] {
        let text = match &security[key] {
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
    if security["assetProtocol"]["enable"].as_bool() == Some(true) {
        v.push("assetProtocol ligado (UI leria arquivos por asset://)".into());
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
            if forbidden_permission(&id) {
                v.push(format!("{name}: permissão proibida {id}"));
            }
        }
    }
    v
}

/// Plugins registrados no código (`tauri_plugin_<nome>`) ou dependências diretas `tauri-plugin-*` fora da lista.
fn plugin_violations(lib_rs: &str, cargo_toml: &str) -> Vec<String> {
    let mut v = Vec::new();
    for (i, _) in lib_rs.match_indices("tauri_plugin_") {
        let name: String = lib_rs[i + "tauri_plugin_".len()..]
            .chars()
            .take_while(|c| c.is_ascii_alphanumeric() || *c == '_')
            .collect();
        if !ALLOWED_PLUGINS.contains(&name.as_str()) {
            v.push(format!("lib.rs: plugin fora da lista: tauri_plugin_{name}"));
        }
    }
    let manifest: toml::Table = toml::from_str(cargo_toml).expect("Cargo.toml inválido");
    for section in ["dependencies", "build-dependencies"] {
        let Some(deps) = manifest.get(section).and_then(|d| d.as_table()) else {
            continue;
        };
        for dep in deps.keys() {
            if let Some(name) = dep.strip_prefix("tauri-plugin-") {
                if !ALLOWED_PLUGINS.contains(&name.replace('-', "_").as_str()) {
                    v.push(format!(
                        "Cargo.toml [{section}]: plugin fora da lista: {dep}"
                    ));
                }
            }
        }
    }
    v
}

// ----- testes do verificador (casos sintéticos e fixtures temporárias) -----

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
    let caps = cap(serde_json::json!([
        "core:event:allow-listen",
        "allow-app-version"
    ]));
    assert_eq!(violations(&good_conf(), &caps), Vec::<String>::new());
}

#[test]
fn verificador_pega_permissoes_proibidas() {
    for p in [
        "fs:default",
        "shell:allow-open",
        "http:default",
        "opener:allow-open-url",
        "fs:allow-read-file",
        "core:default",
        "core:image:default",
        "core:image:allow-from-path",
        "core:tray:allow-new",
        "core:menu:default",
        "core:event:default",
        "core:event:allow-emit",
        "core:event:allow-emit-to",
        "core:webview:default",
        "core:webview:allow-internal-toggle-devtools",
    ] {
        let caps = cap(serde_json::json!(["core:event:allow-listen", p]));
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
fn verificador_pega_asset_protocol_ligado() {
    let mut c = good_conf();
    c["app"]["security"]["assetProtocol"] = serde_json::json!({ "enable": false, "scope": [] });
    assert_eq!(violations(&c, &[]), Vec::<String>::new());
    c["app"]["security"]["assetProtocol"] =
        serde_json::json!({ "enable": true, "scope": ["$APPDATA/**"] });
    let v = violations(&c, &[]);
    assert_eq!(v.len(), 1);
    assert!(v[0].contains("assetProtocol"));
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

#[test]
fn fixture_capability_em_subpasta_toml_e_vista() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(
        dir.path().join("default.json"),
        r#"{ "identifier": "default", "permissions": ["allow-app-version"] }"#,
    )
    .unwrap();
    std::fs::create_dir_all(dir.path().join("extra/fundo")).unwrap();
    std::fs::write(
        dir.path().join("extra/fundo/fs.toml"),
        "identifier = \"esc\"\npermissions = [\"fs:default\"]\n",
    )
    .unwrap();
    std::fs::write(
        dir.path().join("extra/lista.json"),
        r#"[{ "identifier": "a", "permissions": ["core:tray:default"] }]"#,
    )
    .unwrap();
    let (caps, problems) = load_capabilities(dir.path());
    assert!(problems.is_empty(), "{problems:?}");
    assert_eq!(caps.len(), 3);
    let v = violations(&good_conf(), &caps);
    assert!(
        v.iter()
            .any(|e| e.contains("extra/fundo/fs.toml") && e.contains("fs:default")),
        "{v:?}"
    );
    assert!(
        v.iter()
            .any(|e| e.contains("extra/lista.json") && e.contains("core:tray")),
        "{v:?}"
    );
}

#[test]
fn fixture_capability_em_formato_nao_verificado_falha() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::create_dir(dir.path().join("sub")).unwrap();
    std::fs::write(
        dir.path().join("sub/x.json5"),
        "{ identifier: 'x', permissions: ['fs:default'] }",
    )
    .unwrap();
    std::fs::write(dir.path().join("quebrado.json"), "{ nao json").unwrap();
    let (caps, problems) = load_capabilities(dir.path());
    assert!(caps.is_empty());
    assert_eq!(problems.len(), 2, "{problems:?}");
}

#[test]
fn fixture_plugin_fora_da_lista_no_codigo_e_no_cargo() {
    let lib_ok = "tauri::Builder::default().plugin(tauri_plugin_dialog::init())\
                  .plugin(tauri_plugin_single_instance::init(|_, _, _| {}))";
    let cargo_ok = "[package]\nname = \"x\"\n[dependencies]\ntauri = \"2\"\ntauri-plugin-dialog = \"2\"\ntauri-plugin-deep-link = \"2\"\n";
    assert_eq!(plugin_violations(lib_ok, cargo_ok), Vec::<String>::new());

    let lib_bad = format!("{lib_ok}.plugin(tauri_plugin_shell::init())");
    let v = plugin_violations(&lib_bad, cargo_ok);
    assert_eq!(v.len(), 1);
    assert!(v[0].contains("tauri_plugin_shell"));

    let cargo_bad = format!("{cargo_ok}tauri-plugin-opener = \"2\"\n");
    let v = plugin_violations(lib_ok, &cargo_bad);
    assert_eq!(v.len(), 1);
    assert!(v[0].contains("tauri-plugin-opener"));
}

#[test]
fn verificador_do_overlay_e_do_updater() {
    assert!(overlay_violations(&serde_json::json!({
        "productName": "x", "identifier": "y", "app": { "windows": [{}] },
        "plugins": { "updater": { "endpoints": [] } }
    }))
    .is_empty());
    for bad in [
        serde_json::json!({ "app": { "security": { "csp": "default-src *" } } }),
        serde_json::json!({ "app": { "withGlobalTauri": true } }),
        serde_json::json!({ "plugins": { "updater": { "pubkey": "outra" } } }),
        serde_json::json!({ "plugins": { "shell": {} } }),
        serde_json::json!({ "bundle": { "createUpdaterArtifacts": true } }),
        serde_json::json!({ "build": { "devUrl": "http://evil" } }),
        serde_json::json!({ "version": "9.9.9" }),
    ] {
        assert_eq!(overlay_violations(&bad).len(), 1, "{bad}");
    }

    let mut c = good_conf();
    c["plugins"]["updater"] = serde_json::json!({
        "endpoints": ["https://hub/v1/x"], "dangerousInsecureTransportProtocol": true
    });
    assert_eq!(updater_violations(&c, false).len(), 1);
    assert!(updater_violations(&c, true).is_empty());
    c["plugins"]["updater"]["endpoints"] = serde_json::json!(["http://hub.exemplo:8787/x"]);
    assert_eq!(updater_violations(&c, true).len(), 1);
    c["plugins"]["updater"]["endpoints"] = serde_json::json!(["http://127.0.0.1:8787/x"]);
    assert!(updater_violations(&c, true).is_empty());
    assert_eq!(updater_violations(&c, false).len(), 2);
    c["plugins"]["updater"]["dangerousAcceptInvalidCerts"] = Value::Bool(true);
    assert_eq!(updater_violations(&c, true).len(), 1);

    let mut base = serde_json::json!({ "a": { "b": 1, "c": [1, 2] }, "d": 1 });
    merge_patch(
        &mut base,
        &serde_json::json!({ "a": { "c": [3] }, "d": null, "e": "x" }),
    );
    assert_eq!(
        base,
        serde_json::json!({ "a": { "b": 1, "c": [3] }, "e": "x" })
    );
}

// ----- os arquivos reais -----

fn real_capabilities() -> Vec<(String, Value)> {
    let (caps, problems) = load_capabilities(&root().join("capabilities"));
    assert!(problems.is_empty(), "{problems:?}");
    assert!(!caps.is_empty(), "nenhuma capability encontrada");
    caps
}

#[test]
fn configuracao_real_passa_no_guarda() {
    assert_eq!(
        violations(&conf(), &real_capabilities()),
        Vec::<String>::new()
    );
}

#[test]
fn plugins_reais_estao_na_lista() {
    let lib_rs = read(&root().join("src/lib.rs"));
    let cargo = read(&root().join("Cargo.toml"));
    assert_eq!(plugin_violations(&lib_rs, &cargo), Vec::<String>::new());
}

#[test]
fn capability_real_so_tem_o_permitido() {
    let caps = real_capabilities();
    assert_eq!(caps.len(), 1, "capability nova precisa ser revisada aqui");
    let (_, c) = &caps[0];
    assert_eq!(c["windows"], serde_json::json!(["main"]));
    let mut perms = permission_ids(c);
    perms.sort();
    assert_eq!(
        perms,
        [
            "allow-app-version",
            "allow-cached-servers",
            "allow-cancel-play",
            "allow-check-update",
            "allow-clear-cache",
            "allow-detect-game",
            "allow-get-settings",
            "allow-install-update",
            "allow-list-servers",
            "allow-login",
            "allow-logout",
            "allow-me",
            "allow-mod-admin-setup",
            "allow-mod-status",
            "allow-open-mod-logs",
            "allow-pick-game-dir",
            "allow-ping-servers",
            "allow-play",
            "allow-remove-mod",
            "allow-resolve-mod-conflict",
            "allow-set-settings",
            "allow-use-detected-game",
            "core:event:allow-listen",
            "core:event:allow-unlisten",
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
    let pubkey = read(&root().join("keys/update.pub"));
    assert_eq!(
        up["pubkey"].as_str(),
        Some(pubkey.trim()),
        "pubkey do tauri.conf.json difere de keys/update.pub"
    );
    // A versão anunciada pelo Hub tem de ser a assinada (sem rollback para um launcher antigo assinado).
    assert_eq!(up["requireSignedVersion"], serde_json::json!(true));
    assert!(up["allowDowngrades"].is_null() || up["allowDowngrades"] == serde_json::json!(false));
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
    let build = read(&root().join("build.rs"));
    let lib_rs = read(&root().join("src/lib.rs"));
    // Nenhum comando além da lista: cada `"x",` dentro de `.commands(&[...])` está nela.
    let listed: Vec<&str> = build
        .split(".commands(&[")
        .nth(1)
        .and_then(|rest| rest.split("])").next())
        .unwrap()
        .split(',')
        .map(|c| c.trim().trim_matches('"'))
        .filter(|c| !c.is_empty())
        .collect();
    assert_eq!(
        listed.len(),
        22,
        "comando novo precisa ser revisado aqui: {listed:?}"
    );
    let caps = real_capabilities();
    let perms = permission_ids(&caps[0].1);
    for cmd in [
        "get_settings",
        "set_settings",
        "app_version",
        "login",
        "logout",
        "me",
        "pick_game_dir",
        "detect_game",
        "use_detected_game",
        "play",
        "cancel_play",
        "clear_cache",
        "check_update",
        "install_update",
        "list_servers",
        "cached_servers",
        "ping_servers",
        "mod_status",
        "remove_mod",
        "resolve_mod_conflict",
        "open_mod_logs",
        "mod_admin_setup",
    ] {
        assert!(
            build.contains(&format!("\"{cmd}\"")),
            "{cmd} fora do build.rs"
        );
        assert!(
            lib_rs.contains(&format!("commands::{cmd}")),
            "{cmd} fora do generate_handler!"
        );
        let perm = format!("allow-{}", cmd.replace('_', "-"));
        assert!(perms.contains(&perm), "{perm} fora da capability");
    }
}

#[test]
fn updater_oficial_sem_opcoes_perigosas() {
    assert_eq!(updater_violations(&conf(), false), Vec::<String>::new());
}

#[test]
fn overlay_de_teste_local_so_muda_o_permitido_e_passa_no_guarda() {
    let overlay = localtest_overlay();
    assert_eq!(overlay_violations(&overlay), Vec::<String>::new());
    let base = conf();
    let mut merged = base.clone();
    merge_patch(&mut merged, &overlay);
    // Mesma CSP, capabilities, plugins e chave pública do oficial.
    assert_eq!(
        violations(&merged, &real_capabilities()),
        Vec::<String>::new()
    );
    assert_eq!(updater_violations(&merged, true), Vec::<String>::new());
    assert_eq!(merged["app"]["security"], base["app"]["security"]);
    assert_eq!(
        merged["plugins"]["updater"]["pubkey"],
        base["plugins"]["updater"]["pubkey"]
    );
    assert_eq!(
        merged["plugins"]["updater"]["requireSignedVersion"],
        serde_json::json!(true)
    );
    assert_eq!(merged["plugins"]["deep-link"], base["plugins"]["deep-link"]);
    assert_eq!(merged["bundle"]["createUpdaterArtifacts"], false);

    assert_eq!(merged["productName"], "CB Multiplayer (Teste local)");
    assert_eq!(merged["identifier"], "br.com.castelobruxo.cbmp.localtest");
    assert_ne!(merged["mainBinaryName"], base["mainBinaryName"]);
    // A janela continua a mesma (label e tamanhos que o resto do app espera); só o título muda.
    let (w, base_w) = (&merged["app"]["windows"][0], &base["app"]["windows"][0]);
    assert_eq!(merged["app"]["windows"].as_array().unwrap().len(), 1);
    // Janela inteira igual à oficial, só com outro título (nada de devtools, url, args extras…).
    let mut expected = base_w.clone();
    expected["title"] = serde_json::json!("CB Multiplayer (Teste local)");
    assert_eq!(w, &expected);

    // O endpoint é o que o Rust monta para o Hub local do build de teste.
    assert_eq!(
        cbmp_launcher_lib::profile::LOCAL_TEST_HUB_URL,
        "http://127.0.0.1:8787"
    );
    let eps = merged["plugins"]["updater"]["endpoints"]
        .as_array()
        .unwrap();
    assert_eq!(eps.len(), 1);
    let built = cbmp_launcher_lib::update::endpoint(cbmp_launcher_lib::profile::LOCAL_TEST_HUB_URL)
        .unwrap();
    assert_eq!(
        eps[0].as_str().unwrap(),
        built.as_str().replace("%7B", "{").replace("%7D", "}")
    );
}

#[test]
fn overlay_de_teste_online_so_muda_o_permitido_e_segue_as_regras_do_oficial() {
    let overlay = onlinetest_overlay();
    assert_eq!(overlay_violations(&overlay), Vec::<String>::new());
    assert!(
        overlay["plugins"]["updater"]["dangerousInsecureTransportProtocol"].is_null(),
        "o teste online é público: updater só em https"
    );
    let base = conf();
    let mut merged = base.clone();
    merge_patch(&mut merged, &overlay);
    assert_eq!(
        violations(&merged, &real_capabilities()),
        Vec::<String>::new()
    );
    // Regras do OFICIAL (false): nenhuma opção dangerous*, endpoint só https.
    assert_eq!(updater_violations(&merged, false), Vec::<String>::new());
    assert_eq!(merged["app"]["security"], base["app"]["security"]);
    assert_eq!(
        merged["plugins"]["updater"]["pubkey"],
        base["plugins"]["updater"]["pubkey"]
    );
    assert_eq!(
        merged["plugins"]["updater"]["requireSignedVersion"],
        serde_json::json!(true)
    );
    assert_eq!(merged["plugins"]["deep-link"], base["plugins"]["deep-link"]);
    assert_eq!(merged["bundle"]["createUpdaterArtifacts"], false);

    assert_eq!(merged["productName"], "CB Multiplayer (Teste)");
    assert_eq!(merged["identifier"], "br.com.castelobruxo.cbmp.teste");
    assert_eq!(merged["mainBinaryName"], "CBMultiplayerTeste");
    let (w, base_w) = (&merged["app"]["windows"][0], &base["app"]["windows"][0]);
    assert_eq!(merged["app"]["windows"].as_array().unwrap().len(), 1);
    let mut expected = base_w.clone();
    expected["title"] = serde_json::json!("CB Multiplayer (Teste)");
    assert_eq!(w, &expected);

    let hub = cbmp_launcher_lib::profile::ONLINE_TEST_HUB_URL;
    assert_eq!(hub, "https://cbmultiplayer.cntoficial.com.br");
    let eps = merged["plugins"]["updater"]["endpoints"]
        .as_array()
        .unwrap();
    assert_eq!(eps.len(), 1);
    let built = cbmp_launcher_lib::update::endpoint(hub).unwrap();
    assert_eq!(
        eps[0].as_str().unwrap(),
        built.as_str().replace("%7B", "{").replace("%7D", "}")
    );
}

#[test]
fn regra_de_rede_relaxada_por_perfil() {
    use cbmp_launcher_lib::profile::{
        relaxed_network_for, LOCAL_TEST, ONLINE_TEST, RELAXED_NETWORK,
    };
    // Release: oficial e teste online (público) SEMPRE estritos; só o teste local relaxa.
    assert!(
        !relaxed_network_for(false, false, false),
        "release oficial relaxado"
    );
    assert!(
        !relaxed_network_for(false, false, true),
        "release do teste online relaxado"
    );
    assert!(
        relaxed_network_for(false, true, false),
        "teste local deve aceitar loopback/LAN"
    );
    // Debug (cargo test / tauri dev) relaxa em qualquer perfil.
    assert!(relaxed_network_for(true, false, false));
    assert!(relaxed_network_for(true, false, true));
    // A constante usada pelo app vem da regra (não de uma conta paralela).
    assert_eq!(
        RELAXED_NETWORK,
        relaxed_network_for(cfg!(debug_assertions), LOCAL_TEST, ONLINE_TEST)
    );
    // E o código-fonte não tem outra definição escondida.
    let src: String = read(&root().join("src/profile.rs"))
        .chars()
        .filter(|c| !c.is_whitespace())
        .collect();
    assert!(
        src.contains("pubconstRELAXED_NETWORK:bool=relaxed_network_for(cfg!(debug_assertions),LOCAL_TEST,ONLINE_TEST);"),
        "RELAXED_NETWORK precisa vir de relaxed_network_for"
    );
    assert_eq!(src.matches("RELAXED_NETWORK:bool").count(), 1);
}

#[test]
fn feature_local_test_so_por_flag_explicita() {
    let manifest: toml::Table = toml::from_str(&read(&root().join("Cargo.toml"))).unwrap();
    let features = manifest["features"].as_table().expect("[features]");
    assert_eq!(features["local-test"], toml::Value::Array(vec![]));
    assert_eq!(features["online-test"], toml::Value::Array(vec![]));
    assert!(
        !features.contains_key("default"),
        "nenhuma feature no padrão (local-test/online-test só por flag explícita)"
    );
    assert_eq!(
        features.len(),
        2,
        "feature nova precisa ser revisada aqui: {features:?}"
    );
    // Pasta de dados: a oficial é `CBMultiplayer` (restrição global); a de teste local é outra.
    assert_eq!(
        cbmp_launcher_lib::paths::APP_DIR,
        if cfg!(feature = "local-test") {
            "CBMultiplayer-TesteLocal"
        } else if cfg!(feature = "online-test") {
            "CBMultiplayer-Teste"
        } else {
            "CBMultiplayer"
        }
    );
    let expected_hub = if cfg!(feature = "local-test") {
        cbmp_launcher_lib::profile::LOCAL_TEST_HUB_URL
    } else if cfg!(feature = "online-test") {
        cbmp_launcher_lib::profile::ONLINE_TEST_HUB_URL
    } else {
        cbmp_launcher_lib::settings::DEFAULT_HUB_URL
    };
    assert_eq!(cbmp_launcher_lib::settings::default_hub_url(), expected_hub);
    assert_eq!(
        cbmp_launcher_lib::auth::VAULT_SERVICE,
        if cfg!(feature = "local-test") {
            "CBMultiplayer (Teste local)"
        } else if cfg!(feature = "online-test") {
            "CBMultiplayer (Teste)"
        } else {
            "CBMultiplayer"
        }
    );
}

#[test]
fn valores_do_launcher_0_2_0() {
    use cbmp_launcher_lib::{launch, modact, modpkg, play, settings};
    assert_eq!(launch::CONNECT_TTL, std::time::Duration::from_secs(240));
    assert_eq!(launch::POINTER_FILE, "cbmp-run.json");
    assert_eq!(play::TICKET_RENEW_AFTER_SECS, 30);
    assert_eq!(modact::OVERRIDE_LINE, "cbmp\\atual\\ue4ss");
    assert_eq!(modpkg::LAYOUT, 2);
    assert_eq!(settings::SETTINGS_VERSION, 2);
    // Versão igual no Cargo.toml, no tauri.conf.json e na UI.
    let manifest: toml::Table = toml::from_str(&read(&root().join("Cargo.toml"))).unwrap();
    assert_eq!(manifest["package"]["version"].as_str(), Some("0.2.0"));
    assert_eq!(conf()["version"], "0.2.0");
    let ui: Value = serde_json::from_str(&read(&root().join("../ui/package.json"))).unwrap();
    assert_eq!(ui["version"], "0.2.0");
    assert_eq!(env!("CARGO_PKG_VERSION"), "0.2.0");
}

/// O texto do diálogo de conflito mostrado pela UI (`MOD_CONFLICT_EXPLANATION`) é exatamente o do Rust.
#[test]
fn explicacao_do_conflito_na_ui_e_o_texto_do_dialogo() {
    let ts = read(&root().join("../ui/src/lib/errors.ts"));
    let decl = ts
        .split("export const MOD_CONFLICT_EXPLANATION =")
        .nth(1)
        .and_then(|rest| rest.split(';').next())
        .expect("MOD_CONFLICT_EXPLANATION em errors.ts");
    // Concatenação de literais "…" + "…" (sem aspas dentro do texto).
    let text: String = decl
        .split('"')
        .skip(1)
        .step_by(2)
        .collect::<Vec<_>>()
        .concat()
        .replace("\\\\", "\\");
    assert_eq!(text, cbmp_launcher_lib::modact::CONFLICT_DIALOG);
}

/// O modo elevado é tratado em `main.rs` antes de qualquer coisa do Tauri, e só pelo primeiro argumento.
#[test]
fn main_trata_o_modo_elevado_antes_do_tauri() {
    let main_rs = read(&root().join("src/main.rs"));
    let entry = main_rs
        .find("admin_setup::entry(std::env::args_os())")
        .expect("main.rs chama admin_setup::entry");
    let run = main_rs
        .find("cbmp_launcher_lib::run()")
        .expect("main.rs chama run()");
    assert!(entry < run, "modo elevado depois do Tauri");
    assert!(main_rs.contains("std::process::exit(code)"));
    assert_eq!(cbmp_launcher_lib::admin_setup::FLAG, "--mod-admin-setup");
    assert_eq!(
        cbmp_launcher_lib::admin_setup::WAIT,
        std::time::Duration::from_secs(120)
    );
}
