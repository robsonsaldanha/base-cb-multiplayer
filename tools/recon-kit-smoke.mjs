#!/usr/bin/env node
// Teste sem o jogo do kit de reconhecimento (dist\cbmp-kit-reconhecimento.zip): extrai o zip numa pasta temporária,
// monta árvores FALSAS do Hogwarts Legacy (Steam e Epic falsos) e roda o ferramentas\cbmp-recon.ps1 de verdade, no
// Windows PowerShell 5.1 (o do PC de casa), sem perguntas nem pausas.
//
//   node tools/recon-kit-smoke.mjs [--zip <zip>] [--keep]
//
// Confere: instalação byte a byte igual ao kit.json; originais (dwmapi.dll/ue4ss de "outro mod") guardados e
// devolvidos idênticos; COLETAR junta logs/relatório/dumps falsos num zip fora da pasta do jogo; DESINSTALAR roda o
// COLETAR sozinho se faltou e deixa a pasta Win64 exatamente como antes (mesmos caminhos e hashes, nada a mais);
// detecção pela Steam (libraryfolders.vdf + appmanifest) e pela Epic (.item); recusas (spike instalado, outro UE4SS).

import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, utimesSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const zip = resolve(args.includes('--zip') ? args[args.indexOf('--zip') + 1] : join(REPO, 'dist', 'cbmp-kit-reconhecimento.zip'));
const keep = args.includes('--keep');
const tar = join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'tar.exe');
const PS = join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');

let failures = 0;
const check = (cond, msg) => { console.log(`${cond ? 'ok   ' : 'FALHA'} ${msg}`); if (!cond) failures++; };
const sha = (p) => createHash('sha256').update(readFileSync(p)).digest('hex');
function snapshot(dir) {
  const out = {};
  const walk = (d) => {
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const f = join(d, e.name);
      if (e.isDirectory()) { out[`${relative(dir, f).split(sep).join('/')}/`] = 'dir'; walk(f); } else out[relative(dir, f).split(sep).join('/')] = sha(f);
    }
  };
  walk(dir);
  return out;
}
const sameSnap = (a, b) => JSON.stringify(Object.entries(a).sort()) === JSON.stringify(Object.entries(b).sort());
const diffSnap = (a, b) => [...new Set([...Object.keys(a), ...Object.keys(b)])].filter((k) => a[k] !== b[k]);
const w = (p, data) => { mkdirSync(dirname(p), { recursive: true }); writeFileSync(p, data); };

if (!existsSync(zip)) { console.error(`zip não encontrado: ${zip} (rode node tools/make-recon-kit.mjs)`); process.exit(1); }
const root = mkdtempSync(join(tmpdir(), 'cbmp-recon-smoke-'));
console.log(`pasta de teste: ${root}`);
const localAppData = join(root, 'LocalAppData');
mkdirSync(localAppData);
let r = spawnSync(tar, ['-x', '-f', zip, '-C', root]);
if (r.status !== 0) { console.error('falha ao extrair'); process.exit(1); }
const kit = join(root, 'cbmp-kit-reconhecimento');
const kitJson = JSON.parse(readFileSync(join(kit, 'kit.json'), 'utf8'));
const script = join(kit, 'ferramentas', 'cbmp-recon.ps1');

// OneDrive/USERPROFILE falsos: nenhum cenário pode gravar no OneDrive real de quem roda o teste.
let fakeOneDrive = join(root, 'SemOneDrive');
function ps(acao, extra = []) {
  const res = spawnSync(PS, ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script, '-Acao', acao,
    '-SemPerguntas', '-SemPausa', '-SemAdmin', ...extra], {
    encoding: 'utf8',
    env: { ...process.env, LOCALAPPDATA: localAppData, OneDrive: fakeOneDrive, OneDriveConsumer: fakeOneDrive, USERPROFILE: join(root, 'Perfil') },
  });
  return { code: res.status, out: `${res.stdout}\n${res.stderr}` };
}

/** Jogo falso: raiz com HogwartsLegacy.exe, Phoenix\Binaries\Win64\HogwartsLegacy.exe e um .pak. */
function fakeGame(dir, { withOriginals = false } = {}) {
  w(join(dir, 'HogwartsLegacy.exe'), 'MZ raiz falsa');
  const win64 = join(dir, 'Phoenix', 'Binaries', 'Win64');
  w(join(win64, 'HogwartsLegacy.exe'), Buffer.concat([Buffer.from('MZ'), Buffer.alloc(4096, 7)]));
  w(join(win64, 'outro.dll'), 'dll do jogo');
  w(join(dir, 'Phoenix', 'Content', 'Paks', 'pakchunk0-WindowsNoEditor.pak'), Buffer.alloc(2048, 3));
  if (withOriginals) {
    w(join(win64, 'dwmapi.dll'), 'dwmapi de outro mod');
    w(join(win64, 'ue4ss', 'Mods', 'OutroMod', 'Scripts', 'main.lua'), 'print("outro")\n');
    w(join(win64, 'ue4ss', 'UE4SS.log'), 'log antigo do outro mod\n');
  }
  return win64;
}

function simulateGameRun(win64) {
  const ue = join(win64, 'ue4ss');
  w(join(ue, 'UE4SS.log'), 'log do UE4SS\n');
  w(join(ue, 'Mods', 'cb-mp', 'cb-mp.log'), 'pos 1 2 3\n');
  w(join(ue, 'Mods', 'cb-recon', 'cb-recon.log'), 'TUDO PRONTO\n');
  w(join(ue, 'Mods', 'cb-recon', 'estado.txt'), 'x objetos OK\n');
  w(join(ue, 'Mods', 'cb-recon', 'relatorio-20261111-120000-1.json'), '{"pawn":{}}\n');
  w(join(ue, 'UE4SS_ObjectDump.txt'), 'objetos\n'.repeat(1000));
  w(join(ue, 'UHTHeaderDump', 'Phoenix', 'Public', 'Biped.h'), 'class X {};\n');
  w(join(ue, 'CXXHeaderDump', 'Phoenix.hpp'), 'struct Y;\n');
  w(join(ue, 'Mods', 'shared', 'types', 'Phoenix.lua'), '---@class Z\n');
  w(join(ue, 'Phoenix-4.27-abc.usmap'), Buffer.alloc(100, 1));
  w(join(ue, '1731340000-ue4ss_actor_data.csv'), 'a,b\n');
  w(join(ue, 'crash_2026_11_11_12_00_00.dmp'), Buffer.alloc(50, 9));
  w(join(localAppData, 'Hogwarts Legacy', 'Saved', 'Logs', 'Phoenix.log'), 'log do jogo\n');
  // tudo "escrito" 5 minutos depois da instalação
  const later = new Date(Date.now() + 5 * 60 * 1000);
  const touch = (d) => { for (const e of readdirSync(d, { withFileTypes: true })) { const f = join(d, e.name); if (e.isDirectory()) touch(f); else utimesSync(f, later, later); } };
  touch(ue);
}

function zipListing(dir) {
  const zips = existsSync(dir) ? readdirSync(dir).filter((f) => /^cbmp-recon-\d{8}-\d{6}\.zip$/.test(f)) : [];
  if (zips.length !== 1) return { zips, list: [] };
  const list = spawnSync(tar, ['-t', '-f', join(dir, zips[0])], { encoding: 'utf8' }).stdout.split(/\r?\n/).filter(Boolean);
  return { zips, list };
}

// --- Cenário A: jogo manual com originais de outro mod; instalar, jogar, coletar, desinstalar --------------------
{
  const game = join(root, 'Jogos', 'Hogwarts Legacy A');
  const win64 = fakeGame(game, { withOriginals: true });
  const before = snapshot(win64);
  const inst = ps('Instalar', ['-PastaJogo', win64]);
  check(inst.code === 0, `A: instalar com -PastaJogo apontando para a Win64 (código ${inst.code})`);
  let allMatch = true;
  for (const f of kitJson.payload) {
    const p = join(win64, ...f.caminho.split('\\'));
    if (!existsSync(p) || sha(p) !== f.sha256) { allMatch = false; console.log(`   diferente: ${f.caminho}`); }
  }
  check(allMatch, `A: ${kitJson.payload.length} arquivos instalados idênticos ao kit.json`);
  check(!existsSync(join(win64, 'ue4ss', 'Mods', 'OutroMod')), 'A: ue4ss do outro mod saiu do caminho');
  check(existsSync(join(win64, 'cbmp-recon-backup', 'dwmapi.dll')) && existsSync(join(win64, 'cbmp-recon-backup', 'ue4ss', 'Mods', 'OutroMod', 'Scripts', 'main.lua')), 'A: originais guardados em cbmp-recon-backup');
  const again = ps('Instalar', ['-PastaJogo', win64]);
  check(again.code !== 0 && /já está instalado/.test(again.out), 'A: segunda instalação recusada');

  simulateGameRun(win64);
  const dest = join(root, 'coleta-A');
  const col = ps('Coletar', ['-Destino', dest]);
  check(col.code === 0, `A: COLETAR (código ${col.code})`);
  const { zips, list } = zipListing(dest);
  check(zips.length === 1, `A: um zip cbmp-recon-*.zip em ${dest}`);
  const need = ['ue4ss/UE4SS.log', 'ue4ss/Mods/cb-mp/cb-mp.log', 'ue4ss/Mods/cb-recon/relatorio-20261111-120000-1.json',
    'ue4ss/UE4SS_ObjectDump.txt', 'ue4ss/UHTHeaderDump/Phoenix/Public/Biped.h', 'ue4ss/CXXHeaderDump/Phoenix.hpp',
    'ue4ss/Mods/shared/types/Phoenix.lua', 'ue4ss/Phoenix-4.27-abc.usmap', 'ue4ss/1731340000-ue4ss_actor_data.csv',
    'ue4ss/crash_2026_11_11_12_00_00.dmp', 'jogo-logs/Phoenix.log', 'info-jogo.json', 'PRIVADO-LEIA.txt'];
  const norm = list.map((l) => l.replace(/\\/g, '/').replace(/^cbmp-recon-\d{8}-\d{6}\//, ''));
  const missing = need.filter((n) => !norm.includes(n));
  check(missing.length === 0, `A: zip da coleta tem logs, relatório, dumps, minidump e info do jogo${missing.length ? ` (faltando: ${missing.join(', ')})` : ''}`);
  check(!norm.some((n) => /main\.dll|UE4SS\.dll|OutroMod/.test(n)), 'A: coleta não leva DLLs do kit nem arquivos do outro mod');

  const un = ps('Desinstalar', ['-PastaJogo', win64]);
  check(un.code === 0, `A: DESINSTALAR (código ${un.code})`);
  const after = snapshot(win64);
  check(sameSnap(before, after), `A: Win64 idêntica a antes da instalação (caminhos + sha256)${sameSnap(before, after) ? '' : ` diferenças: ${diffSnap(before, after).join(', ')}`}`);
  check(!existsSync(join(kit, 'instalado-em.txt')), 'A: marca de instalação removida do kit');
  const un2 = ps('Desinstalar', ['-PastaJogo', win64]);
  check(un2.code === 0 && /Nada para desinstalar/.test(un2.out), 'A: desinstalar de novo não faz nada');
}

// --- Cenário B: Steam falsa (detecção automática), pasta limpa; desinstalar sem coletar -> coleta automática ------
{
  const steam = join(root, 'Steam');
  const lib = join(root, 'Biblioteca Steam');
  w(join(steam, 'steamapps', 'libraryfolders.vdf'), `"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"${steam.replace(/\\/g, '\\\\')}"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"${lib.replace(/\\/g, '\\\\')}"\n\t}\n}\n`);
  w(join(lib, 'steamapps', 'appmanifest_990080.acf'), `"AppState"\n{\n\t"appid"\t\t"990080"\n\t"installdir"\t\t"Hogwarts Legacy"\n\t"buildid"\t\t"12345678"\n}\n`);
  const win64 = fakeGame(join(lib, 'steamapps', 'common', 'Hogwarts Legacy'));
  const before = snapshot(win64);
  const inst = ps('Instalar', ['-SteamRaiz', steam, '-EpicManifestos', join(root, 'nada')]);
  check(inst.code === 0 && /Jogo encontrado \(Steam\)/.test(inst.out), `B: Steam detectada pelo libraryfolders.vdf + appmanifest (código ${inst.code})`);
  simulateGameRun(win64);
  const destB = join(root, 'coleta-B');
  // DESINSTALAR sem COLETAR antes: coleta sozinho (no destino padrão: a Área de Trabalho, já que não há OneDrive
  // falso; para não sujar a Área de Trabalho real, o teste passa -Destino, que também vale para a coleta automática)
  const un = ps('Desinstalar', ['-SteamRaiz', steam, '-EpicManifestos', join(root, 'nada'), '-Destino', destB]);
  check(un.code === 0 && /ainda não foram coletados/.test(un.out), `B: desinstalar sem coletar roda o COLETAR antes (código ${un.code})`);
  const { zips, list } = zipListing(destB);
  check(zips.length === 1 && list.some((l) => /info-jogo\.json$/.test(l)), 'B: zip da coleta automática criado');
  if (zips.length === 1) {
    const info = spawnSync(tar, ['-x', '-O', '-f', join(destB, zips[0]), `${zips[0].replace(/\.zip$/, '')}/info-jogo.json`], { encoding: 'utf8' }).stdout;
    check(/"steam_buildid":\s*"12345678"/.test(info) && /"loja":\s*"Steam"/.test(info), 'B: info-jogo.json traz loja Steam e buildid');
  }
  check(sameSnap(before, snapshot(win64)), 'B: Win64 idêntica a antes');
}

// --- Cenário C: Epic falsa (.item) --------------------------------------------------------------------------------
{
  const game = join(root, 'Epic Games', 'HogwartsLegacy');
  const win64 = fakeGame(game);
  const man = join(root, 'EpicManifests');
  w(join(man, 'AAAA.item'), JSON.stringify({ DisplayName: 'Outro Jogo', InstallLocation: join(root, 'x') }));
  w(join(man, 'BBBB.item'), JSON.stringify({ DisplayName: 'Hogwarts Legacy', AppName: 'fa4240e57a3c46b39f169041b7811293', InstallLocation: game, AppVersionString: '1.0.0.123' }));
  const before = snapshot(win64);
  const inst = ps('Instalar', ['-SteamRaiz', join(root, 'nada'), '-EpicManifestos', man]);
  check(inst.code === 0 && /Jogo encontrado \(Epic\)/.test(inst.out), `C: Epic detectada pelo .item (código ${inst.code})`);
  const un = ps('Desinstalar', ['-SteamRaiz', join(root, 'nada'), '-EpicManifestos', man]);
  check(un.code === 0, 'C: desinstalar (sem jogo aberto, sem coleta pendente)');
  check(sameSnap(before, snapshot(win64)), 'C: Win64 idêntica a antes');
}

// --- Cenário D: recusas -------------------------------------------------------------------------------------------
{
  const g1 = join(root, 'Jogos', 'D-spike');
  const w1 = fakeGame(g1);
  w(join(w1, 'cbmp-backup', 'x.txt'), 'spike');
  w(join(w1, 'dwmapi.dll'), 'spike dll');
  const b1 = snapshot(w1);
  const r1 = ps('Instalar', ['-PastaJogo', g1]);
  check(r1.code !== 0 && /spike/.test(r1.out) && sameSnap(b1, snapshot(w1)), 'D: spike instalado -> recusa sem mexer em nada');
  const g2 = join(root, 'Jogos', 'D-ue4ss-antigo');
  const w2 = fakeGame(g2);
  w(join(w2, 'xinput1_3.dll'), 'ue4ss antigo');
  const b2 = snapshot(w2);
  const r2 = ps('Instalar', ['-PastaJogo', g2]);
  check(r2.code !== 0 && /xinput1_3/.test(r2.out) && sameSnap(b2, snapshot(w2)), 'D: outro UE4SS -> recusa sem mexer em nada');
  const r3 = ps('Instalar', ['-PastaJogo', join(root, 'nao-existe')]);
  check(r3.code !== 0 && /não é a pasta do Hogwarts Legacy/.test(r3.out), 'D: pasta errada -> recusa');
  // kit corrompido
  const lua = join(kit, 'payload', 'ue4ss', 'Mods', 'cb-recon', 'Scripts', 'main.lua');
  const orig = readFileSync(lua);
  writeFileSync(lua, Buffer.concat([orig, Buffer.from('\n-- mexido\n')]));
  const g4 = join(root, 'Jogos', 'D-kit-ruim');
  const w4 = fakeGame(g4);
  const b4 = snapshot(w4);
  const r4 = ps('Instalar', ['-PastaJogo', g4]);
  writeFileSync(lua, orig);
  check(r4.code !== 0 && /incompleto ou corrompido/.test(r4.out) && sameSnap(b4, snapshot(w4)), 'D: kit alterado -> recusa sem mexer em nada');
}

// --- Cenário E: restauração pela metade (dwmapi.dll já devolvido, ue4ss não) e nova execução ------------------------
const mv = (a, b) => { const r2 = spawnSync('cmd.exe', ['/c', 'move', a, b], { encoding: 'utf8' }); if (r2.status !== 0) throw new Error(r2.stdout + r2.stderr); };
{
  const game = join(root, 'Jogos', 'E-parcial');
  const win64 = fakeGame(game, { withOriginals: true });
  const before = snapshot(win64);
  check(ps('Instalar', ['-PastaJogo', game]).code === 0, 'E: instalar');
  const bk = join(win64, 'cbmp-recon-backup');
  // o que um DESINSTALAR interrompido deixa: dwmapi do kit apagado e o original devolvido; ue4ss do kit e o backup ainda lá
  rmSync(join(win64, 'dwmapi.dll'));
  mv(join(bk, 'dwmapi.dll'), join(win64, 'dwmapi.dll'));
  const un = ps('Desinstalar', ['-PastaJogo', game]);
  check(un.code === 0 && /já estava devolvido: dwmapi\.dll/.test(un.out) && /voltou a ser como antes/.test(un.out), `E: reexecução reconhece o dwmapi.dll já devolvido (código ${un.code})`);
  check(!existsSync(join(win64, 'cbmp-recon-sobras')), 'E: o original do usuário NÃO vai para cbmp-recon-sobras');
  check(sameSnap(before, snapshot(win64)), `E: Win64 idêntica a antes${sameSnap(before, snapshot(win64)) ? '' : `: ${diffSnap(before, snapshot(win64)).join(', ')}`}`);
  const again = ps('Desinstalar', ['-PastaJogo', game]);
  check(again.code === 0 && /Nada para desinstalar/.test(again.out), 'E: terceira execução não faz nada');
}

// --- Cenário F: tudo devolvido mas estado.json ficou (interrompido no fim) ------------------------------------------
{
  const game = join(root, 'Jogos', 'F-quase');
  const win64 = fakeGame(game, { withOriginals: true });
  const before = snapshot(win64);
  check(ps('Instalar', ['-PastaJogo', game]).code === 0, 'F: instalar');
  const bk = join(win64, 'cbmp-recon-backup');
  rmSync(join(win64, 'dwmapi.dll'));
  rmSync(join(win64, 'ue4ss'), { recursive: true });
  mv(join(bk, 'dwmapi.dll'), join(win64, 'dwmapi.dll'));
  mv(join(bk, 'ue4ss'), join(win64, 'ue4ss'));
  const un = ps('Desinstalar', ['-PastaJogo', game]);
  check(un.code === 0 && (un.out.match(/já estava devolvido/g) || []).length === 2, `F: os dois originais reconhecidos como devolvidos (código ${un.code})`);
  check(sameSnap(before, snapshot(win64)), 'F: Win64 idêntica a antes');
}

// --- Cenário G: original sumiu do backup e o que está no lugar não é ele -> para sem apagar nada --------------------
{
  const game = join(root, 'Jogos', 'G-estranho');
  const win64 = fakeGame(game, { withOriginals: true });
  check(ps('Instalar', ['-PastaJogo', game]).code === 0, 'G: instalar');
  rmSync(join(win64, 'cbmp-recon-backup', 'dwmapi.dll'));
  const mid = snapshot(win64);
  const un = ps('Desinstalar', ['-PastaJogo', game]);
  check(un.code !== 0 && /não está em/.test(un.out) && sameSnap(mid, snapshot(win64)), 'G: recusa e não mexe em nada');
}

// --- Cenário H: dwmapi.dll trocado por outro programa; destino padrão no OneDrive falso ------------------------------
{
  const game = join(root, 'Jogos', 'H-trocado');
  const win64 = fakeGame(game);
  check(ps('Instalar', ['-PastaJogo', game]).code === 0, 'H: instalar');
  simulateGameRun(win64);
  fakeOneDrive = join(root, 'OneDriveFalso');
  mkdirSync(join(fakeOneDrive, 'CasteloBruxoRP'), { recursive: true });
  const col = ps('Coletar', ['-PastaJogo', game]);
  const destH = join(fakeOneDrive, 'CasteloBruxoRP', 'cbmp-dumps');
  check(col.code === 0 && zipListing(destH).zips.length === 1, 'H: COLETAR sem -Destino grava em <OneDrive>\\CasteloBruxoRP\\cbmp-dumps');
  const repoDest = join(REPO, 'dist', 'nao-pode');
  const bad = ps('Coletar', ['-PastaJogo', game, '-Destino', repoDest]);
  check(bad.code !== 0 && !existsSync(repoDest), 'H: COLETAR recusa destino dentro do repositório');
  w(join(win64, 'dwmapi.dll'), 'outro programa trocou');
  const un = ps('Desinstalar', ['-PastaJogo', game]);
  check(un.code === 0 && /NÃO está exatamente/.test(un.out) && !/voltou a ser como antes/.test(un.out)
    && existsSync(join(win64, 'cbmp-recon-sobras', 'dwmapi.dll')), 'H: dll estranha vai para sobras e o aviso final não diz "como antes"');
  fakeOneDrive = join(root, 'SemOneDrive');
}

if (!keep) rmSync(root, { recursive: true, force: true });
console.log(failures === 0 ? 'TODOS OK' : `${failures} FALHA(S)`);
process.exit(failures === 0 ? 0 : 1);
