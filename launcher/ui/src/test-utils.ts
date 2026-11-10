// Backend falso dos testes de UI: `invoke` responde por comando e `listen` guarda os ouvintes para `emit`.
import type { Mock } from "vitest";
import type { ServerListing, Settings } from "./lib/api";

export type Handler = (args: Record<string, unknown> | undefined) => unknown;
type Listener = (e: { payload: unknown }) => void;

export function fakeBackend(invoke: Mock, listen: Mock, handlers: Record<string, Handler>) {
  const listeners = new Map<string, Set<Listener>>();
  invoke.mockReset();
  listen.mockReset();
  invoke.mockImplementation(async (cmd: string, args?: Record<string, unknown>) => {
    const h = handlers[cmd];
    if (!h) throw new Error(`comando inesperado: ${cmd}`);
    return h(args);
  });
  listen.mockImplementation(async (name: string, cb: Listener) => {
    const set = listeners.get(name) ?? new Set<Listener>();
    set.add(cb);
    listeners.set(name, set);
    return () => set.delete(cb);
  });
  return {
    handlers,
    emit(name: string, payload: unknown) {
      for (const cb of listeners.get(name) ?? []) cb({ payload });
    },
    calls(cmd: string): unknown[] {
      return invoke.mock.calls.filter((c) => c[0] === cmd).map((c) => c[1]);
    },
  };
}

export const baseSettings: Settings = {
  hubUrl: "https://hub.cbmp.invalid",
  favorites: [],
  recent: [],
  gameDir: null,
  cacheLimitMB: 10240,
  maxServerContentMB: 2048,
  simulateLaunch: true,
};

export function server(id: string, name: string, extra: Partial<ServerListing> = {}): ServerListing {
  return {
    id,
    name,
    description: `Descrição de ${name}`,
    tags: ["rp"],
    locale: "pt-BR",
    players: 10,
    maxPlayers: 100,
    host: "127.0.0.1",
    gamePort: 7777,
    httpPort: 30120,
    version: "1.0.0",
    protocol: 1,
    featured: false,
    ...extra,
  };
}

export const SERVERS: ServerListing[] = [
  server("cb", "Castelo Bruxo RP", { featured: true, tags: ["rp", "magia"], players: 182, maxPlayers: 512 }),
  server("arena", "Arena Global", { featured: true, tags: ["pvp"], locale: "en-US", players: 64, maxPlayers: 64 }),
  server("hl", "Hogsmeade Life", { tags: ["rp"], players: 54, maxPlayers: 128 }),
  server("duel", "Duelos Arena", { tags: ["pvp", "duelo"], players: 12, maxPlayers: 64 }),
  server("sp", "Escola de Magia SP", { tags: ["rp", "escola"], players: 200, maxPlayers: 200 }),
  server("en", "Wizard School", { tags: ["rp"], locale: "en-US", players: 7, maxPlayers: 200 }),
];
