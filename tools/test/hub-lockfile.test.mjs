import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { hubLockfile } from '../lib/hub-lockfile.mjs';

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');

test('hubLockfile: só better-sqlite3 e dependências, com versão e integridade do lock do repositório', () => {
  const repoLock = JSON.parse(readFileSync(join(REPO, 'package-lock.json'), 'utf8'));
  const { pkg, lock } = hubLockfile(repoLock);
  const bs = repoLock.packages['node_modules/better-sqlite3'];
  assert.deepEqual(pkg.dependencies, { 'better-sqlite3': bs.version });
  assert.equal(lock.lockfileVersion, 3);
  for (const [k, v] of Object.entries(lock.packages)) {
    if (k === '') continue;
    assert.equal(v.integrity, repoLock.packages[k].integrity, k);
    assert.equal(v.resolved, repoLock.packages[k].resolved, k);
    assert.ok(!('dev' in v), k);
  }
  assert.ok(lock.packages['node_modules/better-sqlite3']);
});

test('hubLockfile: entrada sem integridade é recusada', () => {
  assert.throws(() => hubLockfile({ packages: { 'node_modules/better-sqlite3': { version: '1.0.0', resolved: 'x' } } }), /integrity/);
});
