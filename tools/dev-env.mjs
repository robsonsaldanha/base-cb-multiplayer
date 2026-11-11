#!/usr/bin/env node
// Ambiente de teste local do CB Multiplayer (Windows): Hub local + cb-server com hello-client + releases de teste.
//
//   node tools/dev-env.mjs --build-releases   gera em <pasta>/releases: launcher "Teste local" 0.1.0 e 0.1.1 assinados com
//                                             a chave dev e o mod de teste mod-p1-v0.1.0 (lento: dois builds release)
//   node tools/dev-env.mjs --run              sobe Hub (dev login, servidores privados, HUB_RELEASES_DIR) e cb-server
//                                             (modo Hub, hello-client); servidor "Castelo Bruxo (teste)" em destaque.
//                                             Ctrl+C encerra tudo.
//   node tools/dev-env.mjs --stop             encerra o que um --run anterior deixou rodando (janela fechada etc.)
//
// Opções: --dir <pasta>  (padrão %TEMP%\cbmp-dev-env; ou CBMP_DEV_DIR)
//         --key <arquivo> chave privada dev do Tauri (padrão %USERPROFILE%\.cbmp-devkeys\update.key; ou CBMP_DEV_KEY).
//                         Lida só para assinar; nunca copiada nem impressa. Recusada se estiver dentro do repositório.
//         --cb-server <exe> (padrão build-native\windows-release\server\Release\cb-server.exe; ou CBMP_CB_SERVER)
//         --force        refaz releases que já existem (sem ele, --build-releases pula o que já está pronto)
//         --hub-port <n>, --game-port <n>  portas do Hub (padrão 8787) e do cb-server (padrão 30500). Só para testes
//                        automáticos: o launcher "Teste local" instalado procura o Hub em 127.0.0.1:8787.
//
// Idempotente: rodar de novo reaproveita os dados do Hub, o servidor cadastrado (estado em <pasta>\state.json) e as
// releases prontas. Nada aqui vai para produção: Hub só em 127.0.0.1, login de dev, releases de teste.

import { spawn, spawnSync } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { createWriteStream, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync, copyFileSync, cpSync } from 'node:fs';
import { createServer } from 'node:net';
import { homedir, tmpdir } from 'node:os';
import { dirname, join, relative, resolve, isAbsolute } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..');
// Portas padrão (as que o launcher "Teste local" conhece: Hub fixo em 127.0.0.1:8787). --hub-port/--game-port só para
// o teste de ponta a ponta (launcher/src-tauri/tests/e2e.rs), que usa portas livres para não colidir com um --run aberto.
let HUB_PORT = 8787;
let HUB_URL = `http://127.0.0.1:${HUB_PORT}`;
let GAME_PORT = 30500;
const SERVER_NAME = 'Castelo Bruxo (teste)';
const DEV_DID = '111111111111111111';
const DEV_NAME = 'Dev Local';
const MOD_PROTOCOL = 1;
const MOD_VERSION = '0.1.0';
const LAUNCHER_VERSIONS = ['0.1.0', '0.1.1'];
const PRODUCT = 'CB Multiplayer (Teste local)';

// ----------------------------------------------------------------------------------------------------- argumentos

function parseArgs(argv) {
  const o = { buildReleases: false, run: false, stop: false, force: false, help: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => {
      const v = argv[++i];
      if (v === undefined) fail(`${a} precisa de um valor`);
      return v;
    };
    switch (a) {
      case '--build-releases': o.buildReleases = true; break;
      case '--run': o.run = true; break;
      case '--stop': o.stop = true; break;
      case '--force': o.force = true; break;
      case '--dir': o.dir = val(); break;
      case '--key': o.key = val(); break;
      case '--cb-server': o.cbServer = val(); break;
      case '--hub-port': o.hubPort = port(a, val()); break;
      case '--game-port': o.gamePort = port(a, val()); break;
      case '-h': case '--help': o.help = true; break;
      default: fail(`opção desconhecida: ${a} (use --help)`);
    }
  }
  return o;
}

function port(flag, v) {
  const n = Number(v);
  if (!/^[1-9]\d{0,4}$/.test(v) || n > 65535) fail(`${flag} inválida: ${v}`);
  return n;
}

function usage() {
  const text = readFileSync(fileURLToPath(import.meta.url), 'utf8').split('\n');
  const end = text.findIndex((l) => l.startsWith('import '));
  console.log(text.slice(1, end).map((l) => l.replace(/^\/\/ ?/, '')).join('\n').trim());
}

function fail(msg) {
  console.error(`\n[dev-env] ERRO: ${msg}\n`);
  process.exit(1);
}

const log = (msg) => console.log(`[dev-env] ${msg}`);

// ------------------------------------------------------------------------------------------------------- caminhos

function paths(o) {
  const dir = resolve(o.dir ?? process.env.CBMP_DEV_DIR ?? join(tmpdir(), 'cbmp-dev-env'));
  return {
    dir,
    releases: join(dir, 'releases'),
    hubData: join(dir, 'hub-data'),
    server: join(dir, 'server'),
    logs: join(dir, 'logs'),
    state: join(dir, 'state.json'),
    pids: join(dir, 'pids.json'),
    key: resolve(o.key ?? process.env.CBMP_DEV_KEY ?? join(homedir(), '.cbmp-devkeys', 'update.key')),
    cbServer: resolve(o.cbServer ?? process.env.CBMP_CB_SERVER ?? join(REPO, 'build-native', 'windows-release', 'server', 'Release', 'cb-server.exe')),
  };
}

const inside = (parent, child) => {
  const rel = relative(parent, child);
  return rel === '' || (!rel.startsWith('..') && !isAbsolute(rel));
};

// ------------------------------------------------------------------------------------------------------ processos

/** Roda e espera; saída herdada (builds). */
function runSync(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { stdio: 'inherit', ...opts });
  if (r.error) fail(`não foi possível rodar ${cmd}: ${r.error.message}`);
  if (r.status !== 0) fail(`${cmd} ${args.join(' ')} terminou com código ${r.status}`);
}

/** Mata a árvore de processos (Windows: taskkill /T /F). */
function killTree(pid) {
  if (!pid) return;
  if (process.platform === 'win32') spawnSync('taskkill', ['/PID', String(pid), '/T', '/F'], { stdio: 'ignore' });
  else { try { process.kill(pid, 'SIGTERM'); } catch { /* já saiu */ } }
}

/**
 * Identidade de processos vivos: pid → { name, start } (nome da imagem sem extensão, minúsculo; início em UTC).
 * Um pid gravado só é morto se nome E hora de início ainda baterem: o Windows reaproveita pids, e um pids.json velho
 * (janela fechada, reboot) não pode derrubar o navegador ou o jogo de ninguém.
 */
function identities(pids) {
  const list = pids.filter((n) => Number.isInteger(n) && n > 0);
  const out = new Map();
  if (list.length === 0) return out;
  let rows = [];
  if (process.platform === 'win32') {
    const cmd = `Get-Process -Id ${list.join(',')} -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Id)|$($_.ProcessName)|$($_.StartTime.ToUniversalTime().ToString('o'))" }`;
    const text = spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', cmd], { encoding: 'utf8' }).stdout ?? '';
    rows = text.split(/\r?\n/).map((l) => l.trim().split('|'));
  } else {
    const text = spawnSync('ps', ['-o', 'pid=,comm=,lstart=', '-p', list.join(',')], { encoding: 'utf8' }).stdout ?? '';
    rows = text.split(/\r?\n/).map((l) => /^\s*(\d+)\s+(\S+)\s+(.+?)\s*$/.exec(l)?.slice(1) ?? []);
  }
  for (const [id, name, start] of rows) {
    if (id && name && start) out.set(Number(id), { name: name.toLowerCase().replace(/\.exe$/, ''), start });
  }
  return out;
}

// -------------------------------------------------------------------------------------------------------- releases

function readKey(p) {
  if (inside(REPO, p.key)) fail(`a chave privada não pode ficar dentro do repositório (${p.key}). Guarde-a fora (ex.: ${join(homedir(), '.cbmp-devkeys')}).`);
  if (!existsSync(p.key)) fail(`chave privada dev não encontrada em ${p.key} (use --key ou CBMP_DEV_KEY)`);
  return readFileSync(p.key, 'utf8').trim();
}

/** Comentário confiável de um `.sig` do Tauri (base64 do arquivo do minisign). */
function trustedComment(sigB64) {
  const text = Buffer.from(sigB64.trim(), 'base64').toString('utf8');
  return text.split(/\r?\n/).find((l) => l.startsWith('trusted comment: '))?.slice('trusted comment: '.length) ?? '';
}

function buildMod(p, o) {
  const tag = `mod-p${MOD_PROTOCOL}-v${MOD_VERSION}`;
  const out = join(p.releases, tag);
  const zip = `cbmp-mod-p${MOD_PROTOCOL}-v${MOD_VERSION}.zip`;
  if (!o.force && [zip, `${zip}.sig`, `${zip}.sha256`].every((f) => existsSync(join(out, f)))) {
    log(`${tag}: já existe (use --force para refazer)`);
    return;
  }
  rmSync(out, { recursive: true, force: true });
  log(`${tag}: gerando e assinando o mod de teste…`);
  runSync('cargo', ['run', '--quiet', '--locked', '--example', 'dev_sign_mod', '--',
    '--key', p.key, '--protocol', String(MOD_PROTOCOL), '--version', MOD_VERSION, '--out', out],
  { cwd: join(REPO, 'launcher', 'src-tauri') });
  writeFileSync(join(out, 'notes.txt'), `Mod de teste ${MOD_VERSION} (protocolo ${MOD_PROTOCOL}), gerado por tools/dev-env.mjs.\n`);
}

// O que entra no binário do launcher (testes e examples ficam de fora): um release pronto só é reaproveitado se isso
// não mudou desde o build. Sem essa conferência, um instalador antigo (outra pasta de dados, outro Hub) passaria por novo.
const LAUNCHER_INPUTS = ['launcher/src-tauri/src', 'launcher/src-tauri/Cargo.toml', 'launcher/src-tauri/Cargo.lock',
  'launcher/src-tauri/build.rs', 'launcher/src-tauri/tauri.conf.json', 'launcher/src-tauri/tauri.localtest.conf.json',
  'launcher/src-tauri/capabilities', 'launcher/src-tauri/permissions', 'launcher/src-tauri/keys', 'launcher/src-tauri/icons',
  // A UI inteira (node_modules/dist ficam de fora pelo .gitignore; testes pelo filtro abaixo).
  'launcher/ui', 'package.json', 'package-lock.json'];

/** sha256 do conteúdo dos arquivos que entram no launcher (versionados e novos não ignorados; sem testes). */
function sourceStamp() {
  const r = spawnSync('git', ['ls-files', '-z', '--cached', '--others', '--exclude-standard', '--', ...LAUNCHER_INPUTS], { cwd: REPO, encoding: 'utf8' });
  if (r.status !== 0) fail('git ls-files falhou (o dev-env precisa rodar dentro do repositório git)');
  const files = [...new Set(r.stdout.split('\0').filter(Boolean))].sort();
  const h = createHash('sha256');
  for (const f of files) {
    if (/(^|\/)(tests|__tests__)\//.test(f) || /\.test\.ts$/.test(f)) continue;
    const full = join(REPO, f);
    if (!existsSync(full)) continue; // apagado e ainda não commitado
    h.update(`${f}\0`).update(readFileSync(full)).update('\0');
  }
  return h.digest('hex');
}

function buildLauncher(p, o, version, keyText, stamp) {
  const tag = `launcher-v${version}`;
  const out = join(p.releases, tag);
  const exe = `${PRODUCT}_${version}_x64-setup.exe`;
  // Nome com ponto inicial: o Hub (HUB_RELEASES_DIR) não o lista como asset.
  const stampFile = join(out, '.source-stamp');
  const built = existsSync(join(out, exe)) && existsSync(join(out, `${exe}.sig`));
  const same = built && existsSync(stampFile) && readFileSync(stampFile, 'utf8').trim() === stamp;
  if (!o.force && same) {
    log(`${tag}: já existe e o código do launcher não mudou (use --force para refazer)`);
    return;
  }
  if (built && !o.force) log(`${tag}: o código do launcher mudou desde o build anterior (ou build sem registro); refazendo`);
  const bundle = join(REPO, 'launcher', 'src-tauri', 'target', 'release', 'bundle', 'nsis');
  rmSync(bundle, { recursive: true, force: true });
  log(`${tag}: build release do launcher de teste local (alguns minutos)…`);
  const extra = JSON.stringify({ version, bundle: { createUpdaterArtifacts: true } });
  const env = { ...process.env, TAURI_SIGNING_PRIVATE_KEY: keyText, TAURI_SIGNING_PRIVATE_KEY_PASSWORD: process.env.CBMP_KEY_PASSWORD ?? '' };
  runSync(process.execPath, [join(REPO, 'launcher', 'ui', 'tauri.mjs'), 'build', '--ci', '--features', 'local-test',
    '--config', join(REPO, 'launcher', 'src-tauri', 'tauri.localtest.conf.json'), '--config', extra, '--', '--locked'],
  { cwd: join(REPO, 'launcher', 'ui'), env });
  for (const f of [exe, `${exe}.sig`]) if (!existsSync(join(bundle, f))) fail(`build não gerou ${join(bundle, f)}`);
  const comment = trustedComment(readFileSync(join(bundle, `${exe}.sig`), 'utf8'));
  if (!comment.split('\t').includes(`version:${version}`)) fail(`assinatura de ${exe} sem version:${version} no comentário confiável ("${comment}")`);
  rmSync(out, { recursive: true, force: true });
  mkdirSync(out, { recursive: true });
  for (const f of [exe, `${exe}.sig`]) copyFileSync(join(bundle, f), join(out, f));
  writeFileSync(join(out, 'notes.txt'), `Versão de teste ${version} do launcher (Teste local), gerada por tools/dev-env.mjs.\n`);
  writeFileSync(stampFile, `${stamp}\n`);
  log(`${tag}: pronto (${comment.replace(/\t/g, ' ')})`);
}

function buildReleases(p, o) {
  const keyText = readKey(p);
  mkdirSync(p.releases, { recursive: true });
  // O mod primeiro: o assinador confere a chave com keys/update.pub e para tudo se não for o par.
  buildMod(p, o);
  const stamp = sourceStamp();
  for (const v of LAUNCHER_VERSIONS) buildLauncher(p, o, v, keyText, stamp);
  log(`releases em ${p.releases}`);
}

// ------------------------------------------------------------------------------------------------------------- run

async function portFree(port) {
  return new Promise((res) => {
    const s = createServer();
    s.once('error', () => res(false));
    s.listen(port, '127.0.0.1', () => s.close(() => res(true)));
  });
}

async function http(method, path, { token, body, redirect = 'follow' } = {}) {
  const headers = {};
  if (token) headers.authorization = `Bearer ${token}`;
  if (body !== undefined) headers['content-type'] = 'application/json';
  const res = await fetch(`${HUB_URL}${path}`, { method, headers, body: body === undefined ? undefined : JSON.stringify(body), redirect });
  const text = await res.text();
  let json;
  try { json = text ? JSON.parse(text) : undefined; } catch { json = undefined; }
  return { status: res.status, headers: res.headers, json, text };
}

async function waitFor(what, fn, ms, isAlive) {
  const end = Date.now() + ms;
  for (;;) {
    try { if (await fn()) return; } catch { /* ainda não */ }
    if (isAlive && !isAlive()) fail(`${what}: o processo terminou antes de ficar pronto (veja os logs)`);
    if (Date.now() > end) fail(`${what}: não ficou pronto em ${ms / 1000} s (veja os logs)`);
    await new Promise((r) => setTimeout(r, 500));
  }
}

/** Login de dev com PKCE (como o launcher faz), devolve o access token. Nunca é impresso. */
async function devLogin() {
  const verifier = randomBytes(32).toString('base64url');
  const challenge = createHash('sha256').update(verifier).digest('base64url');
  const q = new URLSearchParams({ client: 'launcher', code_challenge: challenge, code_challenge_method: 'S256', discordId: DEV_DID, name: DEV_NAME });
  const r = await http('GET', `/v1/auth/dev-login?${q}`, { redirect: 'manual' });
  const code = new URL(r.headers.get('location') ?? 'x:').searchParams.get('code');
  if (!code) fail(`dev-login falhou (HTTP ${r.status})`);
  const ex = await http('POST', '/v1/auth/exchange', { body: { code, codeVerifier: verifier } });
  if (ex.status !== 200) fail(`troca do código falhou (HTTP ${ex.status})`);
  return ex.json.accessToken;
}

function readState(p) {
  try { return JSON.parse(readFileSync(p.state, 'utf8')); } catch { return {}; }
}

/** Cadastra (ou reaproveita) o servidor de teste e marca como destaque. Devolve { id, key }. */
async function ensureServer(p, token) {
  const state = readState(p);
  const mine = await http('GET', '/v1/owner/servers', { token });
  if (mine.status !== 200) fail(`listar servidores do dono falhou (HTTP ${mine.status})`);
  let srv = mine.json.find((s) => s.name === SERVER_NAME);
  let key = srv && state.serverId === srv.id ? state.serverKey : undefined;
  if (!srv) {
    const c = await http('POST', '/v1/owner/servers', { token, body: { name: SERVER_NAME, description: 'Servidor local de teste (tools/dev-env.mjs): hello-client.', tags: ['teste'], locale: 'pt-BR' } });
    if (c.status !== 201) fail(`cadastro do servidor falhou (HTTP ${c.status}: ${c.text})`);
    srv = c.json.server;
    key = c.json.key;
    log(`servidor cadastrado: ${srv.id}`);
  } else if (!key) {
    const r = await http('POST', `/v1/owner/servers/${srv.id}/rotate-key`, { token });
    if (r.status !== 200) fail(`nova chave do servidor falhou (HTTP ${r.status})`);
    key = r.json.key;
    log(`servidor reaproveitado (${srv.id}), chave renovada`);
  } else {
    log(`servidor reaproveitado (${srv.id})`);
  }
  writeFileSync(p.state, JSON.stringify({ serverId: srv.id, serverKey: key }, null, 2));
  const f = await http('PATCH', `/v1/admin/servers/${srv.id}`, { token, body: { featured: true } });
  if (f.status !== 200) fail(`marcar destaque falhou (HTTP ${f.status}: ${f.text})`);
  return { id: srv.id, key };
}

function writeServerConfig(p, srv) {
  mkdirSync(p.server, { recursive: true });
  const res = join(p.server, 'resources');
  rmSync(res, { recursive: true, force: true });
  cpSync(join(REPO, 'resources.example', 'hello-client'), join(res, 'hello-client'), { recursive: true });
  const cfg = {
    name: SERVER_NAME,
    description: 'Servidor local de teste (tools/dev-env.mjs): hello-client.',
    tags: ['teste'],
    locale: 'pt-BR',
    maxPlayers: 32,
    port: GAME_PORT,
    hub: { url: HUB_URL, serverId: srv.id, key: srv.key },
    resourcesDir: res,
    logsDir: join(p.server, 'logs'),
    scripting: { bansFile: join(p.server, 'bans.json') },
  };
  writeFileSync(join(p.server, 'server.cfg'), JSON.stringify(cfg, null, 2));
}

function startChild(name, cmd, args, opts, logFile) {
  const out = createWriteStream(logFile, { flags: 'a' });
  const child = spawn(cmd, args, { ...opts, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
  child.stdout.pipe(out);
  child.stderr.pipe(out);
  child.on('error', (e) => fail(`${name} não iniciou: ${e.message}`));
  return child;
}

function stopFromPidFile(p, quiet = false) {
  let saved;
  try { saved = JSON.parse(readFileSync(p.pids, 'utf8')); } catch { if (!quiet) log('nada rodando (sem pids.json)'); return; }
  // Só entradas completas (pid + nome + início); formato antigo (só o pid) é ignorado: sem identidade, não mata.
  const entries = Object.entries(saved).filter(([, e]) => e && typeof e === 'object' && Number.isInteger(e.pid) && e.name && e.start);
  const live = identities(entries.map(([, e]) => e.pid));
  for (const [what, e] of entries) {
    if (e.pid === process.pid) continue;
    const now = live.get(e.pid);
    if (!now) { if (!quiet) log(`${what} (pid ${e.pid}) já tinha terminado`); continue; }
    if (now.name !== e.name || now.start !== e.start) {
      log(`${what}: o pid ${e.pid} agora é outro processo (${now.name}); não mexi nele`);
      continue;
    }
    killTree(e.pid);
    if (!quiet) log(`${what} (pid ${e.pid}) encerrado`);
  }
  rmSync(p.pids, { force: true });
}

async function run(p) {
  if (process.platform !== 'win32') log('aviso: feito para Windows; em outro sistema ajuste --cb-server');
  if (!existsSync(p.cbServer)) fail(`cb-server não encontrado em ${p.cbServer}. Compile antes (server/README.md, preset windows-release) ou use --cb-server.`);
  // Sobras de um --run anterior que não foi encerrado com Ctrl+C.
  stopFromPidFile(p, true);
  if (!(await portFree(HUB_PORT))) fail(`a porta ${HUB_PORT} está em uso (outro Hub rodando?). Encerre-o ou rode "node tools/dev-env.mjs --stop".`);
  if (!(await portFree(GAME_PORT))) fail(`a porta ${GAME_PORT} está em uso (outro cb-server rodando?).`);
  for (const d of [p.releases, p.hubData, p.logs]) mkdirSync(d, { recursive: true });

  const children = {};
  let stopping = false;
  const stopAll = (code) => {
    if (stopping) return;
    stopping = true;
    log('encerrando…');
    // Filhos deste processo: o pid é deles enquanto não saíram.
    for (const c of Object.values(children)) if (c.exitCode === null && c.signalCode === null) killTree(c.pid);
    rmSync(p.pids, { force: true });
    process.exit(code);
  };
  process.on('SIGINT', () => stopAll(0));
  process.on('SIGTERM', () => stopAll(0));
  process.on('SIGBREAK', () => stopAll(0)); // Ctrl+Break
  process.on('SIGHUP', () => stopAll(0)); // janela do console fechada (o Windows dá ~10 s para limpar)
  // pid + nome + hora de início de cada processo: o --stop/--run seguinte só mata o que ainda for nosso.
  const savePids = () => {
    const pids = { 'dev-env': process.pid, ...Object.fromEntries(Object.entries(children).map(([k, c]) => [k, c.pid])) };
    const ids = identities(Object.values(pids));
    const out = {};
    for (const [k, pid] of Object.entries(pids)) {
      const id = ids.get(pid);
      if (id) out[k] = { pid, ...id };
    }
    writeFileSync(p.pids, JSON.stringify(out, null, 2));
  };

  // Hub: só 127.0.0.1, login de dev, servidores em IP privado, releases da pasta local. Nada de token do GitHub.
  const hubEnv = { ...process.env };
  for (const k of Object.keys(hubEnv)) if (k.startsWith('HUB_') || k.startsWith('DISCORD_') || k === 'NODE_ENV') delete hubEnv[k];
  Object.assign(hubEnv, {
    HUB_HOST: '127.0.0.1',
    HUB_PORT: String(HUB_PORT),
    HUB_PUBLIC_URL: HUB_URL,
    HUB_DATA_DIR: p.hubData,
    HUB_DEV_LOGIN: '1',
    HUB_ALLOW_PRIVATE_SERVERS: '1',
    HUB_RELEASES_DIR: p.releases,
    HUB_PLATFORM_ADMINS: DEV_DID,
    // App Discord de teste (opcional): só por variável de ambiente, nunca em arquivo.
    ...(process.env.DISCORD_CLIENT_ID && process.env.DISCORD_CLIENT_SECRET
      ? { DISCORD_CLIENT_ID: process.env.DISCORD_CLIENT_ID, DISCORD_CLIENT_SECRET: process.env.DISCORD_CLIENT_SECRET } : {}),
  });
  const hubLog = join(p.logs, 'hub.log');
  log(`subindo o Hub em ${HUB_URL} (log: ${hubLog})`);
  children.hub = startChild('Hub', process.execPath, [join(REPO, 'node_modules', 'tsx', 'dist', 'cli.mjs'), 'src/main.ts'],
    { cwd: join(REPO, 'hub'), env: hubEnv }, hubLog);
  children.hub.on('exit', (c) => { if (!stopping) { console.error(`[dev-env] o Hub parou (código ${c}); veja ${hubLog}`); stopAll(1); } });
  savePids();
  await waitFor('Hub', async () => (await http('GET', '/health')).status === 200, 60_000, () => children.hub.exitCode === null);

  const token = await devLogin();
  const srv = await ensureServer(p, token);
  writeServerConfig(p, srv);

  const srvLog = join(p.logs, 'cb-server.log');
  log(`subindo o cb-server na porta ${GAME_PORT} (log: ${srvLog})`);
  children.cbServer = startChild('cb-server', p.cbServer, ['--config', join(p.server, 'server.cfg')], { cwd: p.server }, srvLog);
  children.cbServer.on('exit', (c) => { if (!stopping) { console.error(`[dev-env] o cb-server parou (código ${c}); veja ${srvLog}`); stopAll(1); } });
  savePids();
  await waitFor('servidor na lista do Hub', async () => {
    const r = await http('GET', '/v1/servers');
    // /v1/servers só lista servidores online (heartbeat recente e verificado pelo Hub).
    return r.status === 200 && Array.isArray(r.json) && r.json.some((s) => s.id === srv.id);
  }, 60_000, () => children.cbServer.exitCode === null);

  await report(p, srv);
  log('rodando. Ctrl+C encerra Hub e cb-server.');
  await new Promise(() => {});
}

async function report(p, srv) {
  const list = await http('GET', '/v1/servers');
  const s = Array.isArray(list.json) ? list.json.find((x) => x.id === srv.id) : undefined;
  const up = await http('GET', '/v1/launcher/update/windows/x86_64/0.1.0');
  const mod = await http('GET', `/v1/mod/latest?protocol=${MOD_PROTOCOL}`);
  const ok = (b) => (b ? 'OK  ' : 'FALTA');
  const installer = join(p.releases, 'launcher-v0.1.0', `${PRODUCT}_0.1.0_x64-setup.exe`);
  const haveInstaller = existsSync(installer);
  console.log(`
==========================================================================================
 Ambiente de teste local do CB Multiplayer
==========================================================================================
 [${ok(!!s?.featured)}] Hub ${HUB_URL} lista "${SERVER_NAME}" em destaque e online (${s?.id ?? '?'})
 [${ok(up.status === 200 && up.json?.version === '0.1.1')}] update 0.1.0 → ${up.json?.version ?? `HTTP ${up.status}`}
 [${ok(mod.status === 200)}] mod de teste: ${mod.status === 200 ? `${mod.json.version} (protocolo ${mod.json.protocol})` : `HTTP ${mod.status}`}
${haveInstaller ? '' : `
 Releases de teste ausentes: rode antes "node tools/dev-env.mjs --build-releases" (alguns minutos).
`}
 O que fazer (só nesta máquina):
  Passo a passo completo: docs/TESTES-MANUAIS-SUB4.md. Resumo:
  1. Instale o launcher de teste: ${installer}
     (nome "${PRODUCT}"; o SmartScreen pode avisar: "Mais informações" → "Executar assim mesmo").
  2. Abra o "${PRODUCT}" e clique em "Entrar com Discord". O navegador abre a página do Hub de teste:
     confirme "Entrar como dev" (conta fictícia ${DEV_NAME}). Login com Discord de verdade só com
     DISCORD_CLIENT_ID/DISCORD_CLIENT_SECRET de um app de teste nas variáveis de ambiente (nunca em arquivo).
  3. A lista mostra "${SERVER_NAME}" em destaque. Com "Modo simular" ligado (padrão), Jogar baixa e confere o
     hello-client e para em "Pronto para abrir" sem abrir o jogo (o mod de teste só é baixado fora do modo simular).
  4. Auto-update: o launcher 0.1.0 encontra a 0.1.1 neste Hub (Configurações → Instalar atualização).
  5. Pastas: %LOCALAPPDATA%\\CBMultiplayer-TesteLocal (cache, servers, mods, logs\\launcher.log); o build de
     teste local tem pasta própria (o oficial usa %LOCALAPPDATA%\\CBMultiplayer).

 Painel do Hub: ${HUB_URL}/panel   Logs: ${p.logs}   Dados: ${p.dir}
 Ctrl+C encerra tudo (ou, de outro terminal: node tools/dev-env.mjs --stop).
==========================================================================================
`);
}

// ------------------------------------------------------------------------------------------------------------ main

const o = parseArgs(process.argv.slice(2));
if (o.help || (!o.buildReleases && !o.run && !o.stop)) {
  usage();
  process.exit(o.help ? 0 : 1);
}
if (o.hubPort) { HUB_PORT = o.hubPort; HUB_URL = `http://127.0.0.1:${HUB_PORT}`; }
if (o.gamePort) GAME_PORT = o.gamePort;
const p = paths(o);
if (inside(REPO, p.dir)) fail(`a pasta de trabalho não pode ficar dentro do repositório (${p.dir})`);
mkdirSync(p.dir, { recursive: true });
if (o.stop) stopFromPidFile(p);
if (o.buildReleases) buildReleases(p, o);
if (o.run) await run(p);
