import { PassThrough } from 'node:stream';
import { describe, expect, it } from 'vitest';
import { ModInfo, UpdateInfo } from '@cbmp/contracts';
import { makeTestApp } from './helpers';
import { createGitHubReleases, type AssetStream, type GitHubReleases, type Release, type ReleaseAsset } from '../src/github';
import { compareSemver, parseSemver } from '../src/routes/releases';
import { HttpError } from '../src/errors';
import { loadConfig } from '../src/config';

const TOKEN = 'ghp_TESTTOKEN_nao_vazar';
const PUB = '2026-11-10T12:00:00Z';

let nextId = 1;
const asset = (name: string, size = 10, text?: string): ReleaseAsset => ({ id: nextId++, name, size, ...(text !== undefined ? { text } : {}) });
const rel = (tag: string, assets: ReleaseAsset[], over: Partial<Release> = {}): Release =>
  ({ tag, draft: false, prerelease: false, publishedAt: PUB, body: `notas ${tag}`, assets, ...over });

const setupExe = (v: string) => `CB Multiplayer_${v}_x64-setup.exe`;
function launcher(v: string, opts: { sig?: boolean } = {}, over: Partial<Release> = {}): Release {
  const assets = [asset(setupExe(v), 1000)];
  if (opts.sig !== false) assets.push(asset(`${setupExe(v)}.sig`, 120, `SIG-${v}`));
  return rel(`launcher-v${v}`, assets, over);
}
const SHA = 'a'.repeat(64);
function mod(p: number, v: string, opts: { sig?: boolean; sha?: string | null } = {}, over: Partial<Release> = {}): Release {
  const zip = `cbmp-mod-p${p}-v${v}.zip`;
  const assets = [asset(zip, 5000)];
  if (opts.sig !== false) assets.push(asset(`${zip}.sig`, 120, `MODSIG-${p}-${v}`));
  if (opts.sha !== null) assets.push(asset(`${zip}.sha256`, 90, opts.sha ?? `${SHA}  ${zip}\n`));
  return rel(`mod-p${p}-v${v}`, assets, over);
}

class FakeReleases implements GitHubReleases {
  releases: Release[] = [];
  blobs = new Map<number, Uint8Array<ArrayBuffer>>();
  downloads: number[] = [];
  /** Quando definido, o corpo só é entregue depois que a promessa resolve (downloads "em andamento"). */
  hold: Promise<void> | null = null;
  async list(): Promise<Release[]> { return this.releases; }
  async download(assetId: number): Promise<AssetStream> {
    this.downloads.push(assetId);
    const b = this.blobs.get(assetId);
    if (!b) throw new HttpError(502, 'github_error', 'falha');
    const hold = this.hold;
    if (!hold) return Object.assign(new Blob([b]).stream(), { size: b.length });
    const body = new ReadableStream<Uint8Array>({
      async pull(c) { await hold; c.enqueue(b); c.close(); },
    });
    return Object.assign(body, { size: b.length });
  }
}

async function setup(opts: { token?: string; rateLimit?: boolean } = {}) {
  const fake = new FakeReleases();
  const t = await makeTestApp({
    config: { githubToken: opts.token ?? TOKEN, releasesRepo: 'dono/repo', rateLimit: opts.rateLimit ?? false },
    releases: fake,
  });
  const get = (url: string, ip = '203.0.113.9') => t.app.inject({ method: 'GET', url, remoteAddress: ip });
  return { ...t, fake, get };
}

describe('semver local', () => {
  it('parse e comparação', () => {
    expect(parseSemver('1.2.3')).not.toBeNull();
    expect(parseSemver('01.2.3')).toBeNull();
    expect(parseSemver('1.2')).toBeNull();
    expect(parseSemver('v1.2.3')).toBeNull();
    expect(compareSemver('0.10.0', '0.9.9')).toBeGreaterThan(0);
    expect(compareSemver('1.0.0', '1.0.0')).toBe(0);
    expect(compareSemver('1.0.0-beta.1', '1.0.0')).toBeLessThan(0);
    expect(compareSemver('1.0.0-beta.2', '1.0.0-beta.10')).toBeLessThan(0);
    expect(compareSemver('1.0.0+abc', '1.0.0')).toBe(0);
  });
});

describe('sem HUB_GITHUB_TOKEN', () => {
  it('as três rotas respondem 503 updates_disabled', async () => {
    const t = await setup({ token: '' });
    t.fake.releases = [launcher('0.2.0'), mod(1, '0.1.0')];
    for (const url of ['/v1/launcher/update/windows/x86_64/0.1.0', '/v1/mod/latest?protocol=1', '/v1/dl/launcher-v0.2.0/x.exe']) {
      const r = await t.get(url);
      expect(r.statusCode, url).toBe(503);
      expect(r.json().error).toBe('updates_disabled');
    }
  });
});

describe('GET /v1/launcher/update/{target}/{arch}/{current_version}', () => {
  it('versão atual menor → 200 com a maior launcher-v* (semver), url no Hub e conteúdo do .sig', async () => {
    const t = await setup();
    t.fake.releases = [launcher('0.9.0'), launcher('0.10.0'), launcher('0.2.0'), mod(1, '9.9.9')];
    const r = await t.get('/v1/launcher/update/windows/x86_64/0.1.0');
    expect(r.statusCode).toBe(200);
    const body = UpdateInfo.parse(r.json());
    expect(body).toEqual({
      version: '0.10.0',
      notes: 'notas launcher-v0.10.0',
      pub_date: PUB,
      url: `http://localhost:8787/v1/dl/launcher-v0.10.0/${encodeURIComponent(setupExe('0.10.0'))}`,
      signature: 'SIG-0.10.0',
    });
  });

  it('versão igual ou maior → 204', async () => {
    const t = await setup();
    t.fake.releases = [launcher('0.2.0')];
    expect((await t.get('/v1/launcher/update/windows/x86_64/0.2.0')).statusCode).toBe(204);
    expect((await t.get('/v1/launcher/update/windows/x86_64/0.3.0')).statusCode).toBe(204);
  });

  it('sem releases → 204', async () => {
    const t = await setup();
    expect((await t.get('/v1/launcher/update/windows/x86_64/0.1.0')).statusCode).toBe(204);
  });

  it('target ≠ windows ou arch ≠ x86_64 → 204', async () => {
    const t = await setup();
    t.fake.releases = [launcher('0.2.0')];
    expect((await t.get('/v1/launcher/update/linux/x86_64/0.1.0')).statusCode).toBe(204);
    expect((await t.get('/v1/launcher/update/windows/aarch64/0.1.0')).statusCode).toBe(204);
    expect((await t.get('/v1/launcher/update/darwin/aarch64/0.1.0')).statusCode).toBe(204);
  });

  it('versão inválida → 400', async () => {
    const t = await setup();
    t.fake.releases = [launcher('0.2.0')];
    for (const v of ['abc', '1.2', '1.2.3.4', 'v1.2.3', '01.2.3']) {
      const r = await t.get(`/v1/launcher/update/windows/x86_64/${v}`);
      expect(r.statusCode, v).toBe(400);
      expect(r.json().error).toBe('invalid_version');
    }
  });

  it('release sem .sig é ignorado (cai para o próximo maior)', async () => {
    const t = await setup();
    t.fake.releases = [launcher('0.3.0', { sig: false }), launcher('0.2.0')];
    const r = await t.get('/v1/launcher/update/windows/x86_64/0.1.0');
    expect(r.statusCode).toBe(200);
    expect(r.json().version).toBe('0.2.0');
  });

  it('draft, prerelease e tags fora do padrão são ignorados', async () => {
    const t = await setup();
    t.fake.releases = [
      launcher('0.5.0', {}, { draft: true }),
      launcher('0.4.0', {}, { prerelease: true }),
      launcher('0.3.0', {}, { tag: 'launcher-0.3.0' }),
      launcher('0.3.1', {}, { tag: 'v0.3.1' }),
      launcher('0.2.0'),
    ];
    expect((await t.get('/v1/launcher/update/windows/x86_64/0.1.0')).json().version).toBe('0.2.0');
  });
});

describe('GET /v1/mod/latest?protocol=N', () => {
  it('maior mod-pN-v* do protocolo pedido, sha256 lido do asset .sha256', async () => {
    const t = await setup();
    t.fake.releases = [mod(1, '0.2.0'), mod(1, '0.10.0'), mod(2, '5.0.0'), launcher('9.0.0')];
    const r = await t.get('/v1/mod/latest?protocol=1');
    expect(r.statusCode).toBe(200);
    expect(ModInfo.parse(r.json())).toEqual({
      version: '0.10.0',
      protocol: 1,
      url: 'http://localhost:8787/v1/dl/mod-p1-v0.10.0/cbmp-mod-p1-v0.10.0.zip',
      sha256: SHA,
      size: 5000,
      signature: 'MODSIG-1-0.10.0',
    });
    expect((await t.get('/v1/mod/latest?protocol=2')).json().version).toBe('5.0.0');
  });

  it('sha256 em maiúsculas é normalizado', async () => {
    const t = await setup();
    t.fake.releases = [mod(1, '0.1.0', { sha: 'AB'.repeat(32) })];
    expect((await t.get('/v1/mod/latest?protocol=1')).json().sha256).toBe('ab'.repeat(32));
  });

  it('sem .sha256, sem .sig, sha256 malformado ou protocolo sem release → 404 no_mod', async () => {
    const t = await setup();
    t.fake.releases = [mod(1, '0.1.0', { sha: null }), mod(2, '0.1.0', { sig: false }), mod(4, '0.1.0', { sha: 'xyz' })];
    for (const p of [1, 2, 3, 4]) {
      const r = await t.get(`/v1/mod/latest?protocol=${p}`);
      expect(r.statusCode, String(p)).toBe(404);
      expect(r.json().error).toBe('no_mod');
    }
  });

  it('release inválido é pulado em favor do próximo', async () => {
    const t = await setup();
    t.fake.releases = [mod(1, '0.3.0', { sha: null }), mod(1, '0.2.0')];
    expect((await t.get('/v1/mod/latest?protocol=1')).json().version).toBe('0.2.0');
  });

  it('protocol ausente ou inválido → 400', async () => {
    const t = await setup();
    for (const q of ['', '?protocol=', '?protocol=abc', '?protocol=-1', '?protocol=1.5']) {
      const r = await t.get(`/v1/mod/latest${q}`);
      expect(r.statusCode, q).toBe(400);
      expect(r.json().error).toBe('invalid_query');
    }
  });
});

describe('GET /v1/dl/{tag}/{asset}', () => {
  it('stream do asset com Content-Length e Content-Type application/octet-stream', async () => {
    const t = await setup();
    const r1 = launcher('0.2.0');
    t.fake.releases = [r1];
    const exe = r1.assets[0]!;
    const bytes = new Uint8Array(1000).map((_, i) => i % 251);
    t.fake.blobs.set(exe.id, bytes);
    const r = await t.get(`/v1/dl/launcher-v0.2.0/${encodeURIComponent(exe.name)}`);
    expect(r.statusCode).toBe(200);
    expect(r.headers['content-type']).toBe('application/octet-stream');
    expect(r.headers['content-length']).toBe('1000');
    expect(Buffer.compare(r.rawPayload, Buffer.from(bytes))).toBe(0);
    expect(t.fake.downloads).toEqual([exe.id]);
  });

  it('assets pequenos (.sig) também são servidos', async () => {
    const t = await setup();
    const m = mod(1, '0.1.0');
    t.fake.releases = [m];
    const sig = m.assets[1]!;
    t.fake.blobs.set(sig.id, new TextEncoder().encode('MODSIG'));
    const r = await t.get(`/v1/dl/mod-p1-v0.1.0/${sig.name}`);
    expect(r.statusCode).toBe(200);
    expect(r.body).toBe('MODSIG');
  });

  it('tag fora dos padrões → 404 (sem consultar download)', async () => {
    const t = await setup();
    t.fake.releases = [rel('v1.0.0', [asset('x.exe')]), rel('launcher-v1.0', [asset('y.exe')])];
    for (const url of ['/v1/dl/v1.0.0/x.exe', '/v1/dl/launcher-v1.0/y.exe', '/v1/dl/mod-p1-v1/x.zip', '/v1/dl/..%2F..%2Fetc/passwd']) {
      const r = await t.get(url);
      expect(r.statusCode, url).toBe(404);
    }
    expect(t.fake.downloads).toEqual([]);
  });

  it('tag válida inexistente, asset não listado, draft ou prerelease → 404', async () => {
    const t = await setup();
    t.fake.releases = [launcher('0.2.0'), launcher('0.3.0', {}, { draft: true }), launcher('0.4.0', {}, { prerelease: true })];
    for (const url of [
      '/v1/dl/launcher-v0.9.0/x.exe',
      '/v1/dl/launcher-v0.2.0/outro.exe',
      `/v1/dl/launcher-v0.3.0/${encodeURIComponent(setupExe('0.3.0'))}`,
      `/v1/dl/launcher-v0.4.0/${encodeURIComponent(setupExe('0.4.0'))}`,
    ]) {
      const r = await t.get(url);
      expect(r.statusCode, url).toBe(404);
      expect(r.json().error).toBe('not_found');
    }
    expect(t.fake.downloads).toEqual([]);
  });
});

describe('rate limit por IP', () => {
  it('61ª requisição no minuto → 429; outro IP segue livre', async () => {
    const t = await setup({ rateLimit: true });
    t.fake.releases = [launcher('0.2.0')];
    const codes: number[] = [];
    for (let i = 0; i < 61; i++) codes.push((await t.get('/v1/launcher/update/windows/x86_64/0.1.0', '198.51.100.1')).statusCode);
    expect(codes.slice(0, 60).every((c) => c === 200)).toBe(true);
    expect(codes[60]).toBe(429);
    expect((await t.get('/v1/mod/latest?protocol=1', '198.51.100.2')).statusCode).toBe(404);
  });
});

describe('config', () => {
  const base = { DISCORD_CLIENT_ID: 'cid', DISCORD_CLIENT_SECRET: 'secret' };
  it('token e repo opcionais (vazios = updates desligados)', () => {
    const c = loadConfig({ ...base });
    expect(c.githubToken).toBe('');
    expect(c.releasesRepo).toBe('');
  });
  it('token exige HUB_RELEASES_REPO no formato dono/repo', () => {
    expect(loadConfig({ ...base, HUB_GITHUB_TOKEN: TOKEN, HUB_RELEASES_REPO: 'dono/repo' }).releasesRepo).toBe('dono/repo');
    expect(() => loadConfig({ ...base, HUB_GITHUB_TOKEN: TOKEN })).toThrow(/HUB_RELEASES_REPO/);
    expect(() => loadConfig({ ...base, HUB_GITHUB_TOKEN: TOKEN, HUB_RELEASES_REPO: 'semdono' })).toThrow(/HUB_RELEASES_REPO/);
    expect(() => loadConfig({ ...base, HUB_GITHUB_TOKEN: TOKEN, HUB_RELEASES_REPO: 'a/b/c' })).toThrow(/HUB_RELEASES_REPO/);
  });
  it('repo com . ou .. é recusado e a mensagem nunca contém o token', () => {
    for (const repo of ['../x', 'x/..', './x']) {
      let msg = '';
      try { loadConfig({ ...base, HUB_GITHUB_TOKEN: TOKEN, HUB_RELEASES_REPO: repo }); } catch (e) { msg = (e as Error).message; }
      expect(msg, repo).toMatch(/HUB_RELEASES_REPO/);
      expect(msg).not.toContain(TOKEN);
    }
  });
});

/** Implementação real com fetch falso (sem rede). */
describe('createGitHubReleases (fetch falso)', () => {
  type Call = { url: string; init: RequestInit };
  const ghRelease = (tag: string, assets: Array<{ id: number; name: string; size: number }>, over: Record<string, unknown> = {}) =>
    ({ tag_name: tag, draft: false, prerelease: false, published_at: PUB, created_at: PUB, body: 'n', assets, ...over });

  function fakeFetch(routes: (url: string, init: RequestInit) => Response) {
    const calls: Call[] = [];
    const impl = (async (input: string | URL | Request, init: RequestInit = {}) => {
      const url = String(input);
      calls.push({ url, init });
      return routes(url, init);
    }) as typeof fetch;
    return { calls, impl };
  }
  const header = (init: RequestInit, name: string) => new Headers(init.headers).get(name);

  it('lista com Bearer, lê .sig/.sha256, ignora os demais e guarda em cache por 60 s', async () => {
    const clock = { ms: 1_000_000 };
    const { calls, impl } = fakeFetch((url) => {
      if (url.endsWith('/releases?per_page=100')) {
        return Response.json([
          ghRelease('launcher-v0.2.0', [{ id: 1, name: 'a.exe', size: 9 }, { id: 2, name: 'a.exe.sig', size: 4 }]),
          ghRelease('mod-p1-v0.1.0', [{ id: 3, name: 'm.zip', size: 9 }, { id: 4, name: 'm.zip.sha256', size: 64 }]),
          ghRelease('outra-coisa', [{ id: 5, name: 'z.sig', size: 4 }]),
          ghRelease('launcher-v0.3.0', [{ id: 6, name: 'b.exe.sig', size: 4 }], { draft: true }),
        ]);
      }
      if (url.endsWith('/releases/assets/2')) return new Response('SIG2');
      if (url.endsWith('/releases/assets/4')) return new Response(SHA);
      return new Response('nope', { status: 500 });
    });
    const gh = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: impl, nowMs: () => clock.ms });
    const list = await gh.list();
    expect(calls[0]!.url).toBe('https://api.github.com/repos/dono/repo/releases?per_page=100');
    for (const c of calls) expect(header(c.init, 'authorization')).toBe(`Bearer ${TOKEN}`);
    expect(calls.filter((c) => c.url.includes('/assets/')).map((c) => c.url.split('/').pop()).sort()).toEqual(['2', '4']);
    expect(list.find((r) => r.tag === 'launcher-v0.2.0')!.assets.find((a) => a.id === 2)!.text).toBe('SIG2');
    expect(list.find((r) => r.tag === 'mod-p1-v0.1.0')!.assets.find((a) => a.id === 4)!.text).toBe(SHA);
    expect(list.find((r) => r.tag === 'launcher-v0.3.0')!.draft).toBe(true);
    const n = calls.length;
    clock.ms += 59_000;
    await gh.list();
    expect(calls.length).toBe(n);
    clock.ms += 2_000;
    await gh.list();
    // nova lista; os textos dos assets (id imutável) vêm do cache
    expect(calls.length).toBe(n + 1);
  });

  it('download: octet-stream com Bearer na API; redirect seguido sem o token; size do Content-Length', async () => {
    const { calls, impl } = fakeFetch((url) => {
      if (url === 'https://api.github.com/repos/dono/repo/releases/assets/7') {
        return new Response(null, { status: 302, headers: { location: 'https://objects.example/blob?sig=xyz' } });
      }
      if (url.startsWith('https://objects.example/')) return new Response('0123456789', { headers: { 'content-length': '10' } });
      return new Response('nope', { status: 404 });
    });
    const gh = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: impl });
    const s = await gh.download(7);
    expect(s.size).toBe(10);
    expect(await new Response(s).text()).toBe('0123456789');
    expect(header(calls[0]!.init, 'authorization')).toBe(`Bearer ${TOKEN}`);
    expect(header(calls[0]!.init, 'accept')).toBe('application/octet-stream');
    expect(calls[0]!.init.redirect).toBe('manual');
    expect(header(calls[1]!.init, 'authorization')).toBeNull();
  });

  it('falha do GitHub na rota → 502 github_error; o log não contém o token', async () => {
    const lines: string[] = [];
    const logStream = new PassThrough();
    logStream.on('data', (b: Buffer) => lines.push(b.toString('utf8')));
    const { impl } = fakeFetch(() => new Response('bad', { status: 500 }));
    const releases = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: impl });
    const t = await makeTestApp({ config: { githubToken: TOKEN, releasesRepo: 'dono/repo', logLevel: 'info' }, releases, logStream });
    const r = await t.app.inject({ method: 'GET', url: '/v1/mod/latest?protocol=1' });
    expect(r.statusCode).toBe(502);
    expect(r.json().error).toBe('github_error');
    expect(r.body).not.toContain(TOKEN);
    expect(lines.join('')).toContain('github_error');
    expect(lines.join('')).not.toContain(TOKEN);
  });

  it('falha do GitHub vira 502 github_error sem o token na mensagem nem no detalhe', async () => {
    const { impl } = fakeFetch(() => new Response('bad', { status: 401 }));
    const gh = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: impl });
    const err = await gh.list().catch((e: unknown) => e);
    expect(err).toBeInstanceOf(HttpError);
    expect((err as HttpError).status).toBe(502);
    expect((err as HttpError).code).toBe('github_error');
    expect(JSON.stringify({ m: (err as HttpError).message, u: (err as HttpError).upstream })).not.toContain(TOKEN);
  });
});

// ---------------------------------------------------------------- fix round 1 (review I1–I3, M1–M6)

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
type FCall = { url: string; init: RequestInit };
const hdr = (init: RequestInit, name: string) => new Headers(init.headers).get(name);
function fetchFake(routes: (url: string, init: RequestInit) => Response | Promise<Response>) {
  const calls: FCall[] = [];
  const impl = (async (input: string | URL | Request, init: RequestInit = {}) => {
    const url = String(input);
    calls.push({ url, init });
    return routes(url, init);
  }) as typeof fetch;
  return { calls, impl, api: () => calls.filter((c) => c.url.startsWith('https://api.github.com/')) };
}
const gh = (tag: string, assets: Array<{ id: number; name: string; size: number }>, over: Record<string, unknown> = {}) =>
  ({ tag_name: tag, draft: false, prerelease: false, published_at: PUB, created_at: PUB, body: 'n', assets, ...over });
/** Corpo que entrega `n` bytes, um a cada `everyMs`; erra se o signal do fetch abortar (como o fetch do Node). */
function dribble(n: number, everyMs: number, signal?: AbortSignal | null, stallAfter = Infinity) {
  let i = 0;
  let timer: ReturnType<typeof setTimeout> | undefined;
  return new ReadableStream<Uint8Array>({
    start(c) { signal?.addEventListener('abort', () => { clearTimeout(timer); try { c.error(signal.reason); } catch { /* fechado */ } }); },
    pull(c) {
      if (i >= stallAfter) return new Promise<void>(() => {});
      return new Promise<void>((res) => {
        timer = setTimeout(() => { i++; try { c.enqueue(new Uint8Array([i & 0xff])); if (i >= n) c.close(); } catch { /* cancelado */ } res(); }, everyMs);
      });
    },
  });
}
const isAsset = (id: number) => (url: string) => url === `https://api.github.com/repos/dono/repo/releases/assets/${id}`;
const errOf = async (p: Promise<unknown>) => (await p.then(() => null, (e: unknown) => e)) as HttpError;

describe('I1: prazos do download', () => {
  it('corpo que dura mais que o prazo de cabeçalhos, mas segue enviando, chega inteiro', async () => {
    const f = fetchFake((url, init) => {
      if (isAsset(7)(url)) return new Response(null, { status: 302, headers: { location: 'https://objects.example/b' } });
      return new Response(dribble(12, 20, init.signal), { headers: { 'content-length': '12' } });
    });
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, headerTimeoutMs: 50, idleTimeoutMs: 200 });
    const s = await c.download(7);
    const bytes = new Uint8Array(await new Response(s).arrayBuffer());
    expect(bytes.length).toBe(12); // ~240 ms > 50 ms de prazo de cabeçalhos
  });

  it('corpo parado por mais que o prazo de ociosidade → erro', async () => {
    const f = fetchFake((url, init) => {
      if (isAsset(7)(url)) return new Response(null, { status: 302, headers: { location: 'https://objects.example/b' } });
      return new Response(dribble(12, 5, init.signal, 2), { headers: { 'content-length': '12' } });
    });
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, headerTimeoutMs: 50, idleTimeoutMs: 80 });
    const s = await c.download(7);
    const t0 = Date.now();
    const err = await new Response(s).arrayBuffer().catch((e: unknown) => e);
    expect(err).toBeInstanceOf(Error);
    expect(Date.now() - t0).toBeLessThan(1000);
  });

  it('cliente lento (sem ler) não dispara o prazo de ociosidade', async () => {
    const f = fetchFake((url, init) => {
      if (isAsset(7)(url)) return new Response(null, { status: 302, headers: { location: 'https://objects.example/b' } });
      return new Response(dribble(3, 1, init.signal), { headers: { 'content-length': '3' } });
    });
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, idleTimeoutMs: 30 });
    const s = await c.download(7);
    await sleep(120);
    expect(new Uint8Array(await new Response(s).arrayBuffer()).length).toBe(3);
  });

  it('cabeçalhos que não chegam no prazo → 502 github_error (TimeoutError)', async () => {
    const f = fetchFake((_url, init) => new Promise<Response>((_res, rej) => init.signal!.addEventListener('abort', () => rej(init.signal!.reason))));
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, headerTimeoutMs: 30 });
    const err = await errOf(c.download(7));
    expect(err.code).toBe('github_error');
    expect(err.upstream?.error).toBe('TimeoutError');
  });
});

describe('I2: cota do token', () => {
  it('rate limit por /64 em IPv6; outro /64 segue livre', async () => {
    const t = await setup({ rateLimit: true });
    t.fake.releases = [launcher('0.2.0')];
    const codes: number[] = [];
    for (let i = 0; i < 61; i++) codes.push((await t.get('/v1/launcher/update/windows/x86_64/0.1.0', `2001:db8:1:2::${(i + 1).toString(16)}`)).statusCode);
    expect(codes.slice(0, 60).every((c) => c === 200)).toBe(true);
    expect(codes[60]).toBe(429);
    expect((await t.get('/v1/launcher/update/windows/x86_64/0.1.0', '2001:db8:1:3::1')).statusCode).toBe(200);
  });

  it('falha da lista fica em cache por 15 s (sem nova chamada); depois tenta de novo', async () => {
    const clock = { ms: 1_000_000 };
    const f = fetchFake(() => new Response('x', { status: 500 }));
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, nowMs: () => clock.ms, logger: { warn: () => {} } });
    expect((await errOf(c.list())).code).toBe('github_error');
    clock.ms += 14_000;
    expect((await errOf(c.list())).code).toBe('github_error');
    expect(f.calls.length).toBe(1);
    clock.ms += 2_000;
    await errOf(c.list());
    expect(f.calls.length).toBe(2);
  });

  it('com lista anterior, a falha serve a lista antiga e espera 15 s para tentar de novo', async () => {
    const clock = { ms: 1_000_000 };
    let fail = false;
    const f = fetchFake(() => (fail ? new Response('x', { status: 500 }) : Response.json([gh('launcher-v0.2.0', [{ id: 1, name: 'a-setup.exe', size: 9 }])])));
    const warns: string[] = [];
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, nowMs: () => clock.ms, logger: { warn: (o, m) => warns.push(JSON.stringify(o) + m) } });
    const first = await c.list();
    fail = true;
    clock.ms += 61_000;
    expect(await c.list()).toEqual(first);
    clock.ms += 10_000;
    expect(await c.list()).toEqual(first);
    expect(f.calls.length).toBe(2);
    expect(warns.length).toBeGreaterThan(0);
    expect(warns.join('')).not.toContain(TOKEN);
  });

  it('URL assinada guardada por asset: 2 downloads custam 1 chamada à API; URL recusada (403) refaz uma vez', async () => {
    const clock = { ms: 1_000_000 };
    let storageStatus = 200;
    const f = fetchFake((url) => {
      if (isAsset(7)(url)) return new Response(null, { status: 302, headers: { location: 'https://objects.example/b?sig=1' } });
      if (url.startsWith('https://objects.example/')) {
        return storageStatus === 200 ? new Response('abc', { headers: { 'content-length': '3' } }) : new Response('expired', { status: 403 });
      }
      return new Response('x', { status: 404 });
    });
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, nowMs: () => clock.ms });
    expect(await new Response(await c.download(7)).text()).toBe('abc');
    expect(await new Response(await c.download(7)).text()).toBe('abc');
    expect(f.api().length).toBe(1);
    storageStatus = 403;
    expect((await errOf(c.download(7))).code).toBe('github_error');
    expect(f.api().length).toBe(2); // URL recusada → uma nova chamada à API (que de novo dá 403 no storage)
    storageStatus = 200;
    await new Response(await c.download(7)).text();
    expect(f.api().length).toBe(3); // a URL que falhou não ficou guardada
    clock.ms += 61_000; // URL guardada vence sozinha
    await new Response(await c.download(7)).text();
    expect(f.api().length).toBe(4);
  });

  it('teto global de chamadas à API por minuto → 503 busy; libera no minuto seguinte', async () => {
    const clock = { ms: 1_000_000 };
    const f = fetchFake(() => new Response('x', { status: 500 }));
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, nowMs: () => clock.ms, apiCallsPerMinute: 2 });
    for (const id of [1, 2]) expect((await errOf(c.download(id))).code).toBe('github_error');
    const err = await errOf(c.download(3));
    expect([err.status, err.code]).toEqual([503, 'busy']);
    expect(f.calls.length).toBe(2);
    clock.ms += 60_000;
    expect((await errOf(c.download(4))).code).toBe('github_error');
  });

  it('/v1/dl: no máximo 8 downloads simultâneos; o 9º → 503 busy; depois libera', async () => {
    const t = await setup();
    const r1 = launcher('0.2.0');
    t.fake.releases = [r1];
    const exe = r1.assets[0]!;
    t.fake.blobs.set(exe.id, new Uint8Array(1000));
    let open!: () => void;
    t.fake.hold = new Promise<void>((r) => { open = r; });
    const url = `/v1/dl/launcher-v0.2.0/${encodeURIComponent(exe.name)}`;
    const running = Array.from({ length: 8 }, () => t.get(url));
    for (let i = 0; i < 200 && t.fake.downloads.length < 8; i++) await sleep(5);
    expect(t.fake.downloads.length).toBe(8);
    const ninth = await t.get(url);
    expect(ninth.statusCode).toBe(503);
    expect(ninth.json().error).toBe('busy');
    expect(t.fake.downloads.length).toBe(8);
    open();
    for (const r of await Promise.all(running)) expect(r.statusCode).toBe(200);
    t.fake.hold = null;
    expect((await t.get(url)).statusCode).toBe(200);
  });

  it('/v1/dl: falha no download libera a vaga', async () => {
    const t = await setup();
    const r1 = launcher('0.2.0');
    t.fake.releases = [r1];
    const url = `/v1/dl/launcher-v0.2.0/${encodeURIComponent(r1.assets[0]!.name)}`;
    for (let i = 0; i < 10; i++) expect((await t.get(url)).statusCode).toBe(502); // sem blob → 502, nunca busy
  });
});

describe('I3: falha ao ler .sig/.sha256', () => {
  const twoMods = (sigStatus: () => number) => fetchFake((url) => {
    if (url.includes('/releases?')) {
      return Response.json([
        gh('mod-p1-v0.2.0', [{ id: 21, name: 'cbmp-mod-p1-v0.2.0.zip', size: 9 }, { id: 22, name: 'cbmp-mod-p1-v0.2.0.zip.sig', size: 4 }, { id: 23, name: 'cbmp-mod-p1-v0.2.0.zip.sha256', size: 64 }]),
        gh('mod-p1-v0.1.0', [{ id: 11, name: 'cbmp-mod-p1-v0.1.0.zip', size: 9 }, { id: 12, name: 'cbmp-mod-p1-v0.1.0.zip.sig', size: 4 }, { id: 13, name: 'cbmp-mod-p1-v0.1.0.zip.sha256', size: 64 }]),
      ]);
    }
    if (url.endsWith('/assets/22')) return new Response('SIG', { status: sigStatus() });
    if (url.endsWith('/assets/12')) return new Response('SIG');
    if (url.endsWith('/assets/13') || url.endsWith('/assets/23')) return new Response(SHA);
    return new Response('x', { status: 404 });
  });

  it('falha vira falha da lista (502), com aviso sem o token; nunca anuncia a versão antiga', async () => {
    const f = twoMods(() => 500);
    const warns: string[] = [];
    const releases = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, logger: { warn: (o, m) => warns.push(JSON.stringify(o) + m) } });
    const t = await makeTestApp({ config: { githubToken: TOKEN, releasesRepo: 'dono/repo' }, releases });
    const r = await t.app.inject({ method: 'GET', url: '/v1/mod/latest?protocol=1' });
    expect(r.statusCode).toBe(502);
    expect(r.json().error).toBe('github_error');
    expect(warns.some((w) => w.includes('22'))).toBe(true);
    expect(warns.join('')).not.toContain(TOKEN);
  });

  it('com lista anterior boa, release novo cujo .sig falha não derruba a anterior nem é guardado por 60 s', async () => {
    const clock = { ms: 1_000_000 };
    let phase = 1;
    const f = fetchFake((url) => {
      if (url.includes('/releases?')) {
        const old = gh('mod-p1-v0.1.0', [{ id: 11, name: 'cbmp-mod-p1-v0.1.0.zip', size: 9 }, { id: 12, name: 'cbmp-mod-p1-v0.1.0.zip.sig', size: 4 }, { id: 13, name: 'cbmp-mod-p1-v0.1.0.zip.sha256', size: 64 }]);
        const neu = gh('mod-p1-v0.2.0', [{ id: 21, name: 'cbmp-mod-p1-v0.2.0.zip', size: 9 }, { id: 22, name: 'cbmp-mod-p1-v0.2.0.zip.sig', size: 4 }, { id: 23, name: 'cbmp-mod-p1-v0.2.0.zip.sha256', size: 64 }]);
        return Response.json(phase === 1 ? [old] : [neu, old]);
      }
      if (url.endsWith('/assets/22')) return new Response('SIG', { status: phase === 2 ? 500 : 200 });
      if (url.endsWith('/assets/12')) return new Response('SIG');
      return new Response(SHA);
    });
    const releases = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, nowMs: () => clock.ms, logger: { warn: () => {} } });
    const t = await makeTestApp({ config: { githubToken: TOKEN, releasesRepo: 'dono/repo' }, releases });
    const latest = async () => (await t.app.inject({ method: 'GET', url: '/v1/mod/latest?protocol=1' })).json().version as string;
    const lists = () => f.calls.filter((c) => c.url.includes('/releases?')).length;
    expect(await latest()).toBe('0.1.0');
    phase = 2; // publicaram 0.2.0, mas o .sig dele falha agora
    clock.ms += 61_000;
    expect(await latest()).toBe('0.1.0'); // lista anterior (exata); nada de lista nova "sem a 0.2.0" guardada por 60 s
    expect(lists()).toBe(2);
    phase = 3;
    clock.ms += 15_000; // falha só segura 15 s
    expect(await latest()).toBe('0.2.0');
    expect(lists()).toBe(3);
  });

  it('sem lista anterior: depois da falha, a próxima tentativa (15 s) com o .sig de volta anuncia a maior', async () => {
    const clock = { ms: 1_000_000 };
    let st = 500;
    const f = twoMods(() => st);
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl, nowMs: () => clock.ms, logger: { warn: () => {} } });
    await expect(c.list()).rejects.toBeInstanceOf(HttpError);
    st = 200;
    clock.ms += 15_000;
    const l = await c.list();
    expect(l.find((r) => r.tag === 'mod-p1-v0.2.0')!.assets.find((a) => a.id === 22)!.text).toBe('SIG');
  });
});

describe('cliente real: casos de borda (M1–M6)', () => {
  it('M4: segue para a página 2 quando a 1ª vem cheia (100)', async () => {
    const page1 = Array.from({ length: 100 }, (_, i) => gh(`x-${i}`, []));
    const f = fetchFake((url) => {
      if (url.endsWith('/releases?per_page=100')) return Response.json(page1);
      if (url.endsWith('/releases?per_page=100&page=2')) return Response.json([gh('launcher-v0.1.0', [])]);
      return new Response('x', { status: 404 });
    });
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl });
    const l = await c.list();
    expect(l.length).toBe(101);
    expect(l.at(-1)!.tag).toBe('launcher-v0.1.0');
  });

  it('M5: chamadas concorrentes de list() compartilham a mesma busca', async () => {
    const f = fetchFake(async () => { await sleep(10); return Response.json([]); });
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl });
    await Promise.all([c.list(), c.list(), c.list()]);
    expect(f.calls.length).toBe(1);
  });

  it('M5: item da lista com formato inválido é pulado', async () => {
    const f = fetchFake(() => Response.json([{ nada: 1 }, gh('launcher-v0.1.0', [])]));
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl });
    expect((await c.list()).map((r) => r.tag)).toEqual(['launcher-v0.1.0']);
  });

  it('M5: redirect sem Location ou para http → 502 sem seguir', async () => {
    for (const headers of [{}, { location: 'http://objects.example/b' }] as Array<Record<string, string>>) {
      const f = fetchFake(() => new Response(null, { status: 302, headers }));
      const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl });
      expect((await errOf(c.download(7))).code).toBe('github_error');
      expect(f.calls.length).toBe(1);
    }
  });

  it('M5: sem Content-Length usa o tamanho da lista; desconhecido → 502 no_length', async () => {
    const f = fetchFake((url) => {
      if (url.includes('/releases?')) return Response.json([gh('launcher-v0.1.0', [{ id: 7, name: 'a-setup.exe', size: 3 }])]);
      return new Response(dribble(3, 1));
    });
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl });
    expect((await errOf(c.download(7))).upstream?.error).toBe('no_length');
    await c.list();
    expect((await c.download(7)).size).toBe(3);
  });

  it('M2: pede accept-encoding identity e recusa resposta com content-encoding', async () => {
    const f = fetchFake((url) => {
      if (isAsset(7)(url)) return new Response(null, { status: 302, headers: { location: 'https://objects.example/b' } });
      return new Response('abc', { headers: { 'content-length': '3', 'content-encoding': 'gzip' } });
    });
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl });
    expect((await errOf(c.download(7))).upstream?.error).toBe('content_encoding');
    for (const call of f.calls) expect(hdr(call.init, 'accept-encoding')).toBe('identity');
  });

  it('M1: textos lidos em paralelo (limitado a 8)', async () => {
    let active = 0, peak = 0;
    const assets = Array.from({ length: 20 }, (_, i) => ({ id: 100 + i, name: `f${i}.sig`, size: 4 }));
    const f = fetchFake(async (url) => {
      if (url.includes('/releases?')) return Response.json([gh('launcher-v0.1.0', assets)]);
      active++; peak = Math.max(peak, active);
      await sleep(5);
      active--;
      return new Response('S');
    });
    const c = createGitHubReleases({ token: TOKEN, repo: 'dono/repo', fetchImpl: f.impl });
    await c.list();
    expect(peak).toBeGreaterThan(1);
    expect(peak).toBeLessThanOrEqual(8);
  });

  it('M6: release sem data (pub_date vazio) não é oferecido', async () => {
    const t = await setup();
    t.fake.releases = [launcher('0.3.0', {}, { publishedAt: '' }), launcher('0.2.0')];
    expect((await t.get('/v1/launcher/update/windows/x86_64/0.1.0')).json().version).toBe('0.2.0');
  });

  it("M3: Content-Disposition codifica ' ( ) *", async () => {
    const t = await setup();
    const a = asset("x'(1)*.exe", 3);
    t.fake.releases = [rel('launcher-v0.2.0', [a])];
    t.fake.blobs.set(a.id, new Uint8Array(3));
    const r = await t.get(`/v1/dl/launcher-v0.2.0/${encodeURIComponent(a.name)}`);
    expect(r.headers['content-disposition']).toBe("attachment; filename*=UTF-8''x%27%281%29%2A.exe");
  });
});
