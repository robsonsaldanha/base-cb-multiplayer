import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const text = readFileSync(resolve(dirname(fileURLToPath(import.meta.url)), '..', 'build.ps1'), 'utf8');

test('build.ps1 escolhe o gerador do CMake por detecção, sem fixar o VS 2026', () => {
  assert.match(text, /\$Generator = \$env:CBMP_CMAKE_GENERATOR/);
  assert.match(text, /-G \$Generator/);
  assert.doesNotMatch(text, /-G "Visual Studio/);
  assert.match(text, /vswhere/);
  assert.match(text, /\$major -eq 18\) \{ return "Visual Studio 18 2026" \}/);
  assert.match(text, /\$major -eq 17 -and \$minor -ge 13\) \{ return "Visual Studio 17 2022" \}/);
});

test('check-mod-zip.mjs rejeita --version/--protocol sem valor', async () => {
  const { spawnSync } = await import('node:child_process');
  const cli = resolve(dirname(fileURLToPath(import.meta.url)), '..', 'check-mod-zip.mjs');
  for (const args of [['x.zip', '--version'], ['x.zip', '--protocol'], ['x.zip', '--protocol', 'abc']]) {
    const r = spawnSync(process.execPath, [cli, ...args], { encoding: 'utf8' });
    assert.equal(r.status, 2, args.join(' '));
  }
});
