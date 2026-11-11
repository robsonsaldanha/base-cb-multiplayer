// Modo kit do tools/dev-env.mjs: numa cópia mínima do kit (dev-env + lib + kit.json), sem Hub nem cb-server reais.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const TOOLS = resolve(dirname(fileURLToPath(import.meta.url)), '..');

function fakeKit(files = { 'releases/a.txt': 'conteúdo' }, kitJson = undefined) {
  const root = mkdtempSync(join(tmpdir(), 'kit-mode-'));
  mkdirSync(join(root, 'tools', 'lib'), { recursive: true });
  cpSync(join(TOOLS, 'dev-env.mjs'), join(root, 'tools', 'dev-env.mjs'));
  cpSync(join(TOOLS, 'lib'), join(root, 'tools', 'lib'), { recursive: true });
  const hashes = {};
  for (const [rel, content] of Object.entries(files)) {
    const p = join(root, ...rel.split('/'));
    mkdirSync(dirname(p), { recursive: true });
    writeFileSync(p, content);
    hashes[rel] = createHash('sha256').update(content).digest('hex');
  }
  writeFileSync(join(root, 'kit.json'), JSON.stringify(kitJson ?? { kind: 'cbmp-kit-teste-sub4', commit: 'abc1234', node: process.version, files: hashes }));
  return { root, done: () => rmSync(root, { recursive: true, force: true }) };
}

/** Roda o dev-env da cópia; a chave dev aponta para um caminho que não existe (o kit não pode precisar dela). */
function devEnv(root, args, env = {}) {
  const e = { ...process.env, CBMP_DEV_KEY: join(root, 'nao-existe', 'update.key'), ...env };
  delete e.CBMP_DEV_DIR;
  delete e.CBMP_CB_SERVER;
  return spawnSync(process.execPath, [join(root, 'tools', 'dev-env.mjs'), ...args], { cwd: root, env: e, encoding: 'utf8', timeout: 60_000 });
}

test('kit: --build-releases, --key e --force são recusados sem procurar a chave', () => {
  const k = fakeKit();
  try {
    let r = devEnv(k.root, ['--build-releases']);
    assert.equal(r.status, 1);
    assert.match(r.stderr, /kit de teste: os releases já vêm prontos/);
    assert.doesNotMatch(r.stderr, /chave privada dev não encontrada/);
    r = devEnv(k.root, ['--stop', '--key', 'x']);
    assert.equal(r.status, 1);
    assert.match(r.stderr, /--key\/--force não se aplicam/);
  } finally { k.done(); }
});

test('kit: a pasta de trabalho padrão é <kit>\\dados (dentro do kit é permitido)', () => {
  const k = fakeKit();
  try {
    const r = devEnv(k.root, ['--stop']);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /nada rodando/);
    assert.ok(existsSync(join(k.root, 'dados')));
  } finally { k.done(); }
});

test('kit: arquivo faltando ou alterado para o --run antes de subir qualquer coisa', () => {
  const k = fakeKit({ 'releases/a.txt': 'original', 'bin/b.dll': 'dll' });
  try {
    writeFileSync(join(k.root, 'releases', 'a.txt'), 'mexido');
    rmSync(join(k.root, 'bin', 'b.dll'));
    const r = devEnv(k.root, ['--run', '--hub-port', '1', '--game-port', '1']);
    assert.equal(r.status, 1);
    assert.match(r.stderr, /kit está incompleto ou corrompido/);
    assert.match(r.stderr, /releases\/a\.txt \(conteúdo diferente\)/);
    assert.match(r.stderr, /bin\/b\.dll \(faltando\)/);
    assert.doesNotMatch(r.stdout, /subindo o Hub/);
  } finally { k.done(); }
});

test('kit íntegro: passa da conferência e procura o cb-server do kit (bin\\cb-server)', () => {
  const k = fakeKit();
  try {
    const r = devEnv(k.root, ['--run']);
    assert.equal(r.status, 1);
    assert.match(r.stdout, /kit conferido: 1 arquivos ok \(commit abc1234/);
    assert.match(r.stderr, /cb-server não encontrado em .*bin[\\/]cb-server[\\/]cb-server\.exe/);
  } finally { k.done(); }
});

test('fora do kit (sem kit.json) o comportamento não muda: pasta de trabalho dentro do repositório é recusada', () => {
  const root = mkdtempSync(join(tmpdir(), 'repo-mode-'));
  try {
    mkdirSync(join(root, 'tools', 'lib'), { recursive: true });
    cpSync(join(TOOLS, 'dev-env.mjs'), join(root, 'tools', 'dev-env.mjs'));
    cpSync(join(TOOLS, 'lib'), join(root, 'tools', 'lib'), { recursive: true });
    const r = devEnv(root, ['--stop', '--dir', join(root, 'dados')]);
    assert.equal(r.status, 1);
    assert.match(r.stderr, /não pode ficar dentro do repositório/);
    const ok = devEnv(root, ['--build-releases', '--dir', join(tmpdir(), `fora-${process.pid}`)], { CBMP_DEV_KEY: join(tmpdir(), `fora-${process.pid}`, 'nao-existe.key') });
    assert.equal(ok.status, 1);
    assert.match(ok.stderr, /chave privada dev não encontrada/);
  } finally { rmSync(root, { recursive: true, force: true }); rmSync(join(tmpdir(), `fora-${process.pid}`), { recursive: true, force: true }); }
});

test('kit: kind errado em kit.json para o --run com mensagem clara', () => {
  const k = fakeKit({ 'releases/a.txt': 'x' }, { kind: 'outra-coisa', files: { 'releases/a.txt': '0'.repeat(64) } });
  try {
    const r = devEnv(k.root, ['--run']);
    assert.equal(r.status, 1);
    assert.match(r.stderr, /kit\.json não é de um kit "cbmp-kit-teste-sub4"/);
  } finally { k.done(); }
});

test('kit: Node diferente do usado na montagem só avisa', () => {
  const k = fakeKit({ 'releases/a.txt': 'x' }, {
    kind: 'cbmp-kit-teste-sub4', node: 'v99.0.0',
    files: { 'releases/a.txt': '2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881' },
  });
  try {
    const r = devEnv(k.root, ['--run']);
    assert.equal(r.status, 1); // para depois, no cb-server que a cópia não tem
    assert.match(r.stdout, /AVISO: o kit foi montado e testado com o Node 99\.x/);
    assert.match(r.stdout, /kit conferido: 1 arquivos ok/);
  } finally { k.done(); }
});

test('kit.json dentro de um repositório git não liga o modo kit (avisa e segue o modo normal)', () => {
  const k = fakeKit();
  const out = join(tmpdir(), `fora-git-${process.pid}`);
  try {
    mkdirSync(join(k.root, '.git'));
    const r = devEnv(k.root, ['--build-releases', '--dir', out], { CBMP_DEV_KEY: join(out, 'nao-existe.key') });
    assert.equal(r.status, 1);
    assert.match(r.stdout, /kit\.json na raiz do repositório ignorado/);
    assert.match(r.stderr, /chave privada dev não encontrada/);
    assert.ok(!existsSync(join(k.root, 'dados')), 'modo normal não usa <raiz>\dados');
  } finally { k.done(); rmSync(out, { recursive: true, force: true }); }
});
