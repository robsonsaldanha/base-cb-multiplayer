import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const FILE = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', '.github', 'workflows', 'mod.yml');
const text = () => readFileSync(FILE, 'utf8');
const lines = () => text().split(/\r?\n/);
const indentOf = (s) => s.match(/^\s*/)[0].length;

test('o workflow existe no caminho exato', () => assert.ok(existsSync(FILE)));

test('push na main com os cinco caminhos', () => {
  const t = text();
  assert.match(t, /push:\s*\n\s+branches: \[main\]/);
  for (const p of ['client/**', 'proto/**', 'tools/package-mod.ps1', 'tools/check-mod-zip.mjs', '.github/workflows/mod.yml']) {
    assert.ok(t.includes(`- '${p}'`), `caminho ${p}`);
  }
});

test('workflow_dispatch sem inputs', () => {
  const t = text();
  assert.match(t, /^\s*workflow_dispatch:\s*$/m);
  assert.doesNotMatch(t, /inputs:/);
});

test('sem pull_request nem pull_request_target', () => {
  assert.doesNotMatch(text(), /pull_request/);
});

test('checkout do repositório sem ref:', () => {
  const l = lines();
  const idx = l.map((x, i) => (/uses:\s*actions\/checkout@/.test(x) ? i : -1)).filter((i) => i >= 0);
  assert.ok(idx.length >= 1);
  for (const i of idx) {
    // o passo vai até a próxima linha "- " no mesmo nível do "- uses:" (ou "- name:") do passo
    let start = i;
    while (start > 0 && !/^\s*- /.test(l[start])) start--;
    const stepIndent = indentOf(l[start]);
    for (let j = i + 1; j < l.length; j++) {
      if (l[j].trim() !== '' && indentOf(l[j]) <= stepIndent) break;
      assert.doesNotMatch(l[j], /^\s*ref:/, `ref: no checkout (linha ${j + 1})`);
    }
  }
});

test('windows-2025, permissões mínimas e artefato cbmp-mod', () => {
  const t = text();
  assert.match(t, /runs-on: windows-2025/);
  assert.match(t, /permissions:\s*\n\s+contents: read/);
  assert.match(t, /name: cbmp-mod\b/);
  assert.match(t, /path: dist\/cbmp-mod\.zip/);
  assert.match(t, /if-no-files-found: error/);
});

test('avisa quando o token não está configurado', () => {
  assert.ok(text().includes('::notice title=mod.yml::UE4SS_SUBMODULES_TOKEN não configurado: o submódulo privado UEPseudo não pode ser baixado. Build do mod pulado; nenhum artefato cbmp-mod foi gerado.'));
});

test('o segredo só entra por env:, nunca dentro de run:', () => {
  const l = lines();
  let inRun = false; let runIndent = 0;
  l.forEach((line, i) => {
    if (inRun && line.trim() !== '' && indentOf(line) <= runIndent) inRun = false;
    const m = line.match(/^(\s*)(- )?run:\s*(.*)$/);
    if (m) {
      inRun = true; runIndent = m[1].length + (m[2] ? 2 : 0);
      assert.doesNotMatch(m[3], /secrets\./, `segredo em run: (linha ${i + 1})`);
      return;
    }
    if (inRun) assert.doesNotMatch(line, /secrets\./, `segredo em run: (linha ${i + 1})`);
  });
  assert.match(text(), /UE4SS_SUBMODULES_TOKEN: \$\{\{ secrets\.UE4SS_SUBMODULES_TOKEN \}\}/);
});

test('ações de terceiros fixadas por commit; vcpkg e UE4SS nos commits certos', () => {
  const t = text();
  // como em launcher.yml: as ações oficiais "actions/*" ficam por tag; qualquer outra, por commit
  for (const m of t.matchAll(/uses:\s*([^\s@]+)@(\S+)/g)) {
    if (!m[1].startsWith('actions/')) assert.match(m[2], /^[0-9a-f]{40}$/, `${m[1]} sem SHA`);
  }
  assert.ok(t.includes('ec48c11eae9448c8c6e7739d93e06fa2433aa95f'));
  assert.ok(t.includes('4fb9b615a24c724041ef23e2c4073f2610c4c6d0'));
});
