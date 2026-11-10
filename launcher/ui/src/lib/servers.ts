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

/** Ping por id em ms; `null` = sem resposta; ausente = ainda não medido. */
export const pings = writable<Record<string, number | null>>({});

export const PING_INTERVAL_MS = 30_000;
const RETRY_FIRST_MS = 10_000;
const RETRY_MAX_MS = 300_000;

let retryTimer: ReturnType<typeof setTimeout> | undefined;
let retryDelay = RETRY_FIRST_MS;
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
  const gen = generation;
  try {
    const servers = await listServers();
    if (gen !== generation) return;
    retryDelay = RETRY_FIRST_MS;
    serverList.set({ status: "ready", servers: Array.isArray(servers) ? servers : [], savedAt: null, error: null });
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
        ? { status: "stale", servers: cached.servers, savedAt: cached.savedAt, error }
        : { status: "error", servers: [], savedAt: null, error },
    );
    const wait = retryDelay;
    retryDelay = Math.min(retryDelay * 2, RETRY_MAX_MS);
    retryTimer = setTimeout(() => void loadServers(), wait);
  }
}

/** Mede o ping dos ids pedidos (os visíveis na tela). */
export async function refreshPings(ids: readonly string[]): Promise<void> {
  const unique = [...new Set(ids)];
  if (!unique.length) return;
  try {
    const res = await pingServers(unique);
    pings.update((cur) => {
      const next = { ...cur };
      for (const r of res) next[r.id] = r.ms;
      return next;
    });
  } catch {
    // Ping é informativo; falha não incomoda o usuário.
  }
}
