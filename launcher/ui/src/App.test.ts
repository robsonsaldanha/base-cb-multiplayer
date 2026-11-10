import { fireEvent, render, screen, within } from "@testing-library/svelte";
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { Profile, Settings } from "./lib/api";

const invoke = vi.hoisted(() => vi.fn());
const listen = vi.hoisted(() => vi.fn());
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/event", () => ({ listen }));

import App from "./App.svelte";
import { auth } from "./lib/stores";

const settings: Settings = {
  hubUrl: "https://hub.cbmp.invalid",
  favorites: [],
  recent: [],
  gameDir: null,
  cacheLimitMB: 10240,
  maxServerContentMB: 2048,
  simulateLaunch: true,
};

const profile: Profile = { name: "Robson", discordId: "123", avatar: "data:image/png;base64,iVBORw==" };

let loggedIn = true;
let authHandler: ((e: { payload: unknown }) => void) | undefined;

describe("App (layout A)", () => {
  beforeEach(() => {
    loggedIn = true;
    auth.set({ status: "checking" });
    invoke.mockReset();
    invoke.mockImplementation(async (cmd: string) => {
      if (cmd === "app_version") return "0.1.0";
      if (cmd === "get_settings") return settings;
      if (cmd === "me") {
        if (loggedIn) return profile;
        throw { code: "not_logged_in", message: "Entre com o Discord para continuar." };
      }
      if (cmd === "logout") {
        loggedIn = false;
        return undefined;
      }
      throw new Error(`comando inesperado: ${cmd}`);
    });
    listen.mockReset();
    listen.mockImplementation(async (_name: string, cb: (e: { payload: unknown }) => void) => {
      authHandler = cb;
      return () => {};
    });
  });

  it("monta menu lateral, destaque, lista e barra de download", async () => {
    render(App);

    const nav = await screen.findByRole("navigation", { name: "Menu principal" });
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
    const nav = await screen.findByRole("navigation", { name: "Menu principal" });
    const fav = within(nav).getByRole("button", { name: "Favoritos" });
    await fireEvent.click(fav);
    expect(fav).toHaveAttribute("aria-current", "page");
    expect(screen.getByRole("heading", { level: 1, name: "Favoritos" })).toBeInTheDocument();
  });

  it("perfil no topo com nome e avatar; Sair volta à tela de login", async () => {
    render(App);
    const top = await screen.findByRole("group", { name: "Perfil" });
    expect(within(top).getByText("Robson")).toBeInTheDocument();
    expect(within(top).getByRole("img", { name: "Avatar de Robson" })).toHaveAttribute(
      "src",
      "data:image/png;base64,iVBORw==",
    );
    await fireEvent.click(within(top).getByRole("button", { name: "Sair" }));
    expect(await screen.findByRole("button", { name: "Entrar com Discord" })).toBeInTheDocument();
    expect(invoke).toHaveBeenCalledWith("logout", undefined);
  });

  it("avatar fora de data:image não é usado (inicial no lugar)", async () => {
    invoke.mockImplementation(async (cmd: string) => {
      if (cmd === "me") return { ...profile, avatar: "https://evil.example/x.png" };
      if (cmd === "app_version") return "0.1.0";
      return settings;
    });
    render(App);
    const top = await screen.findByRole("group", { name: "Perfil" });
    expect(within(top).queryByRole("img")).toBeNull();
    expect(within(top).getByText("R")).toBeInTheDocument();
  });

  it("sem sessão mostra o login; auth://changed com sucesso abre o launcher", async () => {
    loggedIn = false;
    render(App);
    expect(await screen.findByRole("button", { name: "Entrar com Discord" })).toBeInTheDocument();
    expect(screen.queryByRole("navigation", { name: "Menu principal" })).toBeNull();

    loggedIn = true;
    authHandler!({ payload: { loggedIn: true, error: null } });
    expect(await screen.findByRole("navigation", { name: "Menu principal" })).toBeInTheDocument();
  });
});
