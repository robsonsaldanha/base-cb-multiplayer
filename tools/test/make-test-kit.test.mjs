// Recusas do tools/make-test-kit.mjs que não dependem de build (só Windows + Node 24, como o próprio script).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT = resolve(dirname(fileURLToPath(import.meta.url)), '..', 'make-test-kit.mjs');
const skip = process.platform !== 'win32' || process.versions.node.split('.')[0] !== '24' ? 'só no Windows com Node 24' : false;

function make(args) {
  return spawnSync(process.execPath, [SCRIPT, ...args], { encoding: 'utf8', timeout: 60_000 });
}

test('recusa pasta de releases vazia', { skip }, () => {
  const rel = mkdtempSync(join(tmpdir(), 'kit-rel-'));
  try {
    const r = make(['--releases', rel, '--out', join(rel, 'x.zip')]);
    assert.equal(r.status, 1);
    assert.match(r.stderr, /falta .*launcher-v0\.1\.0.*rode "node tools\/dev-env\.mjs --build-releases"/s);
  } finally { rmSync(rel, { recursive: true, force: true }); }
});

test('recusa release do launcher gerado de outro código (carimbo diferente)', { skip }, () => {
  const rel = mkdtempSync(join(tmpdir(), 'kit-rel-'));
  try {
    for (const v of ['0.1.0', '0.1.1']) {
      const d = join(rel, `launcher-v${v}`);
      mkdirSync(d);
      const exe = `CB Multiplayer (Teste local)_${v}_x64-setup.exe`;
      for (const f of [exe, `${exe}.sig`, 'notes.txt']) writeFileSync(join(d, f), 'x');
      writeFileSync(join(d, '.source-stamp'), `${'0'.repeat(64)}\n`);
    }
    const r = make(['--releases', rel, '--out', join(rel, 'x.zip')]);
    assert.equal(r.status, 1);
    assert.match(r.stderr, /launcher-v0\.1\.0 foi gerado de outro código do launcher \(carimbo diferente\)/);
  } finally { rmSync(rel, { recursive: true, force: true }); }
});
