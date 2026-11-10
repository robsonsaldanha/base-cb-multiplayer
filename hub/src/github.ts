import { z } from 'zod';
import { HttpError } from './errors';

/** Asset de um release. `text` vem preenchido só para `.sig`/`.sha256` pequenos (lidos junto com a lista). */
export interface ReleaseAsset { id: number; name: string; size: number; text?: string }
export interface Release {
  tag: string;
  draft: boolean;
  prerelease: boolean;
  /** RFC 3339 (published_at, ou created_at se ainda não publicado). */
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
const TEXT_MAX = 8 * 1024;
const TIMEOUT_MS = 15_000;

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
const GhList = z.array(z.unknown());

const fail = (msg: string, endpoint: string, detail: { status?: number; error?: string } = {}) =>
  new HttpError(502, 'github_error', msg, { service: 'github', endpoint, ...detail });

export interface GitHubReleasesOptions {
  token: string;
  repo: string;
  fetchImpl?: typeof fetch;
  /** Relógio em ms (injetável nos testes). */
  nowMs?: () => number;
}

export function createGitHubReleases(opts: GitHubReleasesOptions): GitHubReleases {
  const { token, repo } = opts;
  const fetchImpl = opts.fetchImpl ?? fetch;
  const nowMs = opts.nowMs ?? Date.now;
  const auth = { authorization: `Bearer ${token}`, 'x-github-api-version': '2022-11-28', 'user-agent': 'cbmp-hub' };
  let cache: { at: number; releases: Release[] } | null = null;
  let inflight: Promise<Release[]> | null = null;
  /** Textos de `.sig`/`.sha256` por id de asset (o id muda quando o arquivo é trocado). */
  const texts = new Map<number, string>();

  // Só o caminho relativo (sem host, query ou token) e o status vão para `upstream` (logado).
  async function call(endpoint: string, init: RequestInit, msg: string): Promise<Response> {
    let status: number | undefined;
    try {
      const res = await fetchImpl(`${API}/repos/${repo}/${endpoint}`, { ...init, signal: AbortSignal.timeout(TIMEOUT_MS) });
      status = res.status;
      return res;
    } catch (e) {
      throw fail(msg, endpoint.replace(/\?.*$/, ''), { status, error: e instanceof Error ? e.name : 'unknown' });
    }
  }

  async function assetText(id: number): Promise<string | undefined> {
    const cached = texts.get(id);
    if (cached !== undefined) return cached;
    const res = await downloadResponse(id);
    const text = (await res.text()).slice(0, TEXT_MAX);
    texts.set(id, text);
    return text;
  }

  async function fetchList(): Promise<Release[]> {
    const endpoint = 'releases?per_page=100';
    const res = await call(endpoint, { headers: { ...auth, accept: 'application/vnd.github+json' } }, 'falha ao listar releases');
    if (!res.ok) throw fail('falha ao listar releases', 'releases', { status: res.status });
    let raw: unknown;
    try { raw = GhList.parse(await res.json()); } catch { throw fail('resposta inesperada do GitHub', 'releases', { error: 'bad_shape' }); }
    const out: Release[] = [];
    for (const item of raw as unknown[]) {
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
          if ((a.name.endsWith('.sig') || a.name.endsWith('.sha256')) && a.size <= TEXT_MAX) {
            try { a.text = await assetText(a.id); } catch { /* sem texto → release vira inválido nas rotas */ }
          }
        }
      }
      out.push(rel);
    }
    const live = new Set(out.flatMap((r) => r.assets.map((a) => a.id)));
    for (const id of texts.keys()) if (!live.has(id)) texts.delete(id);
    return out;
  }

  /** GET do asset na API (Bearer, octet-stream) com redirect manual: o destino (storage) recebe a URL assinada, nunca o token. */
  async function downloadResponse(assetId: number): Promise<Response> {
    const endpoint = `releases/assets/${assetId}`;
    const res = await call(endpoint, { headers: { ...auth, accept: 'application/octet-stream' }, redirect: 'manual' }, 'falha ao baixar asset');
    if (res.status >= 300 && res.status < 400) {
      const location = res.headers.get('location');
      if (!location) throw fail('falha ao baixar asset', endpoint, { status: res.status, error: 'no_location' });
      let target: URL;
      try { target = new URL(location); } catch { throw fail('falha ao baixar asset', endpoint, { error: 'bad_location' }); }
      if (target.protocol !== 'https:') throw fail('falha ao baixar asset', endpoint, { error: 'bad_location' });
      let status: number | undefined;
      try {
        const r2 = await fetchImpl(target, { headers: { accept: 'application/octet-stream', 'user-agent': 'cbmp-hub' }, signal: AbortSignal.timeout(TIMEOUT_MS) });
        status = r2.status;
        if (!r2.ok || !r2.body) throw fail('falha ao baixar asset', endpoint, { status, error: 'redirect_target' });
        return r2;
      } catch (e) {
        if (e instanceof HttpError) throw e;
        throw fail('falha ao baixar asset', endpoint, { status, error: e instanceof Error ? e.name : 'unknown' });
      }
    }
    if (!res.ok || !res.body) throw fail('falha ao baixar asset', endpoint, { status: res.status });
    return res;
  }

  return {
    async list() {
      if (cache && nowMs() - cache.at < LIST_TTL_MS) return cache.releases;
      inflight ??= fetchList()
        .then((releases) => { cache = { at: nowMs(), releases }; return releases; })
        .finally(() => { inflight = null; });
      return inflight;
    },
    async download(assetId) {
      const res = await downloadResponse(assetId);
      const header = res.headers.get('content-length');
      const len = header !== null && /^\d+$/.test(header) ? Number(header) : NaN;
      const size = Number.isSafeInteger(len) && len >= 0 ? len
        : cache?.releases.flatMap((r) => r.assets).find((a) => a.id === assetId)?.size ?? -1;
      if (size < 0) { await res.body!.cancel().catch(() => {}); throw fail('falha ao baixar asset', `releases/assets/${assetId}`, { error: 'no_length' }); }
      return Object.assign(res.body!, { size });
    },
  };
}
