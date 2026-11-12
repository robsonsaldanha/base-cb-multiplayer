// teste-online.mjs --build-releases: sem o release mod-p1-v0.2.0 na pasta de trabalho e sem --mod-zip, o erro diz como gerar
// o zip; com um zip ruim, recusa antes de assinar. Nada aqui assina ou compila (a chave é um arquivo falso fora do repo).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writeStoredZip } from '../lib/zip.mjs';

const SCRIPT = resolve(dirname(fileURLToPath(import.meta.url)), '..', 'teste-online.mjs');

function run(args) {
  const dir = mkdtempSync(join(tmpdir(), 'cbmp-teste-online-test-'));
  const key = join(dir, 'fake.key');
  writeFileSync(key, 'nao e uma chave');
  mkdirSync(join(dir, 'work', 'releases'), { recursive: true });
  const r = spawnSync(process.execPath, [SCRIPT, '--build-releases', '--dir', join(dir, 'work'), '--key', key, ...args], { encoding: 'utf8' });
  return { r, dir };
}

test('sem --mod-zip e sem o release 0.2.0: erro explica como gerar o zip', () => {
  const { r, dir } = run([]);
  try {
    assert.equal(r.status, 1);
    assert.match(r.stderr, /--mod-zip/);
    assert.match(r.stderr, /tools\/build\.ps1 -Target mod/);
    assert.match(r.stderr, /tools\/package-mod\.ps1/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test('zip do mod com problemas é recusado antes de assinar', () => {
  const dir0 = mkdtempSync(join(tmpdir(), 'cbmp-teste-online-zip-'));
  const zip = join(dir0, 'ruim.zip');
  writeFileSync(zip, writeStoredZip([['LEIA-ME.txt', 'x']]));
  const { r, dir } = run(['--mod-zip', zip]);
  try {
    assert.equal(r.status, 1);
    assert.match(r.stderr, /tem problemas/);
    assert.match(r.stderr, /falta cbmp-mod\.json/);
  } finally { rmSync(dir, { recursive: true, force: true }); rmSync(dir0, { recursive: true, force: true }); }
});
