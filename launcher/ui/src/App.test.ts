import { fireEvent, render, screen, within } from "@testing-library/svelte";
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { Settings } from "./lib/api";

const invoke = vi.hoisted(() => vi.fn());
vi.mock("@tauri-apps/api/core", () => ({ invoke }));

import App from "./App.svelte";

const settings: Settings = {
  hubUrl: "https://hub.cbmp.invalid",
  favorites: [],
  recent: [],
  gameDir: null,
  cacheLimitMB: 10240,
  maxServerContentMB: 2048,
  simulateLaunch: true,
};

describe("App (layout A)", () => {
  beforeEach(() => {
    invoke.mockReset();
    invoke.mockImplementation(async (cmd: string) => {
      if (cmd === "app_version") return "0.1.0";
      if (cmd === "get_settings") return settings;
      throw new Error(`comando inesperado: ${cmd}`);
    });
  });

  it("monta menu lateral, destaque, lista e barra de download", async () => {
    render(App);

    const nav = screen.getByRole("navigation", { name: "Menu principal" });
    for (const item of ["Servidores", "Favoritos", "Recentes", "Configurações"]) {
      expect(within(nav).getByRole("button", { name: item })).toBeInTheDocument();
    }
    expect(screen.getByRole("region", { name: "Destaque" })).toBeInTheDocument();
    expect(screen.getByRole("region", { name: "Lista de servidores" })).toBeInTheDocument();
    expect(screen.getByRole("contentinfo", { name: "Barra de download" })).toBeInTheDocument();
  });

  it("mostra a versão vinda do comando app_version", async () => {
    render(App);
    expect(await screen.findByText("v0.1.0")).toBeInTheDocument();
    expect(invoke).toHaveBeenCalledWith("app_version", undefined);
  });

  it("troca a tela ativa pelo menu lateral", async () => {
    render(App);
    const nav = screen.getByRole("navigation", { name: "Menu principal" });
    const fav = within(nav).getByRole("button", { name: "Favoritos" });
    await fireEvent.click(fav);
    expect(fav).toHaveAttribute("aria-current", "page");
    expect(screen.getByRole("heading", { level: 1, name: "Favoritos" })).toBeInTheDocument();
  });
});
