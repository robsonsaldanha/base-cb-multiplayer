#!/usr/bin/env node
// Monta o kit de reconhecimento do jogo (dist\cbmp-kit-reconhecimento.zip): UE4SS (mesma versão vendorizada do spike,
// com licenças), o mod C++ do spike (cb-mp) e o mod Lua cb-recon, mais INSTALAR/DESINSTALAR/COLETAR.bat.
// Roteiro para o PC de casa: docs/RECONHECIMENTO-JOGO.md (vai no kit como LEIA-ME.md).
//
//   node tools/make-recon-kit.mjs [opções]
//
// Pré-requisitos (máquina de desenvolvimento):
//   - client/deps/RE-UE4SS no commit fixado em tools/fetch-ue4ss.ps1;
//   - mod compilado: tools/build.ps1 -Target mod (UE4SS.dll, dwmapi.dll, cb-mp.dll em build\mod), mais novo que
//     client/src, client/core e client/CMakeLists.txt (ou use --rebuild, que abre o Developer PowerShell do VS);
//   - (recomendado) build\lua54\luac.exe e lua.exe (tools/build-lua54.ps1): checa a sintaxe do cb-recon e roda o teste
//     simulado tools/test/cb-recon-mock.lua. Sem eles o kit só sai com --no-lua-check.
//
// Opções: --out <zip>        (padrão dist\cbmp-kit-reconhecimento.zip)
//         --rebuild          recompila o mod antes (VS 2026 + vcpkg em C:\vcpkg)
//         --no-lua-check     não exige luac/lua (não recomendado)
//         --keep-staging     mantém a pasta de montagem
//
// Nada aqui lê chaves; o kit passa pela mesma varredura de segredos do kit do sub4 (tools/lib/kit-check.mjs).

import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, existsSync, lstatSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { scanForSecrets } from './lib/kit-check.mjs';

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const KIT_NAME = 'cbmp-kit-reconhecimento';
const KIT_VERSION = '0.1.0';
const UE4SS_DIR = join(REPO, 'client', 'deps', 'RE-UE4SS');
const BUILD = join(REPO, 'build', 'mod');
const LUA_DIR = join(REPO, 'build', 'lua54');
const SRC = join(REPO, 'recon-kit');

const log = (m) => console.log(`[make-recon-kit] ${m}`);
function fail(m) { console.error(`\n[make-recon-kit] ERRO: ${m}\n`); process.exit(1); }

function parseArgs(argv) {
  const o = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    switch (a) {
      case '--out': o.out = argv[++i] ?? fail('--out precisa de valor'); break;
      case '--rebuild': o.rebuild = true; break;
      case '--no-lua-check': o.noLua = true; break;
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

function sh(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { encoding: 'utf8', maxBuffer: 256 * 1024 * 1024, ...opts });
  if (r.error) fail(`não foi possível rodar ${cmd}: ${r.error.message}`);
  return r;
}
const sha256File = (p) => createHash('sha256').update(readFileSync(p)).digest('hex');

function walk(dir, base = dir, out = [], skipLinks = false) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, e.name);
    if (e.isSymbolicLink() || lstatSync(full).isSymbolicLink()) {
      if (skipLinks) continue;
      fail(`link/junção: ${relative(base, full)}`);
    }
    if (e.isDirectory()) walk(full, base, out, skipLinks);
    else if (e.isFile()) out.push(relative(base, full).split(sep).join('/'));
  }
  return out;
}
const newest = (dir) => walk(dir).reduce((m, f) => Math.max(m, statSync(join(dir, ...f.split('/'))).mtimeMs), 0);

function findOne(name, mustInclude) {
  const hits = walk(BUILD, BUILD, [], true).filter((f) => f.endsWith(`/${name}`) && f.includes('Game__Shipping__Win64') && (!mustInclude || f.includes(mustInclude)));
  if (hits.length !== 1) fail(`esperava 1 '${name}' em ${BUILD}, achei ${hits.length}: ${hits.join('; ')} (rode tools/build.ps1 -Target mod)`);
  return join(BUILD, ...hits[0].split('/'));
}

const o = parseArgs(process.argv.slice(2));
const outZip = resolve(o.out ?? join(REPO, 'dist', `${KIT_NAME}.zip`));

// 1. UE4SS fixado
const pinned = /\$COMMIT\s*=\s*"([0-9a-f]{40})"/.exec(readFileSync(join(REPO, 'tools', 'fetch-ue4ss.ps1'), 'utf8'))?.[1];
if (!pinned) fail('commit do UE4SS não encontrado em tools/fetch-ue4ss.ps1');
const ue4ssHead = sh('git', ['-C', UE4SS_DIR, 'rev-parse', 'HEAD']).stdout.trim();
if (ue4ssHead !== pinned) fail(`RE-UE4SS em ${ue4ssHead || '?'}, esperado ${pinned} (rode tools/fetch-ue4ss.ps1)`);
log(`UE4SS no commit fixado ${pinned.slice(0, 7)}`);

// 2. Mod compilado e atual
if (o.rebuild) {
  log('recompilando o mod (tools/build.ps1 -Target mod)…');
  const ps = `& 'C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\Common7\\Tools\\Launch-VsDevShell.ps1' -Arch amd64 -SkipAutomaticLocation | Out-Null; $env:VCPKG_ROOT='C:\\vcpkg'; & '${join(REPO, 'tools', 'build.ps1')}' -Target mod; exit $LASTEXITCODE`;
  const r = sh('pwsh', ['-NoProfile', '-Command', ps], { stdio: 'inherit' });
  if (r.status !== 0) fail('build do mod falhou');
}
const ue4ssDll = findOne('UE4SS.dll');
const proxyDll = findOne('dwmapi.dll');
const modDll = findOne('cb-mp.dll');
const srcNewest = Math.max(newest(join(REPO, 'client', 'src')), newest(join(REPO, 'client', 'core')), statSync(join(REPO, 'client', 'CMakeLists.txt')).mtimeMs);
if (statSync(modDll).mtimeMs < srcNewest) fail(`cb-mp.dll é mais velho que o código em client/ (use --rebuild)`);
log(`mod: cb-mp.dll ${sha256File(modDll).slice(0, 12)}…, UE4SS.dll ${sha256File(ue4ssDll).slice(0, 12)}…`);

// 3. Lua: sintaxe + teste simulado
const mainLua = join(SRC, 'cb-recon', 'Scripts', 'main.lua');
const luac = join(LUA_DIR, 'luac.exe');
const lua = join(LUA_DIR, 'lua.exe');
if (existsSync(luac) && existsSync(lua)) {
  const ver = sh(lua, ['-v']).stdout.trim();
  const p = sh(luac, ['-p', mainLua]);
  if (p.status !== 0) fail(`luac -p falhou:\n${p.stderr}`);
  const tmp = mkdtempSync(join(tmpdir(), 'cbrecon-mock-'));
  const m = sh(lua, [join(REPO, 'tools', 'test', 'cb-recon-mock.lua'), tmp, mainLua]);
  // Os relatórios do teste simulado têm de ser JSON válido em UTF-8 estrito (inclui texto longo com acentos cortado).
  const repDir = join(tmp, 'Mods', 'cb-recon');
  const reps = existsSync(repDir) ? readdirSync(repDir).filter((f) => /^relatorio-.*\.json$/.test(f)) : [];
  const badJson = [];
  for (const f of reps) {
    try { JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(readFileSync(join(repDir, f)))); } catch (e) { badJson.push(`${f}: ${e.message}`); }
  }
  rmSync(tmp, { recursive: true, force: true });
  const lines = (m.stdout || '').split(/\r?\n/).filter((l) => /^(ok|FALHA|TODOS|\d+ FALHA)/.test(l));
  if (m.status !== 0) fail(`teste simulado do cb-recon falhou:\n${lines.join('\n')}\n${m.stderr}`);
  if (!reps.length || badJson.length) fail(`relatórios do teste simulado: ${reps.length} arquivo(s); inválidos: ${badJson.join('; ') || '-'}`);
  log(`Lua: ${ver}; luac -p ok; teste simulado: ${lines.filter((l) => l.startsWith('ok')).length} verificações ok; ${reps.length} relatório(s) JSON/UTF-8 válidos`);
} else if (!o.noLua) {
  fail(`luac/lua não encontrados em ${LUA_DIR} (rode tools/build-lua54.ps1 no Developer PowerShell, ou use --no-lua-check)`);
} else {
  log('AVISO: sem checagem de Lua (--no-lua-check)');
}

// 4. Montagem
const stagingRoot = mkdtempSync(join(tmpdir(), 'cbmp-recon-kit-'));
const kit = join(stagingRoot, KIT_NAME);
const put = (rel, from) => { const d = join(kit, ...rel.split('/')); mkdirSync(dirname(d), { recursive: true }); cpSync(from, d); };
const write = (rel, text) => { const d = join(kit, ...rel.split('/')); mkdirSync(dirname(d), { recursive: true }); writeFileSync(d, text); };
const crlf = (s) => s.replace(/\r?\n/g, '\r\n');

put('payload/dwmapi.dll', proxyDll);
put('payload/ue4ss/UE4SS.dll', ue4ssDll);
let ini = readFileSync(join(UE4SS_DIR, 'assets', 'UE4SS-settings.ini'), 'utf8');
// Igual ao spike: sem a janela gráfica do UE4SS (fica só o console de texto, onde o cb-recon mostra o progresso).
// Não carregar todos os assets antes dos dumpers (seria lento e arriscado num jogo deste tamanho).
for (const [k, v] of [['GuiConsoleEnabled', '0'], ['GuiConsoleVisible', '0'], ['ConsoleEnabled', '1'],
  ['LoadAllAssetsBeforeDumpingObjects', '0'], ['LoadAllAssetsBeforeGeneratingCXXHeaders', '0'], ['EnableHotReloadSystem', '0']]) {
  const re = new RegExp(`^(\\s*${k}\\s*=\\s*)\\S*`, 'm');
  if (!re.test(ini)) fail(`chave ${k} não encontrada em UE4SS-settings.ini`);
  ini = ini.replace(re, `$1${v}`);
}
write('payload/ue4ss/UE4SS-settings.ini', ini);
write('payload/ue4ss/Mods/mods.txt', crlf(readFileSync(join(SRC, 'mods.txt'), 'utf8')));
put('payload/ue4ss/Mods/cb-mp/dlls/main.dll', modDll);
write('payload/ue4ss/Mods/cb-mp/enabled.txt', '');
put('payload/ue4ss/Mods/cb-recon/Scripts/main.lua', mainLua);
write('payload/ue4ss/Mods/cb-recon/enabled.txt', '');

for (const b of ['INSTALAR.bat', 'DESINSTALAR.bat', 'COLETAR.bat']) write(b, crlf(readFileSync(join(SRC, b), 'utf8')));
let ps1 = readFileSync(join(SRC, 'ferramentas', 'cbmp-recon.ps1'));
if (!(ps1[0] === 0xef && ps1[1] === 0xbb && ps1[2] === 0xbf)) fail('cbmp-recon.ps1 precisa de BOM UTF-8 (o PowerShell 5.1 lê sem BOM como ANSI)');
write('ferramentas/cbmp-recon.ps1', Buffer.from(crlf(ps1.toString('utf8')), 'utf8'));
write('LEIA-ME.md', readFileSync(join(REPO, 'docs', 'RECONHECIMENTO-JOGO.md'), 'utf8'));

// Licenças: UE4SS (MIT) e o que ele embute.
const lic = [];
const addLic = (from, name) => { put(`licencas/${name}`, from); lic.push(name); };
addLic(join(UE4SS_DIR, 'LICENSE'), 'RE-UE4SS-LICENSE.txt');
for (const d of readdirSync(join(UE4SS_DIR, 'deps', 'first'))) {
  for (const f of ['LICENSE', 'LICENSE.md', 'LICENSE.txt', 'LICENSE-MIT', 'LICENSE-APACHE']) {
    const p = join(UE4SS_DIR, 'deps', 'first', d, f);
    if (existsSync(p)) addLic(p, `ue4ss-${d}-${f.replace(/\.(md|txt)$/, '')}.txt`);
  }
}
addLic(join(UE4SS_DIR, 'UE4SS', 'src', 'USMapGenerator', 'LICENSE'), 'ue4ss-USMapGenerator-LICENSE.txt');
addLic(join(UE4SS_DIR, 'UE4SS', 'include', 'fonts', 'LICENSE_Droid.txt'), 'ue4ss-fonts-Droid-LICENSE.txt');
addLic(join(UE4SS_DIR, 'UE4SS', 'include', 'fonts', 'LICENSE_Roboto.txt'), 'ue4ss-fonts-Roboto-LICENSE.txt');
const depsDir = join(BUILD, '_deps');
for (const d of readdirSync(depsDir).filter((x) => x.endsWith('-src'))) {
  for (const f of readdirSync(join(depsDir, d)).filter((x) => /^(LICENSE|COPYING)/i.test(x))) {
    addLic(join(depsDir, d, f), `${d.replace(/-src$/, '')}-${f.replace(/\.(md|txt)$/i, '')}.txt`);
  }
}
const zycore = join(depsDir, 'zydis-src', 'dependencies', 'zycore', 'LICENSE');
if (existsSync(zycore)) addLic(zycore, 'zycore-LICENSE.txt');
write('licencas/AVISOS.txt', crlf(`Avisos de terceiros do kit de reconhecimento do CB Multiplayer

UE4SS (RE-UE4SS, https://github.com/UE4SS-RE/RE-UE4SS), commit ${pinned}, licença MIT
(RE-UE4SS-LICENSE.txt). Arquivos: payload/dwmapi.dll, payload/ue4ss/UE4SS.dll, payload/ue4ss/UE4SS-settings.ini
(copiado de assets/ com GuiConsoleEnabled/GuiConsoleVisible = 0).

O UE4SS.dll embute as bibliotecas cujas licenças estão nesta pasta (dependências "first" do UE4SS, Lua 5.4,
fmt, glaze, GLFW, Dear ImGui, ImGuiColorTextEdit, PolyHook 2, Zydis/Zycore, raw_pdb, concurrentqueue,
corrosion, fontes Droid/Roboto e o gerador de .usmap de OutTheShade). O patternsleuth (Rust) e as crates dele
vêm do código do UE4SS (deps/first/patternsleuth); as licenças das crates estão nos fontes delas.

cb-mp (payload/ue4ss/Mods/cb-mp) e cb-recon (payload/ue4ss/Mods/cb-recon) são do projeto CB Multiplayer.
Nenhum código da MafiaHub/arcane-mp/HogwartsMP nem do Dumper-7 está neste kit.
`));

// 5. kit.json (o cbmp-recon.ps1 confere o payload antes de instalar)
const commit = sh('git', ['-C', REPO, 'rev-parse', 'HEAD']).stdout.trim();
const dirty = sh('git', ['-C', REPO, 'status', '--porcelain', '--', 'recon-kit', 'tools', 'docs/RECONHECIMENTO-JOGO.md', 'client/src', 'client/core']).stdout.trim() !== '';
const payloadFiles = walk(join(kit, 'payload')).sort();
const kitJson = {
  nome: KIT_NAME, versao: KIT_VERSION, commit, sujo: dirty, ue4ssCommit: pinned, montadoEm: new Date().toISOString(),
  payload: payloadFiles.map((f) => {
    const p = join(kit, 'payload', ...f.split('/'));
    return { caminho: f.split('/').join('\\'), sha256: sha256File(p), tamanho: statSync(p).size };
  }),
  licencas: lic,
};
writeFileSync(join(kit, 'kit.json'), `${JSON.stringify(kitJson, null, 2)}\n`);

// 6. Conferências
const files = walk(kit).sort();
const expected = [
  'COLETAR.bat', 'DESINSTALAR.bat', 'INSTALAR.bat', 'LEIA-ME.md', 'ferramentas/cbmp-recon.ps1', 'kit.json',
  'payload/dwmapi.dll', 'payload/ue4ss/Mods/cb-mp/dlls/main.dll', 'payload/ue4ss/Mods/cb-mp/enabled.txt',
  'payload/ue4ss/Mods/cb-recon/Scripts/main.lua', 'payload/ue4ss/Mods/cb-recon/enabled.txt', 'payload/ue4ss/Mods/mods.txt',
  'payload/ue4ss/UE4SS-settings.ini', 'payload/ue4ss/UE4SS.dll',
  ...lic.map((l) => `licencas/${l}`), 'licencas/AVISOS.txt',
].sort();
const extra = files.filter((f) => !expected.includes(f));
const missing = expected.filter((f) => !files.includes(f));
if (extra.length || missing.length) fail(`conteúdo inesperado. sobrando: ${extra.join(', ') || '-'}; faltando: ${missing.join(', ') || '-'}`);
const secrets = scanForSecrets(kit, files);
if (secrets.length) fail(`varredura de segredos:\n  ${secrets.join('\n  ')}`);
for (const b of ['INSTALAR.bat', 'DESINSTALAR.bat', 'COLETAR.bat']) {
  const t = readFileSync(join(kit, b), 'latin1');
  if (/[^\x00-\x7f]/.test(t)) fail(`${b} tem caracteres fora do ASCII (o cmd.exe os mostraria errado)`);
}
if (/(UE4SS_ObjectDump|HeaderDump|\.usmap|relatorio-\d)/.test(files.join('\n'))) fail('saída de dump dentro do kit');

// 7. Zip (tar.exe do Windows = bsdtar)
const tar = join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'tar.exe');
if (!existsSync(tar)) fail(`${tar} não encontrado`);
mkdirSync(dirname(outZip), { recursive: true });
rmSync(outZip, { force: true });
let r = sh(tar, ['-a', '-c', '-f', outZip, '-C', stagingRoot, KIT_NAME]);
if (r.status !== 0) fail(`tar falhou: ${r.stderr}`);
const listing = sh(tar, ['-t', '-f', outZip]).stdout.split(/\r?\n/).filter((l) => l && !l.endsWith('/'));
if (listing.length !== files.length) fail(`o zip tem ${listing.length} arquivos, esperados ${files.length}`);
const zipSha = sha256File(outZip);
writeFileSync(`${outZip}.sha256`, `${zipSha}  ${basename(outZip)}\n`);
const total = files.reduce((n, f) => n + statSync(join(kit, ...f.split('/'))).size, 0);
if (!o.keep) rmSync(stagingRoot, { recursive: true, force: true });
else log(`pasta de montagem mantida: ${kit}`);
const mib = (n) => `${(n / 1048576).toFixed(1)} MiB`;
log(`pronto: ${outZip}`);
log(`  ${mib(statSync(outZip).size)} compactado, ${mib(total)} extraído, ${files.length} arquivos; sha256 ${zipSha}`);
log(`  commit ${commit.slice(0, 7)}${dirty ? ' (com mudanças não commitadas)' : ''}`);
