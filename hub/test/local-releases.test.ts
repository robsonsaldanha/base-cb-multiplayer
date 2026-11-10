import { mkdirSync, mkdtempSync, renameSync, rmSync, symlinkSync, utimesSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { loadConfig } from '../src/config';
import { createLocalReleases } from '../src/local-releases';
import { makeTestApp } from './helpers';

const SIG = 'dW50cnVzdGVkIGNvbW1lbnQ6IHNpZ25hdHVyZQo=';
const SHA = 'a'.repeat(64);

let root: string;
let dir: string;
let outside: string;

function put(rel: string, body: string | Buffer) {
  const p = join(dir, rel);
  mkdirSync(join(p, '..'), { recursive: true });
  writeFileSync(p, body);
}

async function readAll(stream: ReadableStream<Uint8Array>): Promise<Buffer> {
  const chunks: Buffer[] = [];
  for await (const c of stream as unknown as AsyncIterable<Uint8Array>) chunks.push(Buffer.from(c));
  return Buffer.concat(chunks);
}

/** Junção (Windows, não exige privilégio: sempre testado lá) ou link simbólico de pasta; `false` se não der. */
function linkDir(target: string, at: string): boolean {
  try { symlinkSync(target, at, 'junction'); return true; } catch (e) {
    if (process.platform === 'win32') throw e;
    return false;
  }
}

beforeEach(() => {
  root = mkdtempSync(join(tmpdir(), 'cbmp-rel-'));
  dir = join(root, 'releases');
  outside = join(root, 'fora');
  mkdirSync(dir);
  mkdirSync(outside);
  writeFileSync(join(outside, 'segredo.txt'), 'não pode sair');
  writeFileSync(join(outside, 'CB Multiplayer_0.9.0_x64-setup.exe'), 'fora');
  writeFileSync(join(outside, 'CB Multiplayer_0.9.0_x64-setup.exe.sig'), SIG);
});
afterEach(() => rmSync(root, { recursive: true, force: true }));

describe('HUB_RELEASES_DIR (config)', () => {
  const dev = { HUB_DEV_LOGIN: '1' };
  it('lê a pasta como caminho absoluto', () => {
    expect(loadConfig({ ...dev }).releasesDir).toBe('');
    expect(loadConfig({ ...dev, HUB_RELEASES_DIR: dir }).releasesDir).toBe(dir);
    expect(loadConfig({ ...dev, HUB_RELEASES_DIR: 'rel' }).releasesDir).toBe(join(process.cwd(), 'rel'));
  });
  it('é mutuamente exclusivo com HUB_GITHUB_TOKEN', () => {
    expect(() => loadConfig({ ...dev, HUB_RELEASES_DIR: dir, HUB_GITHUB_TOKEN: 't', HUB_RELEASES_REPO: 'a/b' }))
      .toThrow(/HUB_RELEASES_DIR.*HUB_GITHUB_TOKEN/);
  });
  it('é recusado com NODE_ENV=production', () => {
    expect(() => loadConfig({ ...dev, HUB_RELEASES_DIR: dir, NODE_ENV: 'production' })).toThrow(/HUB_RELEASES_DIR.*production/);
    expect(loadConfig({ ...dev, NODE_ENV: 'production' }).releasesDir).toBe('');
  });
});

describe('createLocalReleases', () => {
  it('recusa pasta inexistente', () => {
    expect(() => createLocalReleases({ dir: join(root, 'nada') })).toThrow(/HUB_RELEASES_DIR/);
  });

  it('lista <pasta>/<tag>/<asset> com textos de .sig/.sha256, notas e data', async () => {
    put('launcher-v0.1.1/CB Multiplayer_0.1.1_x64-setup.exe', 'instalador');
    put('launcher-v0.1.1/CB Multiplayer_0.1.1_x64-setup.exe.sig', `${SIG}\n`);
    put('launcher-v0.1.1/notes.txt', 'Versão de teste');
    put('mod-p1-v0.1.0/cbmp-mod-p1-v0.1.0.zip', 'zip');
    put('mod-p1-v0.1.0/cbmp-mod-p1-v0.1.0.zip.sha256', `${SHA}  cbmp-mod-p1-v0.1.0.zip\n`);
    const when = new Date('2026-11-10T12:00:00Z');
    utimesSync(join(dir, 'launcher-v0.1.1'), when, when);
    const list = await createLocalReleases({ dir }).list();
    const l = list.find((r) => r.tag === 'launcher-v0.1.1')!;
    expect(l).toMatchObject({ draft: false, prerelease: false, body: 'Versão de teste', publishedAt: when.toISOString() });
    expect(l.assets.map((a) => a.name).sort()).toEqual(['CB Multiplayer_0.1.1_x64-setup.exe', 'CB Multiplayer_0.1.1_x64-setup.exe.sig']);
    expect(l.assets.find((a) => a.name.endsWith('.sig'))!.text).toBe(`${SIG}\n`);
    const exe = l.assets.find((a) => a.name.endsWith('.exe'))!;
    expect(exe.size).toBe(10);
    expect(exe.text).toBeUndefined();
    const m = list.find((r) => r.tag === 'mod-p1-v0.1.0')!;
    expect(m.assets.find((a) => a.name.endsWith('.sha256'))!.text).toContain(SHA);
  });

  it('download devolve os bytes e o tamanho; id muda quando o arquivo é trocado', async () => {
    put('launcher-v0.1.1/a-setup.exe', 'conteudo-1');
    const rel = createLocalReleases({ dir });
    const id1 = (await rel.list())[0]!.assets[0]!.id;
    const s = await rel.download(id1);
    expect(s.size).toBe(10);
    expect((await readAll(s)).toString()).toBe('conteudo-1');
    put('launcher-v0.1.1/a-setup.exe', 'conteudo-2-maior');
    const id2 = (await rel.list())[0]!.assets[0]!.id;
    expect(id2).not.toBe(id1);
    await expect(rel.download(id1)).rejects.toMatchObject({ status: 404 });
    expect((await readAll(await rel.download(id2))).toString()).toBe('conteudo-2-maior');
  });

  it('ignora pastas fora do padrão de tag, arquivos soltos, subpastas e nomes estranhos', async () => {
    put('launcher-v0.1.1/ok-setup.exe', 'x');
    put('launcher-v0.1.1/sub/dentro-setup.exe', 'x');
    put('launcher-v0.1.1/.oculto', 'x');
    put('launcher-v0.1.1/com espaço e acentuação.exe', 'x');
    put('launcher-v0.1.1/nome;estranho.exe', 'x');
    put('outra-coisa/x.exe', 'x');
    put('launcher-v01.1.1/x.exe', 'x');
    put('solto.exe', 'x');
    const list = await createLocalReleases({ dir }).list();
    expect(list.map((r) => r.tag)).toEqual(['launcher-v0.1.1']);
    expect(list[0]!.assets.map((a) => a.name).sort()).toEqual(['ok-setup.exe']);
  });

  it('não segue junção/link de pasta de tag para fora da pasta de releases', async () => {
    if (!linkDir(outside, join(dir, 'launcher-v0.9.0'))) return;
    put('launcher-v0.1.1/ok-setup.exe', 'x');
    const list = await createLocalReleases({ dir }).list();
    expect(list.map((r) => r.tag)).toEqual(['launcher-v0.1.1']);
  });

  it('não lista asset que é link simbólico para fora', async () => {
    put('launcher-v0.1.1/ok-setup.exe', 'x');
    try { symlinkSync(join(outside, 'segredo.txt'), join(dir, 'launcher-v0.1.1', 'segredo-setup.exe'), 'file'); } catch { return; }
    const list = await createLocalReleases({ dir }).list();
    expect(list[0]!.assets.map((a) => a.name)).toEqual(['ok-setup.exe']);
  });

  it('download recusa se a pasta da tag virou junção para fora depois da listagem', async () => {
    put('launcher-v0.9.0/CB Multiplayer_0.9.0_x64-setup.exe', 'dentro');
    const rel = createLocalReleases({ dir });
    const id = (await rel.list())[0]!.assets[0]!.id;
    renameSync(join(dir, 'launcher-v0.9.0'), join(root, 'movida'));
    if (!linkDir(outside, join(dir, 'launcher-v0.9.0'))) return;
    await expect(rel.download(id)).rejects.toMatchObject({ status: 404 });
  });

  it('id desconhecido → 404', async () => {
    await expect(createLocalReleases({ dir }).download(12345)).rejects.toMatchObject({ status: 404, code: 'not_found' });
  });
});

describe('rotas com HUB_RELEASES_DIR', () => {
  async function app() {
    const t = await makeTestApp({ config: { releasesDir: dir }, releases: createLocalReleases({ dir }) });
    return t.app;
  }
  beforeEach(() => {
    put('launcher-v0.1.0/CB Multiplayer (Teste local)_0.1.0_x64-setup.exe', 'v010');
    put('launcher-v0.1.0/CB Multiplayer (Teste local)_0.1.0_x64-setup.exe.sig', 'c2lnMDEw');
    put('launcher-v0.1.1/CB Multiplayer (Teste local)_0.1.1_x64-setup.exe', 'v011-bytes');
    put('launcher-v0.1.1/CB Multiplayer (Teste local)_0.1.1_x64-setup.exe.sig', 'c2lnMDEx\n');
    put('launcher-v0.1.1/notes.txt', 'Teste 0.1.1');
    put('mod-p1-v0.1.0/cbmp-mod-p1-v0.1.0.zip', 'zipzip');
    put('mod-p1-v0.1.0/cbmp-mod-p1-v0.1.0.zip.sig', 'bW9kc2ln');
    put('mod-p1-v0.1.0/cbmp-mod-p1-v0.1.0.zip.sha256', `${SHA}\n`);
  });

  it('update 0.1.0 → 0.1.1, mod/latest e dl funcionam sem token do GitHub', async () => {
    const a = await app();
    const up = await a.inject({ url: '/v1/launcher/update/windows/x86_64/0.1.0' });
    expect(up.statusCode).toBe(200);
    const body = up.json();
    expect(body).toMatchObject({ version: '0.1.1', notes: 'Teste 0.1.1', signature: 'c2lnMDEx' });
    expect(body.url).toBe('http://localhost:8787/v1/dl/launcher-v0.1.1/CB%20Multiplayer%20(Teste%20local)_0.1.1_x64-setup.exe');
    expect((await a.inject({ url: '/v1/launcher/update/windows/x86_64/0.1.1' })).statusCode).toBe(204);

    const mod = await a.inject({ url: '/v1/mod/latest?protocol=1' });
    expect(mod.statusCode).toBe(200);
    expect(mod.json()).toMatchObject({ version: '0.1.0', protocol: 1, sha256: SHA, size: 6, signature: 'bW9kc2ln' });

    const dl = await a.inject({ url: new URL(body.url).pathname });
    expect(dl.statusCode).toBe(200);
    expect(dl.headers['content-length']).toBe('10');
    expect(dl.body).toBe('v011-bytes');
    await a.close();
  });

  it('/v1/dl não sai da pasta (tag ou asset com ../ ou \\)', async () => {
    const a = await app();
    for (const url of [
      '/v1/dl/..%2Ffora/segredo.txt',
      '/v1/dl/launcher-v0.1.1/..%2F..%2Ffora%2Fsegredo.txt',
      '/v1/dl/launcher-v0.1.1/..%5C..%5Cfora%5Csegredo.txt',
      '/v1/dl/launcher-v0.1.1/notes.txt',
      '/v1/dl/launcher-v0.1.1/.',
      '/v1/dl/launcher-v0.1.1/..',
    ]) {
      const r = await a.inject({ url });
      expect(r.statusCode, url).toBe(404);
    }
    await a.close();
  });

  it('sem releasesDir nem token → 503 updates_disabled', async () => {
    const t = await makeTestApp({ releases: createLocalReleases({ dir }) });
    const r = await t.app.inject({ url: '/v1/mod/latest?protocol=1' });
    expect(r.statusCode).toBe(503);
    expect(r.json().error).toBe('updates_disabled');
    await t.app.close();
  });
});
