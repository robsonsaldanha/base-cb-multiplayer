#!/usr/bin/env node
// Monta o kit de teste "em casa" do subprojeto 4: um zip autossuficiente para testar o launcher num Windows 11 limpo
// que só tem o Node 24 (sem Git, Rust, Visual Studio, vcpkg nem a chave dev). Roteiro: docs/TESTES-EM-CASA-SUB4.md
// (vai no kit como LEIA-ME.md).
//
//   node tools/make-test-kit.mjs [opções]
//
// Pré-requisitos nesta máquina (a de desenvolvimento):
//   - releases de teste prontos e atuais: node tools/dev-env.mjs --build-releases (o carimbo do código do launcher
//     tem de bater com o código atual; senão o kit é recusado);
//   - cb-server compilado (preset windows-release) e mais novo que o último commit em server/, proto/, CMake e vcpkg;
//   - Node 24 (o node_modules do kit leva módulos nativos para a ABI desta versão, win-x64).
//
// Opções: --releases <pasta>  (padrão <CBMP_DEV_DIR ou %TEMP%\cbmp-dev-env>\releases)
//         --cb-server <exe>   (padrão build-native\windows-release\server\Release\cb-server.exe)
//         --out <zip>         (padrão dist\cbmp-kit-teste-sub4.zip)
//         --allow-stale-server  aceita cb-server mais velho que o código (não recomendado)
//         --keep-staging      não apaga a pasta temporária de montagem (para inspecionar)
//
// O que faz: copia para uma pasta temporária (fora do OneDrive) o Hub, packages/contracts, tools/dev-env.mjs,
// tools/kit-smoke.mjs, o hello-client, a chave PÚBLICA, o cb-server com DLLs (+ runtime do VC++ ao lado do exe) e os
// releases; instala as dependências do Hub (npm, mesmas versões do package-lock); confere tudo (assinaturas com
// keys/update.pub, versões, nenhuma chave privada/segredo/.env/dados do Hub); grava kit.json (sha256 de cada arquivo,
// conferido pelo dev-env no kit antes de subir) e compacta com o tar.exe do Windows. A chave privada nunca é lida.

import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync, lstatSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseModComment, parsePublicKey, tauriCommentFields, verifyMinisign } from './lib/minisign.mjs';
import { sourceStamp } from './lib/source-stamp.mjs';
import { scanForSecrets } from './lib/kit-check.mjs';

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const KIT_NAME = 'cbmp-kit-teste-sub4';
const PRODUCT = 'CB Multiplayer (Teste local)';
const LAUNCHER_VERSIONS = ['0.1.0', '0.1.1'];
const MOD = { protocol: 1, version: '0.1.0' };
const NODE_MAJOR = 24;
// Entradas do cb-server: binário mais velho que o último commit nelas é recusado.
const SERVER_INPUTS = ['server', 'proto', 'CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'vcpkg-overlays', 'vcpkg-triplets'];
// Runtime do VC++ que o cb-server e as DLLs do vcpkg importam (dumpbin /dependents); o resto (UCRT) vem com o Windows.
const VC_RUNTIME = ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'];

const log = (m) => console.log(`[make-test-kit] ${m}`);
function fail(m) { console.error(`\n[make-test-kit] ERRO: ${m}\n`); process.exit(1); }

function parseArgs(argv) {
  const o = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => { const v = argv[++i]; if (v === undefined) fail(`${a} precisa de um valor`); return v; };
    switch (a) {
      case '--releases': o.releases = val(); break;
      case '--cb-server': o.cbServer = val(); break;
      case '--out': o.out = val(); break;
      case '--allow-stale-server': o.allowStale = true; break;
      case '--keep-staging': o.keep = true; break;
      case '-h': case '--help': {
        const t = readFileSync(fileURLToPath(import.meta.url), 'utf8').split('\n');
        console.log(t.slice(1, t.findIndex((l) => l.startsWith('import '))).map((l) => l.replace(/^\/\/ ?/, '')).join('\n').trim());
        process.exit(0);
      }
      default: fail(`opção desconhecida: ${a}`);
    }
  }
  return o;
}

function git(args) {
  const r = spawnSync('git', args, { cwd: REPO, encoding: 'utf8' });
  if (r.status !== 0) fail(`git ${args.join(' ')} falhou: ${r.stderr}`);
  return r.stdout.trim();
}

function run(cmd, args, opts) {
  const r = spawnSync(cmd, args, { stdio: 'inherit', ...opts });
  if (r.error) fail(`não foi possível rodar ${cmd}: ${r.error.message}`);
  if (r.status !== 0) fail(`${cmd} ${args.join(' ')} terminou com código ${r.status}`);
}

const sha256File = (p) => createHash('sha256').update(readFileSync(p)).digest('hex');

/** Todos os arquivos sob `dir` (caminhos relativos com `/`); recusa links e junções. */
function walk(dir, base = dir, out = []) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, e.name);
    if (e.isSymbolicLink() || lstatSync(full).isSymbolicLink()) fail(`link/junção no kit: ${relative(base, full)}`);
    if (e.isDirectory()) walk(full, base, out);
    else if (e.isFile()) out.push(relative(base, full).split(sep).join('/'));
    else fail(`entrada estranha no kit: ${relative(base, full)}`);
  }
  return out;
}

/** Pasta Microsoft.VC14x.CRT mais nova do Visual Studio instalado (vswhere). */
function vcRuntimeDir() {
  const vswhere = join(process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)', 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
  if (!existsSync(vswhere)) fail('vswhere.exe não encontrado (Visual Studio instalado?)');
  const vs = spawnSync(vswhere, ['-latest', '-products', '*', '-property', 'installationPath'], { encoding: 'utf8' }).stdout.trim();
  const redist = join(vs, 'VC', 'Redist', 'MSVC');
  const cands = [];
  for (const v of existsSync(redist) ? readdirSync(redist) : []) {
    if (!/^\d+\.\d+\.\d+$/.test(v)) continue;
    const x64 = join(redist, v, 'x64');
    for (const c of existsSync(x64) ? readdirSync(x64) : []) {
      if (/^Microsoft\.VC\d+\.CRT$/.test(c) && VC_RUNTIME.every((f) => existsSync(join(x64, c, f)))) cands.push({ v, dir: join(x64, c) });
    }
  }
  const cmp = (a, b) => a.v.split('.').map(Number).reduce((r, n, i) => r || n - Number(b.v.split('.')[i]), 0);
  const best = cands.sort(cmp).at(-1);
  if (!best) fail(`runtime do VC++ (${VC_RUNTIME.join(', ')}) não encontrado em ${redist}`);
  return best;
}

// ------------------------------------------------------------------------------------------------------- conferências

function checkReleases(rel) {
  const stamp = sourceStamp(REPO);
  for (const v of LAUNCHER_VERSIONS) {
    const d = join(rel, `launcher-v${v}`);
    const exe = `${PRODUCT}_${v}_x64-setup.exe`;
    for (const f of [exe, `${exe}.sig`, 'notes.txt']) if (!existsSync(join(d, f))) fail(`falta ${join(d, f)}: rode "node tools/dev-env.mjs --build-releases"`);
    const st = existsSync(join(d, '.source-stamp')) ? readFileSync(join(d, '.source-stamp'), 'utf8').trim() : '';
    if (st !== stamp) fail(`launcher-v${v} foi gerado de outro código do launcher (carimbo diferente): rode "node tools/dev-env.mjs --build-releases"`);
  }
  const zip = `cbmp-mod-p${MOD.protocol}-v${MOD.version}.zip`;
  for (const f of [zip, `${zip}.sig`, `${zip}.sha256`, 'notes.txt']) {
    if (!existsSync(join(rel, `mod-p${MOD.protocol}-v${MOD.version}`, f))) fail(`falta o mod de teste (${f}): rode "node tools/dev-env.mjs --build-releases"`);
  }
}

/** Confere no kit montado: assinaturas com keys/update.pub, versões e nomes. Devolve um resumo. */
function verifyKitReleases(kit) {
  const pub = readFileSync(join(kit, 'keys', 'update.pub'), 'utf8');
  const repoPub = readFileSync(join(REPO, 'launcher', 'src-tauri', 'keys', 'update.pub'), 'utf8');
  if (pub !== repoPub) fail('keys/update.pub do kit difere da do launcher');
  const out = { keyId: parsePublicKey(pub).keyId, launcher: {}, mod: {} };
  for (const v of LAUNCHER_VERSIONS) {
    const d = join(kit, 'releases', `launcher-v${v}`);
    const exe = `${PRODUCT}_${v}_x64-setup.exe`;
    const comment = verifyMinisign(readFileSync(join(d, exe)), readFileSync(join(d, `${exe}.sig`), 'utf8'), pub);
    const f = tauriCommentFields(comment);
    if (f.version !== v) fail(`${exe}: versão assinada ${f.version} ≠ ${v}`);
    if (f.file !== exe) fail(`${exe}: arquivo assinado ${f.file}`);
    out.launcher[v] = { file: exe, size: statSync(join(d, exe)).size, sha256: sha256File(join(d, exe)), signed: comment };
  }
  const md = join(kit, 'releases', `mod-p${MOD.protocol}-v${MOD.version}`);
  const zip = `cbmp-mod-p${MOD.protocol}-v${MOD.version}.zip`;
  const data = readFileSync(join(md, zip));
  const sha = createHash('sha256').update(data).digest('hex');
  const signed = parseModComment(verifyMinisign(data, readFileSync(join(md, `${zip}.sig`), 'utf8'), pub));
  if (!signed || signed.protocol !== MOD.protocol || signed.version !== MOD.version || signed.sha256 !== sha) fail(`${zip}: comentário assinado não bate (${JSON.stringify(signed)})`);
  const shaFile = readFileSync(join(md, `${zip}.sha256`), 'utf8').trim().split(/\s+/)[0].toLowerCase();
  if (shaFile !== sha) fail(`${zip}.sha256 não bate com o zip`);
  out.mod = { file: zip, size: data.length, sha256: sha };
  return out;
}

// ------------------------------------------------------------------------------------------------------------- main

const o = parseArgs(process.argv.slice(2));
if (process.platform !== 'win32') fail('o kit é para Windows x64; monte-o no Windows');
if (Number(process.versions.node.split('.')[0]) !== NODE_MAJOR) fail(`use o Node ${NODE_MAJOR} (este é ${process.version}): o node_modules do kit tem módulos nativos para a ABI do Node ${NODE_MAJOR}`);
if (existsSync(join(REPO, 'kit.json'))) fail('kit.json na raiz do repositório: isso ligaria o modo kit do dev-env aqui. Apague-o.');

const releasesDir = resolve(o.releases ?? join(process.env.CBMP_DEV_DIR ?? join(tmpdir(), 'cbmp-dev-env'), 'releases'));
const cbServer = resolve(o.cbServer ?? join(REPO, 'build-native', 'windows-release', 'server', 'Release', 'cb-server.exe'));
const outZip = resolve(o.out ?? join(REPO, 'dist', `${KIT_NAME}.zip`));
const guide = join(REPO, 'docs', 'TESTES-EM-CASA-SUB4.md');
if (!existsSync(guide)) fail(`falta o roteiro ${guide}`);

// 1. Entradas atuais?
log(`releases: ${releasesDir}`);
checkReleases(releasesDir);
if (!existsSync(cbServer)) fail(`cb-server não encontrado em ${cbServer}`);
const lastServerCommit = Number(git(['log', '-1', '--format=%ct', '--', ...SERVER_INPUTS]));
const exeTime = statSync(cbServer).mtimeMs / 1000;
const dirty = git(['status', '--porcelain', '--', ...SERVER_INPUTS]);
if (exeTime < lastServerCommit || dirty) {
  const why = dirty ? `há mudanças não commitadas em ${SERVER_INPUTS.join(', ')}` : `cb-server.exe (${new Date(exeTime * 1000).toISOString()}) é mais velho que o último commit do servidor (${new Date(lastServerCommit * 1000).toISOString()})`;
  if (!o.allowStale) fail(`${why}. Recompile (cmake --build --preset windows-release --target cb-server) ou use --allow-stale-server.`);
  log(`AVISO: ${why} (aceito por --allow-stale-server)`);
}
const commit = git(['rev-parse', 'HEAD']);
const repoDirty = git(['status', '--porcelain', '--untracked-files=no']) !== '';

// 2. Montagem numa pasta temporária fora do OneDrive.
const stagingRoot = mkdtempSync(join(tmpdir(), 'cbmp-kit-'));
const kit = join(stagingRoot, KIT_NAME);
mkdirSync(kit);
log(`montando em ${kit}`);
const copy = (from, to = from, opts = {}) => cpSync(join(REPO, from), join(kit, to), { recursive: true, ...opts });
const noTests = (src) => !/[\\/](test|tests|__tests__)$/.test(src) && !/\.test\.ts$/.test(src);
copy('hub/src', 'hub/src', { filter: noTests });
copy('hub/package.json');
copy('hub/tsconfig.json');
copy('packages/contracts/src', 'packages/contracts/src', { filter: noTests });
copy('packages/contracts/package.json');
copy('packages/contracts/tsconfig.json');
copy('tsconfig.base.json');
copy('tools/dev-env.mjs');
copy('tools/kit-smoke.mjs');
copy('tools/lib/minisign.mjs');
copy('tools/lib/source-stamp.mjs');
copy('resources.example/hello-client');
copy('launcher/src-tauri/keys/update.pub', 'keys/update.pub');
copy('THIRD-PARTY-NOTICES.md');
copy('docs/TESTES-EM-CASA-SUB4.md', 'LEIA-ME.md');
copy('package-lock.json');

// package.json do kit: só os workspaces do Hub (o do launcher fica de fora; o npm poda o lock para eles) e, como
// dependência de produção, o tsx que roda o Hub (no repositório ele é devDependency do hub). Instalado com
// --omit=dev: sem vitest/typescript/vite no kit. Versão do tsx = a do package-lock do repositório.
const rootPkg = JSON.parse(readFileSync(join(REPO, 'package.json'), 'utf8'));
const tsxVersion = JSON.parse(readFileSync(join(REPO, 'package-lock.json'), 'utf8')).packages?.['node_modules/tsx']?.version;
if (!tsxVersion) fail('tsx não está no package-lock.json');
writeFileSync(join(kit, 'package.json'), `${JSON.stringify({
  name: rootPkg.name,
  private: true,
  type: 'module',
  description: 'Kit de teste em casa do CB Multiplayer (subprojeto 4). Veja LEIA-ME.md.',
  workspaces: ['packages/contracts', 'hub'],
  dependencies: { tsx: tsxVersion },
  engines: { node: `>=${NODE_MAJOR} <${NODE_MAJOR + 1}` },
  scripts: { start: 'node tools/dev-env.mjs --run', stop: 'node tools/dev-env.mjs --stop', check: 'node tools/kit-smoke.mjs' },
}, null, 2)}\n`);

// Atalhos (CRLF: o cmd.exe exige).
const cmd = (name, lines) => writeFileSync(join(kit, name), `${lines.join('\r\n')}\r\n`);
cmd('INICIAR.cmd', ['@echo off', 'cd /d "%~dp0"', 'title CB Multiplayer - ambiente de teste (feche com Ctrl+C)',
  'node tools\\dev-env.mjs --run', 'echo.', 'echo O ambiente foi encerrado. Pode fechar esta janela.', 'pause']);
cmd('PARAR.cmd', ['@echo off', 'cd /d "%~dp0"', 'node tools\\dev-env.mjs --stop', 'pause']);
cmd('CONFERIR.cmd', ['@echo off', 'cd /d "%~dp0"', 'node tools\\kit-smoke.mjs', 'pause']);

// cb-server + DLLs (sem .lib/.pdb/testes) + runtime do VC++ ao lado (instalação "app-local", permitida pela licença
// de redistribuição do Visual Studio). Assim o PC de casa não precisa do Visual C++ Redistributable.
const binSrc = dirname(cbServer);
const bin = join(kit, 'bin', 'cb-server');
mkdirSync(bin, { recursive: true });
cpSync(cbServer, join(bin, 'cb-server.exe'));
for (const f of readdirSync(binSrc)) if (/\.dll$/i.test(f)) cpSync(join(binSrc, f), join(bin, f));
if (existsSync(join(binSrc, 'plugins'))) cpSync(join(binSrc, 'plugins'), join(bin, 'plugins'), { recursive: true, filter: (s) => statSync(s).isDirectory() || /\.dll$/i.test(s) });
const vc = vcRuntimeDir();
for (const f of VC_RUNTIME) cpSync(join(vc.dir, f), join(bin, f));
log(`runtime do VC++ ${vc.v} (${basename(vc.dir)}) copiado ao lado do cb-server`);

// Releases: só os assets e notes.txt (sem .source-stamp nem outras sobras).
for (const v of LAUNCHER_VERSIONS) {
  const exe = `${PRODUCT}_${v}_x64-setup.exe`;
  for (const f of [exe, `${exe}.sig`, 'notes.txt']) cpSync(join(releasesDir, `launcher-v${v}`, f), join(kit, 'releases', `launcher-v${v}`, f));
}
{
  const tag = `mod-p${MOD.protocol}-v${MOD.version}`;
  const zip = `cbmp-mod-p${MOD.protocol}-v${MOD.version}.zip`;
  for (const f of [zip, `${zip}.sig`, `${zip}.sha256`, 'notes.txt']) cpSync(join(releasesDir, tag, f), join(kit, 'releases', tag, f));
}

// 3. Dependências do Hub: as versões do package-lock (o npm poda as do launcher). Pastas dos workspaces viram cópias
// de verdade em node_modules (zip não guarda junções); o @cbmp/hub não é dependência de ninguém e sai.
log('instalando as dependências do Hub (npm install, versões do package-lock)…');
// Linha fixa, sem nada de fora (o npm.cmd do Windows só roda via shell).
run('npm install --omit=dev --no-audit --no-fund --loglevel=error', [], { cwd: kit, shell: true });
const cbmpMods = join(kit, 'node_modules', '@cbmp');
for (const n of existsSync(cbmpMods) ? readdirSync(cbmpMods) : []) rmSync(join(cbmpMods, n), { recursive: false, force: true });
cpSync(join(kit, 'packages', 'contracts'), join(cbmpMods, 'contracts'), { recursive: true });
for (const junk of ['.package-lock.json']) rmSync(join(kit, 'node_modules', junk), { force: true });
// O npm ci de reserva (LEIA-ME) usa este lock podado. Os binários nativos vêm prontos no pacote (nada é compilado):
// better-sqlite3 traz prebuilds N-API (servem para qualquer Node >= 22) e o esbuild do tsx é um executável Go.
for (const f of [['better-sqlite3', 'prebuilds', 'win32-x64.node'], ['@esbuild', 'win32-x64', 'esbuild.exe'], ['tsx', 'dist', 'cli.mjs']]) {
  if (!existsSync(join(kit, 'node_modules', ...f))) fail(`node_modules do kit sem ${f.join('/')}`);
}
for (const dev of ['vitest', 'typescript', 'vite']) if (existsSync(join(kit, 'node_modules', dev))) fail(`devDependency ${dev} entrou no kit`);

// 4. Conferências do conteúdo.
const sigs = verifyKitReleases(kit);
log(`assinaturas conferidas com a chave pública ${sigs.keyId}: launcher ${LAUNCHER_VERSIONS.join(', ')} e mod ${MOD.version}`);
const files = walk(kit).sort();
const problems = scanForSecrets(kit, files);
if (problems.length) fail(`o kit tem arquivos proibidos ou com cara de segredo:\n  ${problems.join('\n  ')}`);
log(`${files.length} arquivos sem chave privada, segredo, .env ou dados do Hub`);
const longest = files.reduce((a, f) => (f.length > a.length ? f : a), '');
// Explorer/Expand-Archive falham acima de 260 caracteres: C:\cbmp-kit\ (12) + pasta do kit + caminho.
if (`C:\\cbmp-kit\\${KIT_NAME}\\${longest}`.length > 240) fail(`caminho longo demais no kit: ${longest}`);

// 5. kit.json: sha256 de cada arquivo (o dev-env confere antes de subir).
const hashes = Object.fromEntries(files.map((f) => [f, sha256File(join(kit, ...f.split('/')))]));
const kitJson = {
  kind: KIT_NAME,
  format: 1,
  commit,
  repoDirty,
  builtAt: new Date().toISOString(),
  node: process.version,
  platform: 'win32-x64',
  publicKeyId: sigs.keyId,
  launcher: sigs.launcher,
  mod: sigs.mod,
  vcRuntime: vc.v,
  files: hashes,
};
writeFileSync(join(kit, 'kit.json'), `${JSON.stringify(kitJson, null, 2)}\n`);

// 6. Zip com o tar.exe do Windows (bsdtar; o "tar" do Git Bash não faz zip).
const tar = join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'tar.exe');
if (!existsSync(tar)) fail(`${tar} não encontrado`);
mkdirSync(dirname(outZip), { recursive: true });
rmSync(outZip, { force: true });
log(`compactando em ${outZip}…`);
run(tar, ['-a', '-c', '-f', outZip, '-C', stagingRoot, KIT_NAME]);
const listing = spawnSync(tar, ['-t', '-f', outZip], { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 }).stdout.split(/\r?\n/).filter((l) => l && !l.endsWith('/'));
if (listing.length !== files.length + 1) fail(`o zip tem ${listing.length} arquivos, esperados ${files.length + 1}`);
const zipSize = statSync(outZip).size;
const totalSize = files.reduce((n, f) => n + statSync(join(kit, ...f.split('/'))).size, 0);
const zipSha = sha256File(outZip);
writeFileSync(`${outZip}.sha256`, `${zipSha}  ${basename(outZip)}\n`);
if (!o.keep) rmSync(stagingRoot, { recursive: true, force: true });
else log(`pasta de montagem mantida: ${kit}`);

const mib = (n) => `${(n / 1048576).toFixed(1)} MiB`;
log(`pronto: ${outZip}`);
log(`  ${mib(zipSize)} compactado, ${mib(totalSize)} extraído, ${files.length + 1} arquivos; sha256 ${zipSha}`);
log(`  commit ${commit.slice(0, 7)}${repoDirty ? ' (com mudanças não commitadas)' : ''}, Node ${process.version}, chave pública ${sigs.keyId}`);
