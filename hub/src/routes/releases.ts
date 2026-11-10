import { Readable } from 'node:stream';
import type { FastifyInstance } from 'fastify';
import type { ModInfo, UpdateInfo } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { ipKey } from './admin-login';
import { LAUNCHER_TAG, MOD_TAG, isServedTag, type GitHubReleases, type Release, type ReleaseAsset } from '../github';

// --- semver local (X.Y.Z[-pre][+build]; build ignorado na comparação) ---
const SEMVER = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/;
export interface Semver { major: number; minor: number; patch: number; pre: string[] }

export function parseSemver(v: string): Semver | null {
  if (v.length > 128) return null;
  const m = SEMVER.exec(v);
  if (!m) return null;
  const nums = [m[1], m[2], m[3]].map(Number);
  if (!nums.every(Number.isSafeInteger)) return null;
  return { major: nums[0]!, minor: nums[1]!, patch: nums[2]!, pre: m[4] ? m[4].split('.') : [] };
}

/** <0, 0, >0 como a.localeCompare(b). Entradas inválidas lançam. */
export function compareSemver(a: string, b: string): number {
  const x = parseSemver(a), y = parseSemver(b);
  if (!x || !y) throw new Error('semver inválida');
  for (const k of ['major', 'minor', 'patch'] as const) if (x[k] !== y[k]) return x[k] - y[k];
  if (!x.pre.length || !y.pre.length) return y.pre.length - x.pre.length; // sem pre > com pre
  for (let i = 0; i < Math.max(x.pre.length, y.pre.length); i++) {
    const p = x.pre[i], q = y.pre[i];
    if (p === undefined) return -1;
    if (q === undefined) return 1;
    const pn = /^\d+$/.test(p), qn = /^\d+$/.test(q);
    if (pn && qn) { const d = Number(p) - Number(q); if (d) return d; continue; }
    if (pn !== qn) return pn ? -1 : 1;
    if (p !== q) return p < q ? -1 : 1;
  }
  return 0;
}

const byVersionDesc = <T extends { version: string }>(a: T, b: T) => compareSemver(b.version, a.version);
const published = (r: Release) => !r.draft && !r.prerelease;
const SHA256_TEXT = /^([0-9a-fA-F]{64})(?:\s|$)/;
/** 60 req/min por IP (IPv6 agrupado por /64, como nas outras rotas). */
const RATE = { rateLimit: { max: 60, timeWindow: '1 minute', keyGenerator: (req: { ip: string }) => ipKey(req.ip) } };
/** Downloads repassados ao mesmo tempo (todos os clientes); acima disso → 503 busy. */
export const MAX_CONCURRENT_DOWNLOADS = 8;
/** RFC 5987 attr-char: encodeURIComponent deixa ' ( ) * passarem. */
const rfc5987 = (v: string) => encodeURIComponent(v).replace(/['()*]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase()}`);
const validDate = (d: string) => d !== '' && !Number.isNaN(Date.parse(d));

export async function releaseRoutes(app: FastifyInstance) {
  const { config } = app.deps;
  let activeDownloads = 0;

  /** Sem token (ou sem cliente) → 503 updates_disabled. */
  function client(): GitHubReleases {
    const r = app.deps.releases;
    if (!config.githubToken || !r) throw new HttpError(503, 'updates_disabled', 'atualizações desativadas neste Hub');
    return r;
  }
  const dlUrl = (tag: string, asset: string) => `${config.publicUrl}/v1/dl/${encodeURIComponent(tag)}/${encodeURIComponent(asset)}`;
  const textOf = (r: Release, name: string): string | undefined => r.assets.find((a) => a.name === name)?.text?.trim() || undefined;

  app.get<{ Params: { target: string; arch: string; current_version: string } }>(
    '/v1/launcher/update/:target/:arch/:current_version', { config: RATE },
    async (req, reply) => {
      const releases = client();
      const { target, arch, current_version: current } = req.params;
      if (!parseSemver(current)) throw new HttpError(400, 'invalid_version', 'versão atual inválida (esperado X.Y.Z)');
      if (target !== 'windows' || arch !== 'x86_64') return reply.status(204).send();
      const candidates: Array<{ version: string; release: Release; installer: ReleaseAsset; signature: string }> = [];
      for (const r of (await releases.list()).filter(published)) {
        const m = LAUNCHER_TAG.exec(r.tag);
        if (!m || !validDate(r.publishedAt)) continue;
        for (const a of r.assets) {
          if (!a.name.endsWith('-setup.exe')) continue;
          const signature = textOf(r, `${a.name}.sig`);
          if (signature) { candidates.push({ version: m[1]!, release: r, installer: a, signature }); break; }
        }
      }
      const best = candidates.sort(byVersionDesc)[0];
      if (!best || compareSemver(best.version, current) <= 0) return reply.status(204).send();
      const body: UpdateInfo = {
        version: best.version,
        notes: best.release.body,
        pub_date: best.release.publishedAt,
        url: dlUrl(best.release.tag, best.installer.name),
        signature: best.signature,
      };
      return body;
    },
  );

  app.get<{ Querystring: { protocol?: string } }>('/v1/mod/latest', { config: RATE }, async (req) => {
    const releases = client();
    const raw = req.query.protocol;
    if (typeof raw !== 'string' || !/^(0|[1-9]\d{0,8})$/.test(raw)) {
      throw new HttpError(400, 'invalid_query', 'protocol deve ser um inteiro ≥ 0');
    }
    const protocol = Number(raw);
    const candidates: ModInfo[] = [];
    for (const r of (await releases.list()).filter(published)) {
      const m = MOD_TAG.exec(r.tag);
      if (!m || Number(m[1]) !== protocol) continue;
      const version = m[2]!;
      const zipName = `cbmp-mod-p${protocol}-v${version}.zip`;
      const zip = r.assets.find((a) => a.name === zipName);
      const signature = textOf(r, `${zipName}.sig`);
      const sha = SHA256_TEXT.exec(textOf(r, `${zipName}.sha256`) ?? '')?.[1]?.toLowerCase();
      if (!zip || !signature || !sha) continue;
      candidates.push({ version, protocol, url: dlUrl(r.tag, zipName), sha256: sha, size: zip.size, signature });
    }
    const best = candidates.sort(byVersionDesc)[0];
    if (!best) throw new HttpError(404, 'no_mod', `nenhum mod publicado para o protocolo ${protocol}`);
    return best;
  });

  app.get<{ Params: { tag: string; asset: string } }>(
    '/v1/dl/:tag/:asset', { config: RATE, exposeHeadRoute: false },
    async (req, reply) => {
      const releases = client();
      const { tag, asset } = req.params;
      const notFound = () => new HttpError(404, 'not_found', 'arquivo não encontrado');
      if (!isServedTag(tag)) throw notFound();
      const release = (await releases.list()).find((r) => r.tag === tag && published(r));
      const entry = release?.assets.find((a) => a.name === asset);
      if (!entry) throw notFound();
      if (activeDownloads >= MAX_CONCURRENT_DOWNLOADS) throw new HttpError(503, 'busy', 'muitos downloads em andamento; tente em instantes');
      activeDownloads++;
      let released = false;
      const free = () => { if (!released) { released = true; activeDownloads--; } };
      let stream;
      try { stream = await releases.download(entry.id); } catch (e) { free(); throw e; }
      const body = Readable.fromWeb(stream as import('node:stream/web').ReadableStream<Uint8Array>);
      body.once('close', free);
      reply.raw.once('close', free);
      return reply
        .header('content-type', 'application/octet-stream')
        .header('content-length', String(stream.size))
        .header('content-disposition', `attachment; filename*=UTF-8''${rfc5987(entry.name)}`)
        .header('x-content-type-options', 'nosniff')
        .header('cache-control', 'no-store')
        .send(body);
    },
  );
}
