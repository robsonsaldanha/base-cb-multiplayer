import { z } from 'zod';
import { HttpError } from './errors';

/** Asset de um release. `text` vem preenchido só para `.sig`/`.sha256` pequenos (lidos junto com a lista). */
export interface ReleaseAsset { id: number; name: string; size: number; text?: string }
export interface Release {
  tag: string;
  draft: boolean;
  prerelease: boolean;
  /** RFC 3339 (published_at, ou created_at se ainda não publicado); '' se o GitHub não mandou nenhum. */
  publishedAt: string;
  body: string;
  assets: ReleaseAsset[];
}
/** Corpo do asset em stream + tamanho em bytes (vai no Content-Length). */
export type AssetStream = ReadableStream<Uint8Array> & { size: number };

/** Releases do repo privado. O token fica só aqui (nunca vai para log, resposta ou redirect). */
export interface GitHubReleases {
  list(): Promise<Release[]>;
  download(assetId: number): Promise<AssetStream>;
}

/** Tags que o Hub serve: `launcher-vX.Y.Z` e `mod-pN-vX.Y.Z`. */
const SEMVER_CORE = '(0|[1-9]\\d*)\\.(0|[1-9]\\d*)\\.(0|[1-9]\\d*)';
export const LAUNCHER_TAG = new RegExp(`^launcher-v(${SEMVER_CORE})$`);
export const MOD_TAG = new RegExp(`^mod-p(0|[1-9]\\d{0,8})-v(${SEMVER_CORE})$`);
export const isServedTag = (tag: string) => LAUNCHER_TAG.test(tag) || MOD_TAG.test(tag);

const API = 'https://api.github.com';
const LIST_TTL_MS = 60_000;
/** Falha ao atualizar a lista fica guardada por este tempo (sem nova chamada ao GitHub). */
const FAIL_TTL_MS = 15_000;
/** URL assinada do storage (redirect do GitHub) reaproveitada por asset; o GitHub a mantém válida por alguns minutos. */
const LOCATION_TTL_MS = 60_000;
const TEXT_MAX = 8 * 1024;
const TEXT_CONCURRENCY = 8;
const PER_PAGE = 100;
const MAX_PAGES = 10;
/** Falhas seguidas ao ler o mesmo `.sig`/`.sha256` até o release ser excluído da lista (em vez de travar a lista toda). */
const EXCLUDE_AFTER_FAILS = 3;
/** Fração do teto de chamadas/min que downloads podem usar; o resto fica para a lista e os textos. */
const DOWNLOAD_BUDGET_SHARE = 0.6;

const GhAsset = z.object({ id: z.number().int(), name: z.string(), size: z.number().int().min(0) });
const GhRelease = z.object({
  tag_name: z.string(),
  draft: z.boolean(),
  prerelease: z.boolean(),
  published_at: z.string().nullable().optional(),
  created_at: z.string().nullable().optional(),
  body: z.string().nullable().optional(),
  assets: z.array(GhAsset),
});

const fail = (msg: string, endpoint: string, detail: { status?: number; error?: string } = {}) =>
  new HttpError(502, 'github_error', msg, { service: 'github', endpoint, ...detail });
const errName = (e: unknown) => (e instanceof Error ? e.name : 'unknown');
const timeoutError = (msg: string) => new DOMException(msg, 'TimeoutError');

export interface ReleasesLogger { warn(obj: object, msg: string): void }

export interface GitHubReleasesOptions {
  token: string;
  repo: string;
  fetchImpl?: typeof fetch;
  /** Relógio em ms (injetável nos testes). */
  nowMs?: () => number;
  /** Prazo até chegarem os cabeçalhos de cada resposta (padrão 15 s). */
  headerTimeoutMs?: number;
  /** Prazo total para ler corpos pequenos (lista, `.sig`, `.sha256`) (padrão 15 s). */
  bodyTimeoutMs?: number;
  /** Download: erro se o GitHub passar este tempo sem mandar bytes enquanto o cliente espera (padrão 30 s). */
  idleTimeoutMs?: number;
  /** Teto global de chamadas à API do GitHub por minuto (padrão 80 ≈ 4800/h, abaixo da cota de 5000/h). */
  apiCallsPerMinute?: number;
  /** Avisos (sem token, sem URL assinada). */
  logger?: ReleasesLogger;
}

/** Executa `fn` em `items` com no máximo `n` ao mesmo tempo. */
async function pool<T>(items: T[], n: number, fn: (item: T) => Promise<void>): Promise<void> {
  let i = 0;
  await Promise.all(Array.from({ length: Math.min(n, items.length) }, async () => {
    while (i < items.length) await fn(items[i++]!);
  }));
}

/**
 * Repassa `body` com prazo de ociosidade: só conta enquanto há uma leitura pendente (pull), então um cliente lento
 * não derruba o download; um GitHub parado por `idleMs` sim.
 */
function idleGuard(body: ReadableStream<Uint8Array>, ac: AbortController, idleMs: number): ReadableStream<Uint8Array> {
  const reader = body.getReader();
  return new ReadableStream<Uint8Array>({
    async pull(ctrl) {
      let timer: ReturnType<typeof setTimeout> | undefined;
      const idle = new Promise<never>((_res, rej) => {
        timer = setTimeout(() => rej(timeoutError('GitHub parou de enviar dados')), idleMs);
      });
      try {
        const r = await Promise.race([reader.read(), idle]);
        if (r.done) ctrl.close();
        else ctrl.enqueue(r.value);
      } catch (e) {
        ac.abort(e);
        reader.cancel(e).catch(() => {});
        ctrl.error(e);
      } finally {
        clearTimeout(timer);
      }
    },
    cancel(reason) {
      ac.abort(reason);
      return reader.cancel(reason).catch(() => {});
    },
  }, { highWaterMark: 0 });
}

export function createGitHubReleases(opts: GitHubReleasesOptions): GitHubReleases {
  const { token, repo, logger } = opts;
  const fetchImpl = opts.fetchImpl ?? fetch;
  const nowMs = opts.nowMs ?? Date.now;
  const headerTimeoutMs = opts.headerTimeoutMs ?? 15_000;
  const bodyTimeoutMs = opts.bodyTimeoutMs ?? 15_000;
  const idleTimeoutMs = opts.idleTimeoutMs ?? 30_000;
  const apiCap = opts.apiCallsPerMinute ?? 80;
  const base = { 'user-agent': 'cbmp-hub', 'accept-encoding': 'identity' };
  const auth = { ...base, authorization: `Bearer ${token}`, 'x-github-api-version': '2022-11-28' };

  let cache: { at: number; releases: Release[] } | null = null;
  let failure: { at: number; err: HttpError } | null = null;
  let inflight: Promise<Release[]> | null = null;
  /** Textos de `.sig`/`.sha256` por id de asset (o id muda quando o arquivo é trocado). */
  const texts = new Map<number, string>();
  /** URL assinada do storage por asset (nunca logada). */
  const locations = new Map<number, { url: string; at: number }>();
  /** Falhas seguidas por asset de texto (zera ao ler com sucesso). */
  const textFails = new Map<number, number>();
  let apiWindow = { start: nowMs(), count: 0, downloads: 0 };
  const downloadCap = Math.max(1, Math.floor(apiCap * DOWNLOAD_BUDGET_SHARE));

  type CallKind = 'meta' | 'download';
  /** Teto global por minuto; downloads só podem usar parte dele, para não sufocar a lista e os textos. */
  function spendApiCall(kind: CallKind): void {
    const n = nowMs();
    if (n - apiWindow.start >= 60_000) apiWindow = { start: n, count: 0, downloads: 0 };
    if (apiWindow.count >= apiCap || (kind === 'download' && apiWindow.downloads >= downloadCap)) {
      throw new HttpError(503, 'busy', 'muitas requisições ao GitHub agora; tente em instantes');
    }
    apiWindow.count++;
    if (kind === 'download') apiWindow.downloads++;
  }

  /**
   * fetch com prazo só até os cabeçalhos (o timer é limpo quando o fetch resolve, então não corta o corpo).
   * O AbortController volta para quem lê o corpo aplicar o próprio prazo. `endpoint` (relativo, sem query) vai para o log.
   */
  async function timedFetch(url: string | URL, init: RequestInit, endpoint: string, msg: string): Promise<{ res: Response; ac: AbortController }> {
    const ac = new AbortController();
    const timer = setTimeout(() => ac.abort(timeoutError('prazo de cabeçalhos esgotado')), headerTimeoutMs);
    try {
      return { res: await fetchImpl(url, { ...init, signal: ac.signal }), ac };
    } catch (e) {
      throw fail(msg, endpoint, { error: errName(e) });
    } finally {
      clearTimeout(timer);
    }
  }

  async function withDeadline<T>(p: Promise<T>, ac: AbortController, endpoint: string, msg: string): Promise<T> {
    const timer = setTimeout(() => ac.abort(timeoutError('prazo de leitura esgotado')), bodyTimeoutMs);
    try { return await p; } catch (e) { throw fail(msg, endpoint, { error: errName(e) }); } finally { clearTimeout(timer); }
  }

  const discard = (res: Response) => { res.body?.cancel().catch(() => {}); };

  function rejectEncoded(res: Response, endpoint: string, msg: string): void {
    const ce = res.headers.get('content-encoding');
    if (ce && ce.trim().toLowerCase() !== 'identity') {
      discard(res);
      throw fail(msg, endpoint, { status: res.status, error: 'content_encoding' });
    }
  }

  async function apiGet(endpoint: string, init: RequestInit, msg: string, kind: CallKind) {
    spendApiCall(kind);
    return timedFetch(`${API}/repos/${repo}/${endpoint}`, init, endpoint.replace(/\?.*$/, ''), msg);
  }

  /** Segunda perna do download (storage assinado): sem token, só https. */
  async function fetchStorage(url: string, endpoint: string): Promise<{ res: Response; ac: AbortController }> {
    const msg = 'falha ao baixar asset';
    const r = await timedFetch(url, { headers: { ...base, accept: 'application/octet-stream' } }, endpoint, msg);
    if (!r.res.ok || !r.res.body) { discard(r.res); throw fail(msg, endpoint, { status: r.res.status, error: 'redirect_target' }); }
    rejectEncoded(r.res, endpoint, msg);
    return r;
  }

  /** GET do asset: URL assinada guardada, ou API (Bearer, octet-stream, redirect manual) → storage sem o token. */
  async function downloadResponse(assetId: number, kind: CallKind): Promise<{ res: Response; ac: AbortController }> {
    const endpoint = `releases/assets/${assetId}`;
    const msg = 'falha ao baixar asset';
    const saved = locations.get(assetId);
    if (saved && nowMs() - saved.at < LOCATION_TTL_MS) {
      try { return await fetchStorage(saved.url, endpoint); } catch { locations.delete(assetId); /* vencida → pede outra à API */ }
    }
    const { res, ac } = await apiGet(endpoint, { headers: { ...auth, accept: 'application/octet-stream' }, redirect: 'manual' }, msg, kind);
    if (res.status >= 300 && res.status < 400) {
      discard(res);
      const location = res.headers.get('location');
      if (!location) throw fail(msg, endpoint, { status: res.status, error: 'no_location' });
      let target: URL;
      try { target = new URL(location); } catch { throw fail(msg, endpoint, { error: 'bad_location' }); }
      if (target.protocol !== 'https:') throw fail(msg, endpoint, { error: 'bad_location' });
      const out = await fetchStorage(target.href, endpoint);
      locations.set(assetId, { url: target.href, at: nowMs() });
      return out;
    }
    if (!res.ok || !res.body) { discard(res); throw fail(msg, endpoint, { status: res.status }); }
    rejectEncoded(res, endpoint, msg);
    return { res, ac };
  }

  async function assetText(id: number): Promise<string> {
    const cached = texts.get(id);
    if (cached !== undefined) return cached;
    const { res, ac } = await downloadResponse(id, 'meta');
    const text = (await withDeadline(res.text(), ac, `releases/assets/${id}`, 'falha ao ler asset')).slice(0, TEXT_MAX);
    texts.set(id, text);
    return text;
  }

  async function fetchList(): Promise<Release[]> {
    const msg = 'falha ao listar releases';
    const raw: unknown[] = [];
    for (let page = 1; page <= MAX_PAGES; page++) {
      const endpoint = page === 1 ? `releases?per_page=${PER_PAGE}` : `releases?per_page=${PER_PAGE}&page=${page}`;
      const { res, ac } = await apiGet(endpoint, { headers: { ...auth, accept: 'application/vnd.github+json' } }, msg, 'meta');
      if (!res.ok) { discard(res); throw fail(msg, 'releases', { status: res.status }); }
      const items = await withDeadline(res.json() as Promise<unknown>, ac, 'releases', msg);
      if (!Array.isArray(items)) throw fail('resposta inesperada do GitHub', 'releases', { error: 'bad_shape' });
      raw.push(...items);
      if (items.length < PER_PAGE) break;
      if (page === MAX_PAGES) logger?.warn({ service: 'github', pages: MAX_PAGES }, 'lista de releases truncada (mais páginas que o limite)');
    }
    const out: Release[] = [];
    const wanted: Array<{ asset: ReleaseAsset; tag: string }> = [];
    for (const item of raw) {
      const p = GhRelease.safeParse(item);
      if (!p.success) continue;
      const r = p.data;
      const rel: Release = {
        tag: r.tag_name, draft: r.draft, prerelease: r.prerelease,
        publishedAt: r.published_at ?? r.created_at ?? '', body: r.body ?? '',
        assets: r.assets.map((a) => ({ id: a.id, name: a.name, size: a.size })),
      };
      // Só lê os textos dos releases que o Hub serve (economiza chamadas à API).
      if (!rel.draft && !rel.prerelease && isServedTag(rel.tag)) {
        for (const a of rel.assets) {
          if ((a.name.endsWith('.sig') || a.name.endsWith('.sha256')) && a.size <= TEXT_MAX) wanted.push({ asset: a, tag: rel.tag });
        }
      }
      out.push(rel);
    }
    // Falha ao ler um texto = falha da lista: nada de anunciar uma versão menor por um soluço do GitHub.
    // Mas um asset que falha sempre (EXCLUDE_AFTER_FAILS seguidas) tira só o release dele, para não congelar a lista.
    const failed: Array<{ assetId: number; tag: string; error: string; fails: number }> = [];
    await pool(wanted, TEXT_CONCURRENCY, async ({ asset: a, tag }) => {
      try {
        a.text = await assetText(a.id);
        textFails.delete(a.id);
      } catch (e) {
        const fails = (textFails.get(a.id) ?? 0) + 1;
        textFails.set(a.id, fails);
        failed.push({ assetId: a.id, tag, fails, error: e instanceof HttpError ? (e.upstream?.error ?? e.code) : errName(e) });
      }
    });
    if (failed.length) {
      logger?.warn({ service: 'github', failed }, 'falha ao ler .sig/.sha256 de releases');
      if (failed.some((f) => f.fails < EXCLUDE_AFTER_FAILS)) {
        throw fail('falha ao ler assinaturas dos releases', 'releases/assets', { error: 'asset_text' });
      }
      const excluded = [...new Set(failed.map((f) => f.tag))];
      logger?.warn({ service: 'github', excluded }, `releases excluídos: .sig/.sha256 ilegível após ${EXCLUDE_AFTER_FAILS} tentativas seguidas`);
      out.splice(0, out.length, ...out.filter((r) => !excluded.includes(r.tag)));
    }
    const live = new Set(out.flatMap((r) => r.assets.map((a) => a.id)));
    for (const id of texts.keys()) if (!live.has(id)) texts.delete(id);
    for (const id of textFails.keys()) if (!live.has(id)) textFails.delete(id);
    for (const id of locations.keys()) if (!live.has(id)) locations.delete(id);
    return out;
  }

  return {
    async list() {
      const n = nowMs();
      if (cache && n - cache.at < LIST_TTL_MS) return cache.releases;
      if (failure && n - failure.at < FAIL_TTL_MS) {
        if (cache) return cache.releases;
        throw failure.err;
      }
      inflight ??= fetchList()
        .then((releases) => {
          cache = { at: nowMs(), releases };
          failure = null;
          return releases;
        }, (e: unknown) => {
          const err = e instanceof HttpError ? e : fail('falha ao listar releases', 'releases', { error: errName(e) });
          failure = { at: nowMs(), err };
          logger?.warn({ service: 'github', code: err.code, upstream: err.upstream, stale: !!cache },
            cache ? 'falha ao atualizar releases; servindo a lista anterior' : 'falha ao atualizar releases');
          if (cache) return cache.releases;
          throw err;
        })
        .finally(() => { inflight = null; });
      return inflight;
    },
    async download(assetId) {
      const { res, ac } = await downloadResponse(assetId, 'download');
      const header = res.headers.get('content-length');
      const len = header !== null && /^\d+$/.test(header) ? Number(header) : NaN;
      const size = Number.isSafeInteger(len) ? len
        : cache?.releases.flatMap((r) => r.assets).find((a) => a.id === assetId)?.size ?? -1;
      if (size < 0) {
        discard(res);
        throw fail('falha ao baixar asset', `releases/assets/${assetId}`, { error: 'no_length' });
      }
      return Object.assign(idleGuard(res.body!, ac, idleTimeoutMs), { size });
    },
  };
}
