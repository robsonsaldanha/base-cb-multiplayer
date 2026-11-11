// Varredura do kit de teste antes de compactar (tools/make-test-kit.mjs): nenhum arquivo com nome de chave/segredo/
// dados de execução e nenhum conteúdo com cara de chave privada ou token. Falha fechado: qualquer achado para o kit.

import { readFileSync } from 'node:fs';
import { join } from 'node:path';

/** Nomes proibidos (caminho relativo com `/`). */
export const FORBIDDEN_NAMES = [
  [/\.(key|pem|p12|pfx)$/i, 'arquivo de chave'],
  [/(^|\/)\.env(\..*)?$/i, 'arquivo .env'],
  [/(^|\/)\.npmrc$/i, '.npmrc (pode ter token)'],
  [/(^|\/)(hub-data|dados)\//, 'dados de execução do Hub/ambiente'],
  [/(^|\/)(state|pids)\.json$/, 'estado do dev-env (tem a chave do servidor)'],
  [/(^|\/)server\.cfg$/, 'config do cb-server (tem a chave do servidor)'],
  [/(^|\/)\.source-stamp$/, 'sobra do build'],
  [/(^|\/)\.git(\/|$)/, 'pasta .git'],
];

/** Conteúdo proibido (procurado em todos os arquivos, texto ou binário). */
export const FORBIDDEN_CONTENT = [
  // Chave privada do Tauri/minisign: texto e o base64 do texto (o arquivo do Tauri é base64 do arquivo do minisign).
  [/rsign encrypted secret key|minisign encrypted secret key/, 'chave privada minisign'],
  [/dW50cnVzdGVkIGNvbW1lbnQ6IHJzaWduIGVuY3J5cHRlZCBzZWNyZXQga2V5|dW50cnVzdGVkIGNvbW1lbnQ6IG1pbmlzaWduIGVuY3J5cHRlZCBzZWNyZXQga2V5/, 'chave privada do Tauri (base64)'],
  // Linha da chave secreta minisign: "Ed" + "Sc" + "B2" em base64.
  [/RWRTY0Iy[A-Za-z0-9+/]{20,}/, 'linha de chave secreta minisign'],
  // Cabeçalho seguido de material da chave (as DLLs de TLS/SSH têm o cabeçalho sozinho como constante do parser).
  [/-----BEGIN (RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----\r?\n[A-Za-z0-9+/=]{40,}/, 'chave privada PEM'],
  [/\bgh[pousr]_[A-Za-z0-9]{36}\b|\bgithub_pat_[A-Za-z0-9_]{22,}/, 'token do GitHub'],
  [/(DISCORD_CLIENT_SECRET|HUB_GITHUB_TOKEN|CBMP_UPDATE_KEY|TAURI_SIGNING_PRIVATE_KEY)\s*=\s*["']?[A-Za-z0-9+/_-]{8,}/, 'segredo atribuído'],
];

/**
 * Confere `files` (relativos a `root`, com `/`). Devolve a lista de problemas ("caminho: motivo"); vazia = ok.
 * `allow` (opcional) pula arquivos conhecidos por conter padrões inofensivos.
 */
export function scanForSecrets(root, files, allow = new Set()) {
  const out = [];
  for (const f of files) {
    for (const [re, why] of FORBIDDEN_NAMES) if (re.test(f)) out.push(`${f}: ${why}`);
    if (allow.has(f)) continue;
    const text = readFileSync(join(root, ...f.split('/'))).toString('latin1');
    for (const [re, why] of FORBIDDEN_CONTENT) if (re.test(text)) out.push(`${f}: ${why}`);
  }
  return out;
}
