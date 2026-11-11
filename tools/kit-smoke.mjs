#!/usr/bin/env node
// Conferência automática do ambiente de teste local (sem o launcher): com o `dev-env --run` rodando, consulta o Hub e
// o cb-server por HTTP como o launcher faria e confere as assinaturas com a chave pública embutida no launcher.
//
//   node tools/kit-smoke.mjs [--hub http://127.0.0.1:8787]
//
// Confere: Hub responde (/health); "Castelo Bruxo (teste)" listado, online e em destaque; update 0.1.0 → 0.1.1 com
// assinatura válida (minisign, version:0.1.1) e o instalador baixado bate com ela; 0.1.1 não recebe update; mod de
// teste (protocolo 1) com assinatura, sha256 e tamanho certos; manifest do cb-server com o hello-client e um arquivo
// baixado com o sha256 do manifest. Não lê nem precisa de chave privada. Sai com código 1 se algo falhar.
// No kit roda pelo CONFERIR.cmd; no repositório: node tools/kit-smoke.mjs (com node tools/dev-env.mjs --run aberto).

import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseModComment, tauriCommentFields, verifyMinisign } from './lib/minisign.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SERVER_NAME = 'Castelo Bruxo (teste)';

let hub = 'http://127.0.0.1:8787';
for (let i = 2; i < process.argv.length; i++) {
  if (process.argv[i] === '--hub' && process.argv[i + 1]) hub = process.argv[++i].replace(/\/+$/, '');
  else { console.error(`opção desconhecida: ${process.argv[i]}`); process.exit(2); }
}

// No kit a chave pública fica em keys\update.pub; no repositório, em launcher\src-tauri\keys.
const pubPath = [join(ROOT, 'keys', 'update.pub'), join(ROOT, 'launcher', 'src-tauri', 'keys', 'update.pub')].find((p) => existsSync(p));
if (!pubPath) { console.error('keys/update.pub não encontrada'); process.exit(2); }
const PUB = readFileSync(pubPath, 'utf8');

const results = [];
async function check(name, fn) {
  try {
    const detail = await fn();
    results.push({ ok: true, name, detail });
    console.log(`[OK    ] ${name}${detail ? ` — ${detail}` : ''}`);
  } catch (e) {
    results.push({ ok: false, name, detail: e.message });
    console.log(`[FALHOU] ${name} — ${e.message}`);
  }
}

async function get(url, { json = true } = {}) {
  const res = await fetch(url, { signal: AbortSignal.timeout(30_000) });
  const buf = Buffer.from(await res.arrayBuffer());
  let body;
  if (json) { try { body = buf.length ? JSON.parse(buf.toString('utf8')) : undefined; } catch { body = undefined; } }
  return { status: res.status, body, buf };
}

const sha256 = (b) => createHash('sha256').update(b).digest('hex');
function expect(cond, msg) { if (!cond) throw new Error(msg); }
/** URL anunciada pelo Hub tem de ser do próprio Hub (o launcher recusa outra origem). */
function sameOrigin(u) { expect(new URL(u).origin === new URL(hub).origin, `URL fora do Hub: ${u}`); }

let server;
await check('Hub responde (/health)', async () => {
  const r = await get(`${hub}/health`);
  expect(r.status === 200, `HTTP ${r.status}`);
  return hub;
});
await check(`"${SERVER_NAME}" listado, online e em destaque`, async () => {
  const r = await get(`${hub}/v1/servers`);
  expect(r.status === 200 && Array.isArray(r.body), `HTTP ${r.status}`);
  const s = r.body.find((x) => x.name === SERVER_NAME);
  expect(s, `servidor não está na lista (${r.body.length} servidores)`);
  expect(s.featured === true, 'não está em destaque');
  const d = await get(`${hub}/v1/servers/${encodeURIComponent(s.id)}`);
  expect(d.status === 200 && d.body?.online === true, `detalhe: HTTP ${d.status}, online=${d.body?.online}`);
  server = d.body;
  return `${s.id}, ${server.host}:${server.gamePort} (http ${server.httpPort}), protocolo ${server.protocol}, ${s.players ?? 0}/${s.maxPlayers ?? '?'}`;
});
await check('update 0.1.0 → 0.1.1 assinado (e o instalador confere)', async () => {
  const r = await get(`${hub}/v1/launcher/update/windows/x86_64/0.1.0`);
  expect(r.status === 200, `HTTP ${r.status}`);
  expect(r.body.version === '0.1.1', `versão oferecida ${r.body.version}`);
  sameOrigin(r.body.url);
  const exe = await get(r.body.url, { json: false });
  expect(exe.status === 200, `download do instalador: HTTP ${exe.status}`);
  const comment = verifyMinisign(exe.buf, r.body.signature, PUB);
  const f = tauriCommentFields(comment);
  expect(f.version === '0.1.1', `versão assinada ${f.version ?? '(nenhuma)'}`);
  expect(decodeURIComponent(r.body.url).endsWith(`/${f.file}`), `arquivo assinado ${f.file} ≠ URL`);
  return `${f.file}, ${(exe.buf.length / 1048576).toFixed(1)} MiB, assinatura ok (version:${f.version})`;
});
await check('0.1.1 já é a mais nova (sem update)', async () => {
  const r = await get(`${hub}/v1/launcher/update/windows/x86_64/0.1.1`);
  expect(r.status === 204, `HTTP ${r.status}`);
});
await check('mod de teste (protocolo 1) assinado', async () => {
  const r = await get(`${hub}/v1/mod/latest?protocol=1`);
  expect(r.status === 200, `HTTP ${r.status}`);
  sameOrigin(r.body.url);
  const z = await get(r.body.url, { json: false });
  expect(z.status === 200, `download do mod: HTTP ${z.status}`);
  expect(z.buf.length === r.body.size, `tamanho ${z.buf.length} ≠ anunciado ${r.body.size}`);
  const sha = sha256(z.buf);
  expect(sha === r.body.sha256, 'sha256 diferente do anunciado');
  const signed = parseModComment(verifyMinisign(z.buf, r.body.signature, PUB));
  expect(signed, 'comentário confiável fora do formato cbmp-mod');
  expect(signed.protocol === 1 && signed.version === r.body.version && signed.sha256 === sha, 'comentário assinado não bate com o anúncio');
  return `${r.body.version}, ${z.buf.length} bytes, assinatura ok`;
});
await check('cb-server serve o manifest com o hello-client', async () => {
  expect(server, 'sem detalhe do servidor (falhou acima)');
  const base = `http://${server.host.includes(':') ? `[${server.host}]` : server.host}:${server.httpPort}`;
  const m = await get(`${base}/cb/manifest`);
  expect(m.status === 200, `manifest: HTTP ${m.status}`);
  expect(m.body.serverId === server.id, `serverId do manifest ${m.body.serverId}`);
  const res = (m.body.resources ?? []).find((x) => x.name === 'hello-client');
  expect(res, `hello-client ausente (resources: ${(m.body.resources ?? []).map((x) => x.name).join(', ')})`);
  const f = res.files.find((x) => x.path.endsWith('.lua')) ?? res.files[0];
  const dl = await get(`${base}/cb/files/${f.sha256}`, { json: false });
  expect(dl.status === 200 && sha256(dl.buf) === f.sha256, `arquivo ${f.path}: HTTP ${dl.status} ou sha256 diferente`);
  return `${res.files.length} arquivos (${res.files.map((x) => x.path).join(', ')}); ${f.path} baixado e conferido`;
});

const bad = results.filter((r) => !r.ok).length;
console.log(bad ? `\n${bad} de ${results.length} conferências FALHARAM.` : `\nTudo certo: ${results.length} conferências OK.`);
process.exit(bad ? 1 : 0);
