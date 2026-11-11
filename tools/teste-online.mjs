#!/usr/bin/env node
// Ambiente de TESTE ONLINE do CB Multiplayer (Hub https://cbmultiplayer.cntoficial.com.br + cb-server na VPS).
// Ops em docs/AMBIENTE-TESTE-ONLINE.md; guia do testador em docs/TESTE-ONLINE.md.
//
//   node tools/teste-online.mjs --build-releases   launcher "CB Multiplayer (Teste)" 0.1.0 e 0.1.1 + mod de teste
//                                                  mod-p1-v0.1.0, assinados com a chave dev (LOCAL; a chave nunca
//                                                  sai desta máquina). Copia o instalador 0.1.0 para
//                                                  dist/CB-Multiplayer-Teste-setup.exe (o que os testadores baixam).
//   node tools/teste-online.mjs --deploy           empacota Hub (bundle esbuild), hello-client, releases assinadas e
//                                                  (opcional) o cb-server Linux, envia por scp e instala na VPS
//   node tools/teste-online.mjs --cert             emite o certificado (certbot --webroot) e liga o HTTPS (1ª vez)
//   node tools/teste-online.mjs --firewall         libera 30500/udp e 30500/tcp no ufw (1ª vez)
//   node tools/teste-online.mjs --status           estado dos serviços na VPS
//   node tools/teste-online.mjs --check            valida pela internet (Hub, lista, conteúdo, mod, update, login)
//
// Opções: --ssh <usuario@host> (ou CBMP_TESTE_SSH; porta em CBMP_TESTE_SSH_PORT, padrão 22)
//         --server-tar <cb-server-linux-x64.tar.gz>  pacote do workflow server-linux-dist (gh run download ...)
//         --server-sha256 <hex> --server-commit <sha>  obrigatórios com --server-tar: sha256 e commit do resumo do run;
//                        conferidos aqui e de novo na VPS antes de instalar
//         --dir <pasta>  trabalho local (padrão %TEMP%\cbmp-teste-online; releases em <pasta>\releases)
//         --key <arquivo> chave privada dev (padrão %USERPROFILE%\.cbmp-devkeys\update.key; ou CBMP_DEV_KEY).
//                         Lida só para assinar; nunca copiada, enviada ou impressa. Recusada dentro do repositório.
//         --force        refaz releases que já existem
//         --no-releases  --deploy sem mexer nas releases da VPS

import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { homedir, tmpdir } from 'node:os';
import { dirname, isAbsolute, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { sourceStamp } from './lib/source-stamp.mjs';
import { hubLockfile } from './lib/hub-lockfile.mjs';

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const HOST = 'cbmultiplayer.cntoficial.com.br';
const HUB = `https://${HOST}`;
const GAME_IP = '143.95.209.35';
const GAME_PORT = 30500;
const ROOT = '/var/www/html_cbmultiplayer';
const SERVER_NAME = 'CB Multiplayer – Servidor de teste';
const PRODUCT = 'CB Multiplayer (Teste)';
const LAUNCHER_VERSIONS = ['0.1.0', '0.1.1'];
const INSTALLER_VERSION = '0.1.0';
const MOD_PROTOCOL = 1;
const MOD_VERSION = '0.1.0';
const OVERLAY = join(REPO, 'launcher', 'src-tauri', 'tauri.onlinetest.conf.json');

const log = (m) => console.log(`[teste-online] ${m}`);
function fail(m) { console.error(`\n[teste-online] ERRO: ${m}\n`); process.exit(1); }

function parseArgs(argv) {
  const o = { force: false, releases: true };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => argv[++i] ?? fail(`${a} precisa de um valor`);
    switch (a) {
      case '--build-releases': case '--deploy': case '--cert': case '--firewall': case '--status': case '--check':
        o.cmd = a.slice(2); break;
      case '--ssh': o.ssh = val(); break;
      case '--server-tar': o.serverTar = resolve(val()); break;
      case '--server-sha256': o.serverSha = val().toLowerCase(); break;
      case '--server-commit': o.serverCommit = val().toLowerCase(); break;
      case '--dir': o.dir = val(); break;
      case '--key': o.key = val(); break;
      case '--force': o.force = true; break;
      case '--no-releases': o.releases = false; break;
      case '-h': case '--help': o.cmd = 'help'; break;
      default: fail(`opção desconhecida: ${a} (use --help)`);
    }
  }
  return o;
}

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { stdio: 'inherit', ...opts });
  if (r.error) fail(`não foi possível rodar ${cmd}: ${r.error.message}`);
  if (r.status !== 0) fail(`${cmd} ${args.join(' ')} terminou com código ${r.status}`);
}

const inside = (parent, child) => { const rel = relative(parent, child); return rel === '' || (!rel.startsWith('..') && !isAbsolute(rel)); };
const workDir = (o) => resolve(o.dir ?? join(tmpdir(), 'cbmp-teste-online'));

// ------------------------------------------------------------------------------------------------ releases (local)

function keyPath(o) {
  const p = resolve(o.key ?? process.env.CBMP_DEV_KEY ?? join(homedir(), '.cbmp-devkeys', 'update.key'));
  if (inside(REPO, p)) fail('a chave privada não pode ficar dentro do repositório');
  if (!existsSync(p)) fail(`chave privada dev não encontrada (use --key ou CBMP_DEV_KEY)`);
  return p;
}

function trustedComment(sigB64) {
  const text = Buffer.from(sigB64.trim(), 'base64').toString('utf8');
  return text.split(/\r?\n/).find((l) => l.startsWith('trusted comment: '))?.slice('trusted comment: '.length) ?? '';
}

function buildMod(rel, key, o) {
  const tag = `mod-p${MOD_PROTOCOL}-v${MOD_VERSION}`;
  const out = join(rel, tag);
  const zip = `cbmp-mod-p${MOD_PROTOCOL}-v${MOD_VERSION}.zip`;
  if (!o.force && [zip, `${zip}.sig`, `${zip}.sha256`].every((f) => existsSync(join(out, f)))) { log(`${tag}: já existe`); return; }
  rmSync(out, { recursive: true, force: true });
  log(`${tag}: gerando e assinando o mod de teste…`);
  run('cargo', ['run', '--quiet', '--locked', '--example', 'dev_sign_mod', '--', '--key', key, '--protocol', String(MOD_PROTOCOL),
    '--version', MOD_VERSION, '--out', out], { cwd: join(REPO, 'launcher', 'src-tauri') });
  writeFileSync(join(out, 'notes.txt'), `Mod de teste ${MOD_VERSION} (protocolo ${MOD_PROTOCOL}): só um LEIA-ME, sem código.\n`);
}

function buildLauncher(rel, key, version, stamp, o) {
  const tag = `launcher-v${version}`;
  const out = join(rel, tag);
  const exe = `${PRODUCT}_${version}_x64-setup.exe`;
  const stampFile = join(out, '.source-stamp');
  const built = existsSync(join(out, exe)) && existsSync(join(out, `${exe}.sig`));
  if (!o.force && built && existsSync(stampFile) && readFileSync(stampFile, 'utf8').trim() === stamp) {
    log(`${tag}: já existe e o código do launcher não mudou`);
    return;
  }
  const bundle = join(REPO, 'launcher', 'src-tauri', 'target', 'release', 'bundle', 'nsis');
  rmSync(bundle, { recursive: true, force: true });
  log(`${tag}: build release do launcher de teste online (alguns minutos)…`);
  const extra = JSON.stringify({ version, bundle: { createUpdaterArtifacts: true } });
  const env = { ...process.env, TAURI_SIGNING_PRIVATE_KEY: readFileSync(key, 'utf8').trim(), TAURI_SIGNING_PRIVATE_KEY_PASSWORD: process.env.CBMP_KEY_PASSWORD ?? '' };
  run(process.execPath, [join(REPO, 'launcher', 'ui', 'tauri.mjs'), 'build', '--ci', '--features', 'online-test', '--config', OVERLAY,
    '--config', extra, '--', '--locked'], { cwd: join(REPO, 'launcher', 'ui'), env });
  for (const f of [exe, `${exe}.sig`]) if (!existsSync(join(bundle, f))) fail(`o build não gerou ${f}`);
  const comment = trustedComment(readFileSync(join(bundle, `${exe}.sig`), 'utf8'));
  if (!comment.split('\t').includes(`version:${version}`)) fail(`assinatura de ${exe} sem version:${version}`);
  // O binário de teste online não pode apontar para o Hub local.
  const bin = readFileSync(join(REPO, 'launcher', 'src-tauri', 'target', 'release', 'CBMultiplayerTeste.exe'));
  if (!bin.includes(Buffer.from(HUB)) || bin.includes(Buffer.from('127.0.0.1:8787'))) fail('binário de teste online com Hub errado');
  rmSync(out, { recursive: true, force: true });
  mkdirSync(out, { recursive: true });
  for (const f of [exe, `${exe}.sig`]) copyFileSync(join(bundle, f), join(out, f));
  writeFileSync(join(out, 'notes.txt'), `Versão de teste ${version} do launcher CB Multiplayer (Teste).\n`);
  writeFileSync(stampFile, `${stamp}\n`);
  log(`${tag}: pronto (${comment.replace(/\t/g, ' ')})`);
}

function buildReleases(o) {
  const key = keyPath(o);
  const rel = join(workDir(o), 'releases');
  mkdirSync(rel, { recursive: true });
  buildMod(rel, key, o);
  const stamp = sourceStamp(REPO);
  for (const v of LAUNCHER_VERSIONS) buildLauncher(rel, key, v, stamp, o);
  mkdirSync(join(REPO, 'dist'), { recursive: true });
  const src = join(rel, `launcher-v${INSTALLER_VERSION}`, `${PRODUCT}_${INSTALLER_VERSION}_x64-setup.exe`);
  const dst = join(REPO, 'dist', 'CB-Multiplayer-Teste-setup.exe');
  copyFileSync(src, dst);
  const sha = createHash('sha256').update(readFileSync(dst)).digest('hex');
  writeFileSync(`${dst}.sha256`, `${sha}  CB-Multiplayer-Teste-setup.exe\n`);
  log(`instalador dos testadores: ${dst} (sha256 ${sha})`);
  log(`releases em ${rel}`);
}

// ------------------------------------------------------------------------------------------------------- deploy

function sshTarget(o) {
  const t = o.ssh ?? process.env.CBMP_TESTE_SSH;
  if (!t || !/^[A-Za-z0-9._-]+@[A-Za-z0-9.-]+$/.test(t)) fail('informe --ssh usuario@host (ou CBMP_TESTE_SSH)');
  return { target: t, port: process.env.CBMP_TESTE_SSH_PORT ?? '22' };
}

function ssh(o, command, opts = {}) {
  const { target, port } = sshTarget(o);
  run('ssh', ['-p', port, '-o', 'BatchMode=yes', target, command], opts);
}

function deploy(o) {
  const stage = join(workDir(o), 'stage');
  rmSync(stage, { recursive: true, force: true });
  mkdirSync(join(stage, 'hub'), { recursive: true });
  log('bundle do Hub (esbuild)…');
  const esbuild = join(REPO, 'node_modules', 'esbuild', 'bin', 'esbuild');
  run(process.execPath, [esbuild, 'hub/src/main.ts', 'hub/src/cli.ts', '--bundle', '--platform=node', '--format=esm', '--target=node22',
    '--external:better-sqlite3', `--outdir=${join(stage, 'hub')}`, '--out-extension:.js=.mjs', '--log-level=warning',
    "--banner:js=import { createRequire as __cbmpRequire } from 'node:module'; const require = __cbmpRequire(import.meta.url);"], { cwd: REPO });
  // Mesmas versões e hashes de integridade do package-lock do repositório; na VPS, npm ci --ignore-scripts.
  let hubPkg;
  try { hubPkg = hubLockfile(JSON.parse(readFileSync(join(REPO, 'package-lock.json'), 'utf8'))); } catch (e) { fail(e.message); }
  writeFileSync(join(stage, 'hub', 'package.json'), JSON.stringify(hubPkg.pkg, null, 2) + '\n');
  writeFileSync(join(stage, 'hub', 'package-lock.json'), JSON.stringify(hubPkg.lock, null, 2) + '\n');
  copyFileSync(join(REPO, 'tools', 'teste-online', 'hub.env'), join(stage, 'hub.env'));
  cpSync(join(REPO, 'tools', 'teste-online'), join(stage, 'teste-online'), { recursive: true });
  cpSync(join(REPO, 'resources.example', 'hello-client'), join(stage, 'resources', 'hello-client'), { recursive: true });
  if (o.releases) {
    const rel = join(workDir(o), 'releases');
    if (!existsSync(rel)) fail(`sem releases em ${rel}: rode --build-releases antes (ou use --no-releases)`);
    cpSync(rel, join(stage, 'releases'), { recursive: true });
  }
  const setup = join(REPO, 'dist', 'CB-Multiplayer-Teste-setup.exe');
  if (o.releases && existsSync(setup)) {
    mkdirSync(join(stage, 'download'), { recursive: true });
    copyFileSync(setup, join(stage, 'download', 'CB-Multiplayer-Teste-setup.exe'));
    if (existsSync(`${setup}.sha256`)) copyFileSync(`${setup}.sha256`, join(stage, 'download', 'CB-Multiplayer-Teste-setup.exe.sha256'));
  }
  if (o.serverTar) {
    if (!existsSync(o.serverTar)) fail(`pacote do cb-server não encontrado: ${o.serverTar}`);
    if (!/^[0-9a-f]{64}$/.test(o.serverSha ?? '')) fail('--server-sha256 <hex> (resumo do run server-linux-dist) é obrigatório com --server-tar');
    if (!/^[0-9a-f]{40}$/.test(o.serverCommit ?? '')) fail('--server-commit <sha de 40 dígitos> é obrigatório com --server-tar');
    const got = createHash('sha256').update(readFileSync(o.serverTar)).digest('hex');
    if (got !== o.serverSha) fail(`sha256 do pacote não confere: esperado ${o.serverSha}, arquivo ${got}`);
    copyFileSync(o.serverTar, join(stage, 'cb-server-linux-x64.tar.gz'));
    // tar do Windows: caminho relativo (o "C:" de um absoluto viraria host remoto).
    const c = spawnSync('tar', ['-xzOf', 'cb-server-linux-x64.tar.gz', 'cb-server-linux-x64/COMMIT'], { cwd: stage, encoding: 'utf8' });
    const inner = (c.stdout ?? '').trim();
    if (inner !== o.serverCommit) fail(`COMMIT de dentro do pacote (${inner || '?'}) difere de --server-commit ${o.serverCommit}`);
    writeFileSync(join(stage, 'SERVER_SHA256'), `${o.serverSha}\n`);
    writeFileSync(join(stage, 'SERVER_COMMIT'), `${o.serverCommit}\n`);
    log(`cb-server: commit ${o.serverCommit} e sha256 ${o.serverSha} conferidos`);
  }
  const commit = spawnSync('git', ['rev-parse', 'HEAD'], { cwd: REPO, encoding: 'utf8' }).stdout.trim();
  writeFileSync(join(stage, 'COMMIT'), `${commit}\n`);
  const tgz = join(workDir(o), 'stage.tgz');
  rmSync(tgz, { force: true });
  // tar do Windows (bsdtar): caminho relativo evita o "C:" ser lido como host remoto.
  run('tar', ['-czf', 'stage.tgz', '-C', 'stage', '.'], { cwd: workDir(o) });
  const { target, port } = sshTarget(o);
  log(`enviando para ${target}…`);
  ssh(o, `set -e; [ -d ${ROOT} ] || sudo -n install -d -o "$(id -un)" -g "$(id -gn)" -m 755 ${ROOT}; mkdir -p ${ROOT}/deploy`);
  run('scp', ['-P', port, '-o', 'BatchMode=yes', 'stage.tgz', `${target}:${ROOT}/deploy/stage.tgz`], { cwd: workDir(o) });
  ssh(o, `set -e; cd ${ROOT}/deploy; rm -rf incoming; mkdir incoming; tar -xzf stage.tgz -C incoming; rm -f stage.tgz; `
    + `bash incoming/teste-online/remote-install.sh install ${ROOT}/deploy/incoming; rm -rf incoming`);
  log('deploy concluído');
}

// -------------------------------------------------------------------------------------------------------- check

async function get(url, { redirect = 'follow', timeout = 15000 } = {}) {
  const res = await fetch(url, { redirect, signal: AbortSignal.timeout(timeout) });
  const buf = Buffer.from(await res.arrayBuffer());
  return { status: res.status, headers: res.headers, buf, text: buf.toString('utf8'), json() { return JSON.parse(this.text); } };
}

async function check() {
  const results = [];
  const step = async (name, fn) => {
    try { const detail = await fn(); results.push([true, name, detail ?? '']); console.log(`  ok   ${name}${detail ? ` — ${detail}` : ''}`); }
    catch (e) { results.push([false, name, e.message]); console.log(`  FALHA ${name} — ${e.message}`); }
  };
  const must = (cond, msg) => { if (!cond) throw new Error(msg); };
  let srv;
  let manifest;
  log(`validando ${HUB} pela internet`);
  await step('Hub /health', async () => { const r = await get(`${HUB}/health`); must(r.status === 200 && r.json().ok === true, `HTTP ${r.status}`); });
  await step('lista de servidores com o servidor de teste em destaque', async () => {
    const r = await get(`${HUB}/v1/servers`);
    must(r.status === 200, `HTTP ${r.status}`);
    srv = r.json().find((s) => s.name === SERVER_NAME);
    must(srv, `"${SERVER_NAME}" não está na lista (${r.json().length} servidores)`);
    must(srv.featured === true, 'não está em destaque');
    must(srv.host === GAME_IP && srv.gamePort === GAME_PORT && srv.httpPort === GAME_PORT, `endereço ${srv.host}:${srv.gamePort}/${srv.httpPort}`);
    return `${srv.id} em ${srv.host}:${srv.gamePort}, ${srv.players}/${srv.maxPlayers}`;
  });
  await step('detalhe do servidor: online', async () => {
    must(srv, 'sem servidor');
    const r = await get(`${HUB}/v1/servers/${srv.id}`);
    must(r.status === 200 && r.json().online === true, `HTTP ${r.status} online=${r.json().online}`);
  });
  const base = `http://${GAME_IP}:${GAME_PORT}`;
  await step('cb-server /info (TCP 30500)', async () => {
    const r = await get(`${base}/info`);
    must(r.status === 200, `HTTP ${r.status}`);
    must(!srv || r.json().serverId === srv.id, `serverId ${r.json().serverId}`);
    return `versão ${r.json().version}, protocolo ${r.json().protocol}`;
  });
  await step('manifest do cb-server', async () => {
    const r = await get(`${base}/cb/manifest`);
    must(r.status === 200, `HTTP ${r.status}`);
    manifest = r.json();
    const files = (manifest.resources ?? []).flatMap((x) => x.files ?? []);
    must(files.length > 0, 'manifest sem arquivos');
    return `${manifest.resources.length} resource(s), ${files.length} arquivo(s)`;
  });
  await step('download de arquivo do cb-server (sha256 confere)', async () => {
    const files = (manifest?.resources ?? []).flatMap((x) => x.files ?? []);
    must(files.length, 'sem manifest');
    for (const f of files) {
      const r = await get(`${base}/cb/files/${f.sha256}`);
      must(r.status === 200, `${f.path}: HTTP ${r.status}`);
      const sha = createHash('sha256').update(r.buf).digest('hex');
      must(sha === f.sha256 && r.buf.length === f.size, `${f.path}: conteúdo difere`);
    }
    return `${files.length} arquivo(s)`;
  });
  await step(`mod: /v1/mod/latest?protocol=${MOD_PROTOCOL} e download`, async () => {
    const r = await get(`${HUB}/v1/mod/latest?protocol=${MOD_PROTOCOL}`);
    must(r.status === 200, `HTTP ${r.status} ${r.text.slice(0, 120)}`);
    const m = r.json();
    must(m.url.startsWith(`${HUB}/v1/dl/`), `url ${m.url}`);
    const d = await get(m.url, { timeout: 60000 });
    must(d.status === 200, `download HTTP ${d.status}`);
    must(createHash('sha256').update(d.buf).digest('hex') === m.sha256, 'sha256 do zip difere');
    return `v${m.version}, ${d.buf.length} bytes`;
  });
  await step(`update do launcher: ${INSTALLER_VERSION} → mais nova`, async () => {
    const r = await get(`${HUB}/v1/launcher/update/windows/x86_64/${INSTALLER_VERSION}`);
    must(r.status === 200, `HTTP ${r.status}`);
    const u = r.json();
    must(u.url.startsWith(`${HUB}/v1/dl/`) && u.signature, 'sem url/assinatura');
    const comment = trustedComment(u.signature);
    must(comment.split('\t').includes(`version:${u.version}`), `assinatura sem version:${u.version}`);
    const d = await get(u.url, { timeout: 180000 });
    must(d.status === 200 && d.buf.length > 1_000_000, `download HTTP ${d.status} (${d.buf.length} bytes)`);
    const last = await get(`${HUB}/v1/launcher/update/windows/x86_64/${u.version}`);
    must(last.status === 204, `na versão mais nova devia ser 204, veio ${last.status}`);
    return `oferece ${u.version} (${d.buf.length} bytes)`;
  });
  await step('login: redireciona para o Discord com o redirect certo (ou 503 sem app Discord)', async () => {
    const r = await get(`${HUB}/v1/auth/discord/login?client=panel`, { redirect: 'manual' });
    if (r.status === 503) {
      must(r.json().error === 'login_unavailable', r.text);
      return `app Discord ainda não configurado: ${r.json().message}`;
    }
    must(r.status === 302, `HTTP ${r.status}`);
    const loc = new URL(r.headers.get('location'));
    must(loc.origin === 'https://discord.com', `destino ${loc.origin}`);
    must(loc.searchParams.get('redirect_uri') === `${HUB}/v1/auth/discord/callback`, `redirect_uri ${loc.searchParams.get('redirect_uri')}`);
    must(/^\d{17,20}$/.test(loc.searchParams.get('client_id') ?? ''), 'client_id ausente');
    return `client_id ${loc.searchParams.get('client_id')}`;
  });
  await step('instalador dos testadores para download', async () => {
    const r = await get(`${HUB}/CB-Multiplayer-Teste-setup.exe`, { timeout: 180000 });
    must(r.status === 200 && r.buf.length > 1_000_000, `HTTP ${r.status} (${r.buf.length} bytes)`);
    const s = await get(`${HUB}/CB-Multiplayer-Teste-setup.exe.sha256`);
    const sha = createHash('sha256').update(r.buf).digest('hex');
    must(s.status === 200 && s.text.startsWith(sha), 'sha256 publicado difere');
    return `${r.buf.length} bytes, sha256 ${sha.slice(0, 16)}…`;
  });
  await step('dev-login desligado no host público', async () => {
    const r = await get(`${HUB}/v1/auth/dev-login?client=panel&discordId=1&name=x`, { redirect: 'manual' });
    must(r.status === 404, `HTTP ${r.status}`);
  });
  const bad = results.filter((r) => !r[0]);
  log(bad.length ? `${bad.length} falha(s)` : `tudo ok (${results.length} verificações)`);
  if (bad.length) process.exit(1);
}

// ---------------------------------------------------------------------------------------------------------- main

const o = parseArgs(process.argv.slice(2));
switch (o.cmd) {
  case 'build-releases': buildReleases(o); break;
  case 'deploy': deploy(o); break;
  case 'cert': ssh(o, `bash ${ROOT}/deploy/remote-install.sh cert`); break;
  case 'firewall': ssh(o, `bash ${ROOT}/deploy/remote-install.sh firewall`); break;
  case 'status': ssh(o, `bash ${ROOT}/deploy/remote-install.sh status`); break;
  case 'check': await check(); break;
  default: {
    const text = readFileSync(fileURLToPath(import.meta.url), 'utf8').split('\n');
    console.log(text.slice(1, text.findIndex((l) => l.startsWith('import '))).map((l) => l.replace(/^\/\/ ?/, '')).join('\n').trim());
  }
}
