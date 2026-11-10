import { Readable } from 'node:stream';
import type { FastifyInstance } from 'fastify';
import type { ModInfo, UpdateInfo } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { ipKey } from './admin-login';
import { LAUNCHER_TAG, MOD_TAG, isServedTag, type AssetStream, type GitHubReleases, type Release, type ReleaseAsset } from '../github';

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
/** Vagas de /v1/dl (estado do app; os testes leem). */
export interface DownloadSlots { active: number; perIp: Map<string, number>; watchdogs: number }
export const newDownloadSlots = (): DownloadSlots => ({ active: 0, perIp: new Map(), watchdogs: 0 });
/** Downloads simultâneos por IP (IPv6 por /64); acima disso → 429 too_many_downloads. O teto global vem de config.dlMaxConcurrent. */
export const MAX_DOWNLOADS_PER_IP = 2;
/** RFC 5987 attr-char: encodeURIComponent deixa ' ( ) * passarem. */
const rfc5987 = (v: string) => encodeURIComponent(v).replace(/['()*]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase()}`);
const validDate = (d: string) => d !== '' && !Number.isNaN(Date.parse(d));

export async function releaseRoutes(app: FastifyInstance) {
  const { config } = app.deps;

  /** Sem fonte (token do GitHub ou pasta local de dev) ou sem cliente → 503 updates_disabled. */
  function client(): GitHubReleases {
    const r = app.deps.releases;
    if ((!config.githubToken && !config.releasesDir) || !r) throw new HttpError(503, 'updates_disabled', 'atualizações desativadas neste Hub');
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
      const ip = ipKey(req.ip);
      const slots = app.downloads;
      if ((slots.perIp.get(ip) ?? 0) >= MAX_DOWNLOADS_PER_IP) {
        throw new HttpError(429, 'too_many_downloads', `no máximo ${MAX_DOWNLOADS_PER_IP} downloads simultâneos por conexão; aguarde terminar`);
      }
      if (slots.active >= config.dlMaxConcurrent) throw new HttpError(503, 'busy', 'muitos downloads em andamento; tente em instantes');
      // Vaga: ocupada aqui e devolvida uma única vez por freeSlot() (fim, erro, cliente que caiu — inclusive antes do
      // GitHub responder —, escrita parada ou taxa abaixo do mínimo). freeSlot() também limpa o cão de guarda.
      slots.active++;
      slots.perIp.set(ip, (slots.perIp.get(ip) ?? 0) + 1);
      let released = false;
      let watchdog: ReturnType<typeof setInterval> | undefined;
      const freeSlot = () => {
        if (watchdog !== undefined) { clearInterval(watchdog); watchdog = undefined; slots.watchdogs--; }
        if (released) return;
        released = true;
        slots.active--;
        const n = (slots.perIp.get(ip) ?? 1) - 1;
        if (n > 0) slots.perIp.set(ip, n); else slots.perIp.delete(ip);
      };
      // 'close' da resposta: terminou de entregar ao SO, o cliente caiu ou ela foi derrubada. Registrado ANTES de
      // esperar o GitHub. (O 'close' do req dispara ao fim do corpo do pedido, não serve.)
      reply.raw.once('close', freeSlot);
      let stream: AssetStream;
      try { stream = await releases.download(entry.id); } catch (e) { freeSlot(); throw e; }
      if (released || reply.raw.destroyed || req.raw.socket?.destroyed) {
        // Cliente sumiu enquanto o GitHub respondia: cancela o corpo (fecha a conexão com o GitHub) e não envia nada.
        freeSlot();
        await stream.cancel().catch(() => {});
        return reply.hijack();
      }
      const body = Readable.fromWeb(stream as import('node:stream/web').ReadableStream<Uint8Array>);
      const drop = (code: string, msg: string) => {
        req.log.warn({ code, tag, assetId: entry.id }, msg);
        freeSlot();
        body.destroy();
        reply.raw.destroy();
      };
      // Cão de guarda:
      // - escrita parada: buffer da resposta acima do limite (esperando 'drain') sem diminuir por dlStallTimeoutMs;
      // - taxa mínima: depois de uma janela de carência, média da última janela (bytes entregues ao socket) abaixo de
      //   dlMinKBps → derruba. Cobre o leitor em conta-gotas, que drena um pouco de cada vez e escapa da regra acima.
      const stallMs = config.dlStallTimeoutMs;
      const windowMs = config.dlRateWindowMs;
      const minBps = config.dlMinKBps * 1024;
      const sock = reply.raw.socket;
      const sent = () => (typeof sock?.bytesWritten === 'number' ? sock.bytesWritten : NaN);
      const start = Date.now();
      const base = sent();
      const samples: Array<{ t: number; b: number }> = [{ t: start, b: 0 }];
      let blockedSince: number | null = null;
      let lastLen = Infinity;
      slots.watchdogs++;
      watchdog = setInterval(() => {
        const res = reply.raw;
        const now = Date.now();
        const len = res.writableLength;
        if (!res.writableNeedDrain || len < lastLen) blockedSince = null;
        else if (blockedSince === null) blockedSince = now;
        lastLen = len;
        if (blockedSince !== null && now - blockedSince >= stallMs) {
          drop('dl_stalled', 'download parado: cliente não drena o socket');
          return;
        }
        const b = sent() - base;
        if (minBps <= 0 || Number.isNaN(b)) return;
        samples.push({ t: now, b });
        while (samples.length > 1 && samples[1]!.t <= now - windowMs) samples.shift();
        const old = samples[0]!;
        if (now - start >= windowMs && now - old.t >= windowMs && ((b - old.b) * 1000) / (now - old.t) < minBps) {
          drop('dl_too_slow', 'download abaixo da taxa mínima');
        }
      }, Math.max(10, Math.min(1000, Math.floor(stallMs / 4), Math.floor(windowMs / 4))));
      watchdog.unref();
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
