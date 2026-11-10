import { fireEvent, render, screen, within } from "@testing-library/svelte";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const invoke = vi.hoisted(() => vi.fn());
const listen = vi.hoisted(() => vi.fn());
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/event", () => ({ listen }));

import App from "../App.svelte";
import { auth } from "../lib/stores";
import { stopServers } from "../lib/servers";
import { baseSettings, fakeBackend, server, SERVERS, type Handler } from "../test-utils";
import type { Settings } from "../lib/api";

const profile = { name: "Robson", discordId: "1", avatar: null };

function setup(extra: Record<string, Handler> = {}) {
  let saved: Settings = { ...baseSettings };
  const b = fakeBackend(invoke, listen, {
    me: () => profile,
    app_version: () => "0.1.0",
    get_settings: () => saved,
    set_settings: (a) => (saved = { ...saved, ...(a!.settings as object) }),
    list_servers: () => SERVERS,
    cached_servers: () => null,
    ping_servers: (a) => (a!.ids as string[]).map((id) => ({ id, ms: id === "cb" ? 24 : id === "hl" ? null : 41 })),
    ...extra,
  });
  return { b, saved: () => saved };
}

const list = () => screen.findByRole("region", { name: "Lista de servidores" });
const rowNames = (region: HTMLElement) =>
  within(region)
    .queryAllByRole("listitem")
    .map((li) => li.querySelector(".name")?.textContent);

describe("Servidores", () => {
  beforeEach(() => {
    stopServers();
    auth.set({ status: "checking" });
  });
  afterEach(() => stopServers());

  it("destaque mostra todos os featured no topo, cada um com Jogar", async () => {
    setup();
    render(App);
    const hero = await screen.findByRole("region", { name: "Destaque" });
    expect(await within(hero).findByText("Castelo Bruxo RP")).toBeInTheDocument();
    expect(within(hero).getByText("Arena Global")).toBeInTheDocument();
    expect(within(hero).queryByText("Hogsmeade Life")).toBeNull();
    expect(within(hero).getByRole("button", { name: "Jogar em Castelo Bruxo RP" })).toBeEnabled();
    expect(within(hero).getByRole("button", { name: "Jogar em Arena Global" })).toBeEnabled();
    expect(within(hero).getByText("182/512")).toBeInTheDocument();
  });

  it("lista com jogadores, tags/idioma como texto e ping", async () => {
    const { b } = setup();
    render(App);
    const region = await list();
    await within(region).findByText("Hogsmeade Life");
    expect(rowNames(region)).toEqual(SERVERS.map((s) => s.name));
    const hl = within(region).getByText("Hogsmeade Life").closest("li")!;
    expect(within(hl).getByText("54/128")).toBeInTheDocument();
    expect(within(hl).getByText("pt-BR")).toBeInTheDocument();
    expect(within(hl).getByText("rp")).toBeInTheDocument();
    await vi.waitFor(() => expect(b.calls("ping_servers")).toHaveLength(1));
    expect(await within(hl).findByText("sem resposta")).toBeInTheDocument();
    const cb = within(region).getByText("Castelo Bruxo RP").closest("li")!;
    expect(within(cb).getByText("24 ms")).toBeInTheDocument();
  });

  it("filtros combinados: busca, tag, idioma e com vagas", async () => {
    setup();
    render(App);
    const region = await list();
    await within(region).findByText("Hogsmeade Life");
    const filters = screen.getByRole("group", { name: "Filtros" });

    await fireEvent.click(within(filters).getByRole("button", { name: "rp" }));
    expect(within(filters).getByRole("button", { name: "rp" })).toHaveAttribute("aria-pressed", "true");
    await fireEvent.change(within(filters).getByRole("combobox", { name: "Idioma" }), { target: { value: "pt-BR" } });
    await fireEvent.click(within(filters).getByRole("checkbox", { name: "Com vagas" }));
    expect(rowNames(region)).toEqual(["Castelo Bruxo RP", "Hogsmeade Life"]);

    await fireEvent.input(within(filters).getByRole("searchbox", { name: "Buscar servidor" }), {
      target: { value: "hog" },
    });
    expect(rowNames(region)).toEqual(["Hogsmeade Life"]);

    await fireEvent.input(within(filters).getByRole("searchbox", { name: "Buscar servidor" }), {
      target: { value: "nada disso" },
    });
    expect(within(region).getByText("Nenhum servidor com esses filtros.")).toBeInTheDocument();
  });

  it("favoritar persiste via set_settings e aparece em Favoritos", async () => {
    const { b, saved } = setup();
    render(App);
    const region = await list();
    await within(region).findByText("Hogsmeade Life");
    const star = within(region).getByRole("button", { name: "Favoritar Hogsmeade Life" });
    expect(star).toHaveAttribute("aria-pressed", "false");
    await fireEvent.click(star);
    await vi.waitFor(() => expect(saved().favorites).toEqual(["hl"]));
    expect(b.calls("set_settings")).toHaveLength(1);
    expect(within(region).getByRole("button", { name: "Remover Hogsmeade Life dos favoritos" })).toHaveAttribute(
      "aria-pressed",
      "true",
    );

    await fireEvent.click(screen.getByRole("button", { name: "Favoritos" }));
    const favs = await screen.findByRole("region", { name: "Servidores favoritos" });
    expect(rowNames(favs)).toEqual(["Hogsmeade Life"]);
  });

  it("Recentes segue a ordem salva", async () => {
    setup({ get_settings: () => ({ ...baseSettings, recent: ["duel", "cb"] }) });
    render(App);
    await within(await list()).findByText("Duelos Arena");
    await fireEvent.click(await screen.findByRole("button", { name: "Recentes" }));
    const rec = await screen.findByRole("region", { name: "Servidores recentes" });
    await within(rec).findByText("Duelos Arena");
    expect(rowNames(rec)).toEqual(["Duelos Arena", "Castelo Bruxo RP"]);
  });

  it("Hub fora: lista desatualizada do servers-cache.json", async () => {
    setup({
      list_servers: () => {
        throw { code: "hub_unreachable", message: "Não foi possível falar com o Hub." };
      },
      cached_servers: () => ({ savedAt: 1_700_000_000, servers: [SERVERS[2]] }),
    });
    render(App);
    const status = await screen.findByText(/Lista desatualizada/);
    expect(status).toBeInTheDocument();
    const region = await list();
    expect(rowNames(region)).toEqual(["Hogsmeade Life"]);
    expect(screen.getByRole("button", { name: "Atualizar lista" })).toBeInTheDocument();
  });

  it("nome e descrição do servidor nunca viram HTML", async () => {
    const evil = "<img src=x onerror=alert(1)>";
    setup({
      list_servers: () => [
        server("x", evil, { description: evil, featured: true, tags: [evil], locale: "<b>pt</b>" }),
      ],
    });
    render(App);
    const hero = await screen.findByRole("region", { name: "Destaque" });
    await within(hero).findAllByText(evil);
    const region = await list();
    for (const root of [hero, region]) {
      expect(within(root).getAllByText(evil).length).toBeGreaterThanOrEqual(2);
      expect(root.querySelector("img")).toBeNull();
      expect(root.querySelector("b")).toBeNull();
    }
    expect(document.querySelector("img[onerror]")).toBeNull();
  });

  it("teclado: setas movem o foco entre os botões Jogar da lista", async () => {
    setup();
    render(App);
    const region = await list();
    await within(region).findByText("Hogsmeade Life");
    const plays = within(region).getAllByRole("button", { name: /^Jogar em/ });
    plays[0]!.focus();
    await fireEvent.keyDown(plays[0]!, { key: "ArrowDown" });
    expect(document.activeElement).toBe(plays[1]);
    await fireEvent.keyDown(plays[1]!, { key: "End" });
    expect(document.activeElement).toBe(plays.at(-1));
    await fireEvent.keyDown(plays.at(-1)!, { key: "ArrowUp" });
    expect(document.activeElement).toBe(plays.at(-2));
    await fireEvent.keyDown(plays.at(-2)!, { key: "Home" });
    expect(document.activeElement).toBe(plays[0]);
  });

  it("id repetido do Hub e nos favoritos não quebra as telas", async () => {
    setup({
      list_servers: () => [SERVERS[0], { ...SERVERS[0], name: "Impostor" }, SERVERS[2], SERVERS[2]],
      get_settings: () => ({ ...baseSettings, favorites: ["hl", "hl", "cb"], recent: ["cb", "cb"] }),
    });
    render(App);
    const region = await list();
    await within(region).findByText("Hogsmeade Life");
    expect(rowNames(region)).toEqual(["Castelo Bruxo RP", "Hogsmeade Life"]);
    const hero = screen.getByRole("region", { name: "Destaque" });
    expect(within(hero).queryByText("Impostor")).toBeNull();

    await fireEvent.click(screen.getByRole("button", { name: "Favoritos" }));
    const favs = await screen.findByRole("region", { name: "Servidores favoritos" });
    expect(rowNames(favs)).toEqual(["Hogsmeade Life", "Castelo Bruxo RP"]);
    await fireEvent.click(screen.getByRole("button", { name: "Recentes" }));
    const rec = await screen.findByRole("region", { name: "Servidores recentes" });
    expect(rowNames(rec)).toEqual(["Castelo Bruxo RP"]);
  });
});
