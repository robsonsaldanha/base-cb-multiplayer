import { beforeEach, describe, expect, it, vi } from "vitest";

const invoke = vi.hoisted(() => vi.fn());
vi.mock("@tauri-apps/api/core", () => ({ invoke }));

import { appVersion, getSettings, setSettings, type Settings } from "./api";

const s: Settings = {
  hubUrl: "https://hub.example",
  favorites: ["a"],
  recent: [],
  gameDir: null,
  cacheLimitMB: 10240,
  maxServerContentMB: 2048,
  simulateLaunch: true,
};

describe("api", () => {
  beforeEach(() => invoke.mockReset());

  it("chama os comandos com os mesmos nomes do Rust", async () => {
    invoke.mockResolvedValueOnce(s).mockResolvedValueOnce(s).mockResolvedValueOnce("1.2.3");
    expect(await getSettings()).toEqual(s);
    expect(await setSettings(s)).toEqual(s);
    expect(await appVersion()).toBe("1.2.3");
    expect(invoke.mock.calls).toEqual([
      ["get_settings", undefined],
      ["set_settings", { settings: s }],
      ["app_version", undefined],
    ]);
  });

  it("converte o erro do Rust em ApiError com código estável", async () => {
    invoke.mockRejectedValueOnce({ code: "invalid_settings", message: "Endereço do Hub inválido" });
    await expect(setSettings(s)).rejects.toMatchObject({
      name: "ApiError",
      code: "invalid_settings",
      message: "Endereço do Hub inválido",
    });
  });

  it("erro sem formato conhecido vira código unknown", async () => {
    invoke.mockRejectedValueOnce("falhou");
    await expect(getSettings()).rejects.toMatchObject({ code: "unknown", message: "falhou" });
  });
});
