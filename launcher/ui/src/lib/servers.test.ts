import { get } from "svelte/store";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const invoke = vi.hoisted(() => vi.fn());
const listen = vi.hoisted(() => vi.fn());
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/event", () => ({ listen }));

import { baseSettings, fakeBackend, SERVERS } from "../test-utils";
import { loadServers, pings, refreshPings, serverList, stopServers } from "./servers";
import { loadSettings, pushRecent, settings, toggleFavorite } from "./settings";
import { notice } from "./notice";

describe("lista de servidores", () => {
  beforeEach(() => {
    stopServers();
    pings.set({});
  });
  afterEach(() => {
    stopServers();
    vi.useRealTimers();
  });

  it("Hub ok: lista fresca", async () => {
    fakeBackend(invoke, listen, { list_servers: () => SERVERS });
    await loadServers();
    expect(get(serverList)).toMatchObject({ status: "ready", servers: SERVERS, savedAt: null });
  });

  it("Hub fora: usa cached_servers e marca desatualizada; sem cache fica erro", async () => {
    const down = () => {
      throw { code: "hub_unreachable", message: "Não foi possível falar com o Hub." };
    };
    fakeBackend(invoke, listen, {
      list_servers: down,
      cached_servers: () => ({ savedAt: 1_700_000_000, servers: SERVERS.slice(0, 2) }),
    });
    await loadServers();
    expect(get(serverList)).toMatchObject({ status: "stale", savedAt: 1_700_000_000 });
    expect(get(serverList).servers).toHaveLength(2);

    fakeBackend(invoke, listen, { list_servers: down, cached_servers: () => null });
    await loadServers();
    expect(get(serverList)).toMatchObject({ status: "error", error: { code: "hub_unreachable" } });
  });

  it("Hub fora: tenta de novo com espera crescente até voltar", async () => {
    vi.useFakeTimers();
    let fail = 3;
    const b = fakeBackend(invoke, listen, {
      list_servers: () => {
        if (fail-- > 0) throw { code: "hub_unreachable", message: "x" };
        return SERVERS;
      },
      cached_servers: () => null,
    });
    await loadServers();
    expect(b.calls("list_servers")).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(10_000);
    expect(b.calls("list_servers")).toHaveLength(2);
    await vi.advanceTimersByTimeAsync(10_000);
    expect(b.calls("list_servers")).toHaveLength(2); // agora espera 20 s
    await vi.advanceTimersByTimeAsync(10_000);
    expect(b.calls("list_servers")).toHaveLength(3);
    await vi.advanceTimersByTimeAsync(40_000);
    expect(b.calls("list_servers")).toHaveLength(4);
    expect(get(serverList).status).toBe("ready");
    await vi.advanceTimersByTimeAsync(600_000);
    expect(b.calls("list_servers")).toHaveLength(4);
  });

  it("ping por id guarda ms (null = sem resposta)", async () => {
    const b = fakeBackend(invoke, listen, {
      ping_servers: (a) => (a!.ids as string[]).map((id) => ({ id, ms: id === "cb" ? 24 : null })),
    });
    await refreshPings(["cb", "hl"]);
    expect(b.calls("ping_servers")).toEqual([{ ids: ["cb", "hl"] }]);
    expect(get(pings)).toEqual({ cb: 24, hl: null });
    await refreshPings([]);
    expect(b.calls("ping_servers")).toHaveLength(1);
  });
});

describe("favoritos e recentes", () => {
  beforeEach(() => notice.set(null));

  it("favoritar/desfavoritar e recentes persistem via set_settings", async () => {
    let saved = { ...baseSettings };
    const b = fakeBackend(invoke, listen, {
      get_settings: () => saved,
      set_settings: (a) => (saved = { ...saved, ...(a!.settings as object) }),
    });
    await loadSettings();
    await toggleFavorite("cb");
    expect(get(settings)!.favorites).toEqual(["cb"]);
    await toggleFavorite("hl");
    await toggleFavorite("cb");
    expect(saved.favorites).toEqual(["hl"]);

    await pushRecent("a");
    await pushRecent("b");
    await pushRecent("a");
    expect(saved.recent).toEqual(["a", "b"]);
    for (let i = 0; i < 30; i++) await pushRecent(`s${i}`);
    expect(saved.recent).toHaveLength(20);
    expect(saved.recent[0]).toBe("s29");
    const last = b.calls("set_settings").at(-1) as { settings: Record<string, unknown> };
    expect(Object.keys(last.settings)).not.toContain("gameDir");
  });

  it("falha ao gravar mostra aviso e mantém o estado anterior", async () => {
    fakeBackend(invoke, listen, {
      get_settings: () => baseSettings,
      set_settings: () => {
        throw { code: "settings_unreadable", message: "Não foi possível ler settings.json" };
      },
    });
    await loadSettings();
    await toggleFavorite("cb");
    expect(get(settings)!.favorites).toEqual([]);
    expect(get(notice)).toMatchObject({ code: "settings_unreadable" });
  });
});
