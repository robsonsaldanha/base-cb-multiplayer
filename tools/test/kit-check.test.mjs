import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { scanForSecrets } from '../lib/kit-check.mjs';

function tree(files) {
  const root = mkdtempSync(join(tmpdir(), 'kit-check-'));
  for (const [rel, content] of Object.entries(files)) {
    const p = join(root, ...rel.split('/'));
    mkdirSync(dirname(p), { recursive: true });
    writeFileSync(p, content);
  }
  return { root, files: Object.keys(files), done: () => rmSync(root, { recursive: true, force: true }) };
}

// Textos montados em partes: este arquivo de teste não pode ele mesmo parecer um segredo.
const TAURI_KEY_B64 = Buffer.from(`untrusted comment: rsign encrypted ${'secret'} key\nRWRTY0Iy${'A'.repeat(40)}\n`).toString('base64');
const PEM = `-----BEGIN ${'PRIVATE'} KEY-----\n${'M'.repeat(64)}\n-----END PRIVATE KEY-----\n`;

test('kit limpo passa (cabeçalho PEM sem chave, como nas DLLs de TLS, não é achado)', () => {
  const t = tree({
    'hub/src/main.ts': 'console.log(1)',
    'keys/update.pub': 'dW50cnVzdGVkIGNvbW1lbnQ6IG1pbmlzaWduIHB1YmxpYyBrZXk=',
    'bin/cb-server/libssl.dll': Buffer.from(`\0-----BEGIN ${'PRIVATE'} KEY-----\0\0\n-----END PRIVATE KEY-----\0`, 'latin1'),
    'tools/dev-env.mjs': 'const env = { TAURI_SIGNING_PRIVATE_KEY: keyText };',
  });
  try { assert.deepEqual(scanForSecrets(t.root, t.files), []); } finally { t.done(); }
});

test('acha chave do Tauri, PEM com material, token, .env, .key e dados de execução', () => {
  const t = tree({
    'a/update.key': 'x',
    'b/copia.txt': TAURI_KEY_B64,
    'c/cert.txt': PEM,
    'd/.env': 'X=1',
    'e/notas.md': `token ${'ghp'}_${'a'.repeat(36)}`,
    'dados/hub-data/hub.db': 'x',
    'dados/state.json': '{}',
    'f/cfg.ps1': `$env:DISCORD_CLIENT_SECRET = "abcdefghijkl"`,
    'releases/launcher-v0.1.0/.source-stamp': 'abc',
  });
  try {
    const found = scanForSecrets(t.root, t.files).join('\n');
    for (const want of ['a/update.key: arquivo de chave', 'b/copia.txt: chave privada do Tauri', 'c/cert.txt: chave privada PEM',
      'd/.env: arquivo .env', 'e/notas.md: token do GitHub', 'dados/hub-data/hub.db: dados', 'dados/state.json: estado',
      'f/cfg.ps1: segredo atribuído', '.source-stamp: sobra']) {
      assert.ok(found.includes(want), `faltou "${want}" em:\n${found}`);
    }
  } finally { t.done(); }
});
