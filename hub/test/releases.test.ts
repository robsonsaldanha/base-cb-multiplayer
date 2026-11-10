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
  async list(): Promise<Release[]> { return this.releases; }
  async download(assetId: number): Promise<AssetStream> {
    this.downloads.push(assetId);
    const b = this.blobs.get(assetId);
    if (!b) throw new HttpError(502, 'github_error', 'falha');
    return Object.assign(new Blob([b]).stream(), { size: b.length });
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
