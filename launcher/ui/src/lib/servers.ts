// Lista do Hub, modo offline ("desatualizada", de `servers-cache.json`) e ping.
import { writable } from "svelte/store";
import { cachedServers, listServers, pingServers, toApiError, type ApiError, type ServerListing } from "./api";

export interface ServerListState {
  status: "loading" | "ready" | "stale" | "error";
  servers: ServerListing[];
  /** Quando a lista em cache foi gravada (só `stale`). */
  savedAt: number | null;
  error: ApiError | null;
}

const LOADING: ServerListState = { status: "loading", servers: [], savedAt: null, error: null };

export const serverList = writable<ServerListState>(LOADING);

/** Ping por id em ms; `null` = sem resposta; ausente = ainda não medido. `Map`: id do Hub nunca vira chave de objeto. */
export const pings = writable<ReadonlyMap<string, number | null>>(new Map());

/** Mesmo teto do Rust (`ping::MAX_IDS`): o resto fica "…" (não medido) em vez de ser cortado sem aviso. */
export const MAX_PING_IDS = 200;

export const PING_INTERVAL_MS = 30_000;
const RETRY_FIRST_MS = 10_000;
const RETRY_MAX_MS = 300_000;

let retryTimer: ReturnType<typeof setTimeout> | undefined;
let retryDelay = RETRY_FIRST_MS;
/** Cada carga (e cada `stopServers`) ganha um número; resposta de carga superada é ignorada. */
let generation = 0;

/** Para as novas tentativas e esquece a lista (logout, testes). */
export function stopServers(): void {
  serverList.set(LOADING);
  clearTimeout(retryTimer);
  retryTimer = undefined;
  retryDelay = RETRY_FIRST_MS;
  generation++;
}

/** Busca no Hub; se falhar, mostra a lista guardada como desatualizada e tenta de novo com espera crescente. */
export async function loadServers(): Promise<void> {
  clearTimeout(retryTimer);
  retryTimer = undefined;
  const gen = ++generation;
  try {
    const servers = await listServers();
    if (gen !== generation) return;
    retryDelay = RETRY_FIRST_MS;
    serverList.set({ status: "ready", servers: uniqueById(servers), savedAt: null, error: null });
  } catch (e) {
    const error = toApiError(e);
    let cached = null;
    try {
      cached = await cachedServers();
    } catch {
      // Sem cache legível: fica só o erro.
    }
    if (gen !== generation) return;
    serverList.set(
      cached && Array.isArray(cached.servers)
        ? { status: "stale", servers: uniqueById(cached.servers), savedAt: cached.savedAt, error }
        : { status: "error", servers: [], savedAt: null, error },
    );
    const wait = retryDelay;
    retryDelay = Math.min(retryDelay * 2, RETRY_MAX_MS);
    clearTimeout(retryTimer);
    retryTimer = setTimeout(() => void loadServers(), wait);
  }
}

/** Primeira ocorrência de cada id (o Rust já limpa; a UI não confia: id repetido quebraria o `{#each}` com chave). */
export function uniqueById<T extends { id: string }>(list: unknown): T[] {
  if (!Array.isArray(list)) return [];
  const seen = new Set<string>();
  return (list as T[]).filter((s) => typeof s?.id === "string" && !seen.has(s.id) && !!seen.add(s.id));
}

/** Mede o ping dos ids pedidos (os visíveis na tela; no máximo os primeiros 200). */
export async function refreshPings(ids: readonly string[]): Promise<void> {
  const unique = [...new Set(ids)].slice(0, MAX_PING_IDS);
  if (!unique.length) return;
  try {
    const res = await pingServers(unique);
    pings.update((cur) => {
      const next = new Map(cur);
      for (const r of res) next.set(r.id, r.ms);
      return next;
    });
  } catch {
    // Ping é informativo; falha não incomoda o usuário.
  }
}

/** Servidores da lista na ordem de `ids` (favoritos/recentes), sem repetir e ignorando ids fora da lista. */
export function byIds(ids: readonly string[], servers: readonly ServerListing[]): ServerListing[] {
  const index = new Map(servers.map((s) => [s.id, s] as const));
  return [...new Set(ids)].flatMap((id) => {
    const s = index.get(id);
    return s ? [s] : [];
  });
}
