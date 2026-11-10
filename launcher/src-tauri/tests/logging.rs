//! Log em arquivo: nada de tokens, tickets ou códigos OAuth; rotação; níveis; e uma guarda que lê o código-fonte e
//! recusa chamadas de log que formatam variáveis com nome de segredo.

use cbmp_launcher_lib::logging::{redact, utc_timestamp, FileLogger, FILE_NAME};
use log::Log;
use std::path::Path;
use std::time::{Duration, UNIX_EPOCH};

// Valores no formato real (gerados aqui, nunca de uma conta de verdade).
const ACCESS: &str = "at_Q2hhdmVEZUFjZXNzb0RlVGVzdGVfMDEyMzQ1Njc4OWFi";
const REFRESH: &str = "rt_UmVmcmVzaERlVGVzdGVfbmFvVXNhcl85ODc2NTQzMjEw";
const SERVER_KEY: &str = "sk_Q2hhdmVEb1NlcnZpZG9yRGVUZXN0ZV8xMTIyMzM0NDU1";
const LOGIN_CODE: &str = "Y29kaWdvRGVMb2dpbjEyMzQ1Njc4OTBhYmNk";
const OAUTH_CODE: &str = "Zx81kQpLm2";
const STATE: &str = "c3RhdGVEb0xhdW5jaGVyRGVUZXN0ZV8wMTIzNDU2Nzg5YQ";
const VERIFIER: &str = "dmVyaWZpZXJEZVRlc3RlRG9MYXVuY2hlcl8wMTIzNDU2Nzg5YWJj";
const TICKET: &str = "eyJ2IjoxLCJzdWIiOiJ1c3JfMTIzIiwiYXVkIjoic3J2X3Rlc3QifQ.c2lnbmF0dXJhRWQyNTUxOURlVGVzdGVfMDEyMzQ1Njc4OWFiY2RlZg";

fn secrets() -> [&'static str; 9] {
    [
        ACCESS,
        REFRESH,
        SERVER_KEY,
        LOGIN_CODE,
        OAUTH_CODE,
        STATE,
        VERIFIER,
        TICKET,
        // A parte de assinatura do ticket sozinha também não pode sobrar.
        "c2lnbmF0dXJhRWQyNTUxOURlVGVzdGVfMDEyMzQ1Njc4OWFiY2RlZg",
    ]
}

fn lines_with_secrets() -> Vec<String> {
    vec![
        format!("deep link recebido: cbmp://auth?code={LOGIN_CODE}&state={STATE}"),
        format!(
            "GET http://127.0.0.1:8787/v1/auth/discord/callback?code={OAUTH_CODE}&state={STATE}"
        ),
        format!("authorization: Bearer {ACCESS}"),
        format!("Authorization: \"Bearer {ACCESS}\""),
        format!(
            r#"resposta {{"accessToken":"{ACCESS}","accessExpiresAt":1,"refreshToken":"{REFRESH}"}}"#
        ),
        format!(r#"pedido {{"code": "{LOGIN_CODE}", "codeVerifier": "{VERIFIER}"}}"#),
        format!("code_verifier={VERIFIER} code_challenge={STATE}"),
        format!("ticket {TICKET} expira em 300 s"),
        format!(r#"{{"ticket":"{TICKET}","expiresAt":1800000300}}"#),
        format!("connect.json: {{ \"ticket\": \"{TICKET}\" }}"),
        format!("chave do servidor {SERVER_KEY} e refresh {REFRESH} soltos no texto"),
        format!("token={ACCESS}; refresh_token={REFRESH}"),
        format!("linha\ncom quebra {ACCESS}\r\nforjada"),
    ]
}

#[test]
fn redact_tira_tokens_tickets_e_codigos() {
    for line in lines_with_secrets() {
        let out = redact(&line);
        for s in secrets() {
            assert!(
                !out.contains(s),
                "segredo sobrou em {out:?} (entrada {line:?})"
            );
        }
        assert!(out.contains("***"), "{out:?}");
        assert!(!out.contains('\n') && !out.contains('\r'), "{out:?}");
    }
}

#[test]
fn redact_mantem_o_que_e_util() {
    let sha = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
    for line in [
        "jogo aberto (Steam)".to_string(),
        r"pasta C:\Users\ana\AppData\Local\CBMultiplayer\cache\tmp".to_string(),
        "Hub em https://hub.cbmp.invalid/v1/servers (http 503)".to_string(),
        "atualização disponível: Some(\"0.1.1\")".to_string(),
        "baixando CB Multiplayer (Teste local)_0.1.1_x64-setup.exe".to_string(),
        format!("sha256 esperado {sha}"),
        "login falhou: bad_code".to_string(),
        "jogar em \"srv_AbC123\" falhou: server_unavailable".to_string(),
        "pubkey: chave embutida".to_string(),
    ] {
        assert_eq!(redact(&line), line);
    }
}

#[test]
fn data_utc() {
    assert_eq!(utc_timestamp(UNIX_EPOCH), "1970-01-01T00:00:00Z");
    assert_eq!(
        utc_timestamp(UNIX_EPOCH + Duration::from_secs(1_800_000_000)),
        "2027-01-15T08:00:00Z"
    );
    assert_eq!(
        utc_timestamp(UNIX_EPOCH + Duration::from_secs(951_782_400 + 86_399)),
        "2000-02-29T23:59:59Z"
    );
}

fn read_all(dir: &Path) -> String {
    let mut all = String::new();
    for e in std::fs::read_dir(dir).unwrap() {
        all.push_str(&std::fs::read_to_string(e.unwrap().path()).unwrap());
    }
    all
}

fn record<'a>(
    level: log::Level,
    target: &'a str,
    args: std::fmt::Arguments<'a>,
) -> log::Record<'a> {
    log::Record::builder()
        .level(level)
        .target(target)
        .args(args)
        .build()
}

#[test]
fn arquivo_nunca_recebe_segredos() {
    let dir = tempfile::tempdir().unwrap();
    let logger = FileLogger::open(dir.path(), 1024 * 1024, 3).unwrap();
    for line in lines_with_secrets() {
        logger.log(&record(
            log::Level::Warn,
            "cbmp_launcher_lib::auth",
            format_args!("{line}"),
        ));
        // Dependências (reqwest, tauri, updater) também passam pela redação.
        logger.log(&record(
            log::Level::Error,
            "reqwest::connect",
            format_args!("{line}"),
        ));
    }
    logger.flush();
    let all = read_all(dir.path());
    assert!(
        all.lines().count() >= 2 * lines_with_secrets().len(),
        "{all}"
    );
    for s in secrets() {
        assert!(!all.contains(s), "segredo no arquivo de log: {s}");
    }
    assert!(all.contains("cbmp_launcher_lib::auth"));
}

#[test]
fn niveis_launcher_info_dependencias_warn() {
    let dir = tempfile::tempdir().unwrap();
    let logger = FileLogger::open(dir.path(), 1024 * 1024, 3).unwrap();
    logger.log(&record(
        log::Level::Info,
        "cbmp_launcher_lib::play",
        format_args!("info do launcher"),
    ));
    logger.log(&record(
        log::Level::Info,
        "tauri::app",
        format_args!("info de dependência"),
    ));
    logger.log(&record(
        log::Level::Debug,
        "reqwest::connect",
        format_args!("debug de dependência"),
    ));
    logger.log(&record(
        log::Level::Warn,
        "tauri_plugin_updater",
        format_args!("aviso de dependência"),
    ));
    let all = read_all(dir.path());
    assert!(
        all.contains("INFO  cbmp_launcher_lib::play: info do launcher"),
        "{all}"
    );
    assert!(all.contains("aviso de dependência"));
    assert!(!all.contains("info de dependência"));
    assert!(!all.contains("debug de dependência"));
}

#[test]
fn rotacao_por_tamanho_mantem_keep_arquivos() {
    let dir = tempfile::tempdir().unwrap();
    let logger = FileLogger::open(dir.path(), 2_000, 3).unwrap();
    for i in 0..400 {
        logger.write_line(
            log::Level::Info,
            "cbmp_launcher_lib::x",
            &format!("linha {i:04} {}", "x".repeat(40)),
        );
    }
    let mut names: Vec<String> = std::fs::read_dir(dir.path())
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    assert_eq!(
        names,
        [
            FILE_NAME.to_string(),
            format!("{FILE_NAME}.1"),
            format!("{FILE_NAME}.2"),
            format!("{FILE_NAME}.3")
        ]
    );
    for n in &names {
        let len = std::fs::metadata(dir.path().join(n)).unwrap().len();
        assert!(len <= 2_000, "{n}: {len}");
    }
    // A última linha está no atual; o `.1` tem as imediatamente anteriores.
    let cur = std::fs::read_to_string(dir.path().join(FILE_NAME)).unwrap();
    assert!(cur.contains("linha 0399"));
    let prev = std::fs::read_to_string(dir.path().join(format!("{FILE_NAME}.1"))).unwrap();
    assert!(!prev.contains("linha 0399") && prev.contains("linha 03"));

    // Reabrir com o arquivo já acima do limite gira na hora.
    std::fs::write(dir.path().join(FILE_NAME), vec![b'a'; 3_000]).unwrap();
    let again = FileLogger::open(dir.path(), 2_000, 3).unwrap();
    assert_eq!(std::fs::metadata(again.path()).unwrap().len(), 0);
    assert_eq!(
        std::fs::metadata(dir.path().join(format!("{FILE_NAME}.1")))
            .unwrap()
            .len(),
        3_000
    );
}

#[test]
fn linha_gigante_e_cortada() {
    let dir = tempfile::tempdir().unwrap();
    let logger = FileLogger::open(dir.path(), 1024 * 1024, 1).unwrap();
    logger.write_line(
        log::Level::Warn,
        "cbmp_launcher_lib::x",
        &"é".repeat(10_000),
    );
    let all = read_all(dir.path());
    assert!(all.len() < 5_000, "{}", all.len());
    assert!(all.trim_end().ends_with('…'));
}

// ----- guarda no código-fonte -----

/// Identificadores que nunca podem ir para um log (como argumento ou captura `{nome}` na string de formato).
const FORBIDDEN: [&str; 7] = [
    "token", "ticket", "verifier", "secret", "password", "refresh", "oauth",
];

/// Texto de cada chamada `log::…!(…)` em `src` (parênteses balanceados, strings respeitadas).
fn log_calls(src: &str) -> Vec<String> {
    let mut out = Vec::new();
    for mac in [
        "log::error!(",
        "log::warn!(",
        "log::info!(",
        "log::debug!(",
        "log::trace!(",
    ] {
        let mut from = 0;
        while let Some(pos) = src[from..].find(mac) {
            let start = from + pos + mac.len();
            let (mut depth, mut in_str, mut esc, mut end) = (1, false, false, start);
            for (i, c) in src[start..].char_indices() {
                if in_str {
                    match (esc, c) {
                        (true, _) => esc = false,
                        (false, '\\') => esc = true,
                        (false, '"') => in_str = false,
                        _ => {}
                    }
                    continue;
                }
                match c {
                    '"' => in_str = true,
                    '(' => depth += 1,
                    ')' => {
                        depth -= 1;
                        if depth == 0 {
                            end = start + i;
                            break;
                        }
                    }
                    _ => {}
                }
            }
            out.push(src[start..end].to_string());
            from = end;
        }
    }
    out
}

/// Argumentos fora das strings + capturas `{nome}` dentro delas, em minúsculas.
fn formatted_names(call: &str) -> String {
    let mut names = String::new();
    let mut in_str = false;
    let mut esc = false;
    let mut lit = String::new();
    for c in call.chars() {
        if in_str {
            match (esc, c) {
                (true, _) => esc = false,
                (false, '\\') => esc = true,
                (false, '"') => {
                    in_str = false;
                    for cap in lit.split('{').skip(1) {
                        if let Some(name) = cap.split(['}', ':']).next() {
                            names.push_str(name);
                            names.push(' ');
                        }
                    }
                    lit.clear();
                }
                _ => lit.push(c),
            }
            continue;
        }
        if c == '"' {
            in_str = true;
        } else {
            names.push(c);
        }
    }
    names.to_ascii_lowercase()
}

#[test]
fn guarda_nenhum_log_formata_segredos() {
    let src_dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let mut checked = 0;
    for e in std::fs::read_dir(&src_dir).unwrap() {
        let p = e.unwrap().path();
        if p.extension().and_then(|x| x.to_str()) != Some("rs") {
            continue;
        }
        let text = std::fs::read_to_string(&p).unwrap();
        for call in log_calls(&text) {
            checked += 1;
            let names = formatted_names(&call);
            for bad in FORBIDDEN {
                assert!(
                    !names.contains(bad),
                    "{}: log formata algo com '{bad}': log!({call})",
                    p.display()
                );
            }
        }
    }
    assert!(checked > 40, "só {checked} chamadas de log achadas");
}

#[test]
fn guarda_pega_log_com_segredo() {
    let bad =
        r#"fn f() { log::info!("sessão {}", tokens.access); log::warn!("abrindo {ticket}"); }"#;
    let calls = log_calls(bad);
    assert_eq!(calls.len(), 2);
    let names: Vec<String> = calls.iter().map(|c| formatted_names(c)).collect();
    assert!(names
        .iter()
        .any(|n| n.contains("token") && !n.contains("ticket")));
    assert!(names
        .iter()
        .any(|n| n.contains("ticket") && !n.contains("token")));
    let ok = r#"log::info!("ticket renovado antes de abrir");"#;
    assert!(!formatted_names(&log_calls(ok)[0]).contains("ticket"));
}
