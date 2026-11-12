import { fireEvent, render, screen, within } from "@testing-library/svelte";
import { get } from "svelte/store";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const invoke = vi.hoisted(() => vi.fn());
const listen = vi.hoisted(() => vi.fn());
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/event", () => ({ listen }));

import App from "../App.svelte";
import { auth } from "../lib/stores";
import { stopServers } from "../lib/servers";
import { blocked, playState } from "../lib/play";
import { notice } from "../lib/notice";
import { baseSettings, fakeBackend, SERVERS, type Handler } from "../test-utils";
import type { Settings } from "../lib/api";

const profile = { name: "Robson", discordId: "1", avatar: null };
const MB = 1024 * 1024;

function deferred<T>() {
  let resolve!: (v: T) => void;
  let reject!: (e: unknown) => void;
  const promise = new Promise<T>((a, b) => ((resolve = a), (reject = b)));
  return { promise, resolve, reject };
}

function setup(extra: Record<string, Handler> = {}) {
  let saved: Settings = { ...baseSettings };
  const b = fakeBackend(invoke, listen, {
    me: () => profile,
    app_version: () => "0.1.0",
    get_settings: () => saved,
    set_settings: (a) => (saved = { ...saved, ...(a!.settings as object) }),
    list_servers: () => SERVERS,
    cached_servers: () => null,
    ping_servers: (a) => (a!.ids as string[]).map((id) => ({ id, ms: 30 })),
    cancel_play: () => true,
    mod_status: () => ({ state: "notInstalled", otherFolders: [], goneFolders: [] }),
    ...extra,
  });
  return { b, saved: () => saved };
}

async function clickPlay(name: string) {
  const hero = await screen.findByRole("region", { name: "Destaque" });
  const btn = await within(hero).findByRole("button", { name: `Jogar em ${name}` });
  await fireEvent.click(btn);
}

describe("Jogar", () => {
  beforeEach(() => {
    stopServers();
    auth.set({ status: "checking" });
    playState.set({ status: "idle" });
    blocked.set(new Map());
  });
  afterEach(() => stopServers());

  it("barra de download: etapas, progresso, velocidade e cancelar", async () => {
    const run = deferred<unknown>();
    const { b } = setup({ play: () => run.promise });
    render(App);
    const bar = await screen.findByRole("contentinfo", { name: "Barra de download" });
    expect(within(bar).getByText("Nenhum download em andamento")).toBeInTheDocument();

    await clickPlay("Castelo Bruxo RP");
    expect(b.calls("play")).toEqual([{ serverId: "cb" }]);
    // Enquanto joga, nenhum outro Jogar fica ativo.
    for (const btn of screen.getAllByRole("button", { name: /^Jogar em/ })) expect(btn).toBeDisabled();

    b.emit("play://stage", { serverId: "cb", stage: "ticket" });
    expect(await within(bar).findByText("Pedindo ticket ao Hub…")).toBeInTheDocument();
    b.emit("play://stage", { serverId: "cb", stage: "manifest" });
    b.emit("play://stage", { serverId: "cb", stage: "download" });
    b.emit("content://progress", {
      doneBytes: 214 * MB,
      totalBytes: 344 * MB,
      bytesPerSec: 3.2 * MB,
      current: "mapa/client/a.lua",
    });
    expect(await within(bar).findByText(/214,0 MB de 344,0 MB/)).toBeInTheDocument();
    expect(within(bar).getByText(/3,2 MB\/s/)).toBeInTheDocument();
    expect(within(bar).getByText("mapa/client/a.lua")).toBeInTheDocument();
    const progress = within(bar).getByRole("progressbar", { name: "Progresso do download" });
    expect(progress).toHaveAttribute("value", String(214 * MB));
    expect(progress).toHaveAttribute("max", String(344 * MB));

    const steps = within(bar).getByRole("list", { name: "Etapas" });
    const items = within(steps).getAllByRole("listitem");
    expect(items.map((li) => li.textContent?.trim())).toEqual([
      "Ticket",
      "Conteúdo",
      "Download",
      "Montagem",
      "Mod",
      "Conexão",
      "Abrir",
    ]);
    expect(items[2]).toHaveAttribute("aria-current", "step");
    expect(items[0]).toHaveClass("done");

    // Evento de outro servidor é ignorado.
    b.emit("play://stage", { serverId: "outro", stage: "launch" });
    expect(items[2]).toHaveAttribute("aria-current", "step");

    await fireEvent.click(within(bar).getByRole("button", { name: "Cancelar download" }));
    expect(b.calls("cancel_play")).toHaveLength(1);
    run.reject({ code: "cancelled", message: "Entrada no servidor cancelada." });
    expect(await screen.findByText("Entrada no servidor cancelada.")).toBeInTheDocument();
    expect(await within(bar).findByText("Nenhum download em andamento")).toBeInTheDocument();
  });

  it("modo simular: Pronto para abrir com a conexão sem ticket e entra em Recentes", async () => {
    const { saved } = setup({
      play: () => ({
        kind: "simulated",
        preview: {
          host: "127.0.0.1",
          port: 7777,
          serverId: "cb",
          contentDir: "C:\\Users\\x\\AppData\\Local\\CBMultiplayer\\servers\\cb",
          protocol: 1,
        },
      }),
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    const ready = await screen.findByRole("region", { name: "Pronto para abrir" });
    expect(within(ready).getByText("127.0.0.1")).toBeInTheDocument();
    expect(within(ready).getByText("7777")).toBeInTheDocument();
    expect(within(ready).getByText(/servers\\cb/)).toBeInTheDocument();
    expect(within(ready).queryByText(/ticket/i)).toBeNull();
    await vi.waitFor(() => expect(saved().recent).toEqual(["cb"]));
    await fireEvent.click(within(ready).getByRole("button", { name: "Fechar" }));
    expect(screen.queryByRole("region", { name: "Pronto para abrir" })).toBeNull();
  });

  it("servidor com id 'constructor' joga normalmente", async () => {
    setup({
      list_servers: () => [{ ...SERVERS[0]!, id: "constructor", name: "Objeto" }],
      play: () => ({ kind: "started" }),
    });
    render(App);
    await clickPlay("Objeto");
    expect(await screen.findByText("Jogo aberto. Bom jogo!")).toBeInTheDocument();
  });

  it("jogo aberto de verdade", async () => {
    setup({ play: () => ({ kind: "started" }) });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    expect(await screen.findByText("Jogo aberto. Bom jogo!")).toBeInTheDocument();
  });

  it("hash inválido: mensagem pt-BR nomeando o resource", async () => {
    setup({
      play: () => {
        throw { code: "hash_mismatch", message: 'Arquivo com hash diferente no resource "mapa".' };
      },
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    const alert = await screen.findByRole("alert");
    expect(alert).toHaveTextContent(/corrompido/);
    expect(alert).toHaveTextContent('"mapa"');
  });

  it("banido: mensagem e Jogar desativado em todos", async () => {
    setup({
      play: () => {
        throw { code: "banned_global", message: "Sua conta está na lista global de banimentos." };
      },
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    expect(await screen.findByRole("alert")).toHaveTextContent(/banida/);
    for (const btn of screen.getAllByRole("button", { name: /^Jogar em/ })) expect(btn).toBeDisabled();
  });

  it("protocolo incompatível (manifest_mismatch): Jogar desativado só nesse servidor", async () => {
    setup({
      play: () => {
        throw { code: "manifest_mismatch", message: "O conteúdo anunciado não corresponde a este servidor." };
      },
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    expect(await screen.findByRole("alert")).toHaveTextContent(/não corresponde/);
    expect(screen.getAllByRole("button", { name: "Jogar em Castelo Bruxo RP" })[0]).toBeDisabled();
    expect(screen.getAllByRole("button", { name: "Jogar em Arena Global" })[0]).toBeEnabled();
  });

  it("jogo não encontrado: oferece o seletor de pasta (Rust)", async () => {
    const { b } = setup({
      play: () => {
        throw { code: "game_not_found", message: "Hogwarts Legacy não encontrado." };
      },
      pick_game_dir: () => ({ ...baseSettings, gameDir: "D:\\Jogos\\Hogwarts Legacy" }),
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    const alert = await screen.findByRole("alert");
    await fireEvent.click(within(alert.closest("section")!).getByRole("button", { name: "Escolher pasta do jogo" }));
    await vi.waitFor(() => expect(b.calls("pick_game_dir")).toHaveLength(1));
  });

  it("jogo não encontrado com uma pasta não verificada: sugere a pasta e confirma pelo Rust", async () => {
    const EPIC = "C:\\Program Files\\Epic Games\\HogwartsLegacy";
    const { b } = setup({
      play: () => {
        throw { code: "game_not_found", message: "Hogwarts Legacy não encontrado." };
      },
      detect_game: () => ({ settings: baseSettings, candidate: { dir: EPIC, store: "epic" }, ambiguous: false }),
      use_detected_game: () => ({ ...baseSettings, gameDir: EPIC }),
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    const group = await screen.findByRole("group", { name: "Pasta do jogo encontrada" });
    expect(group).toHaveTextContent("O launcher encontrou uma pasta que parece ser do Hogwarts Legacy:");
    expect(within(group).getByText(EPIC)).toBeInTheDocument();
    expect(group).toHaveTextContent(/não conseguiu verificar quem controla esta pasta/);
    // O seletor manual continua lá.
    expect(screen.getByRole("button", { name: "Escolher pasta do jogo" })).toBeInTheDocument();
    await fireEvent.click(within(group).getByRole("button", { name: "Usar esta pasta" }));
    await vi.waitFor(() => expect(b.calls("use_detected_game")).toEqual([{ dir: EPIC }]));
    await vi.waitFor(() =>
      expect(screen.queryByRole("group", { name: "Pasta do jogo encontrada" })).not.toBeInTheDocument(),
    );
    expect(b.calls("set_settings")).toHaveLength(0);
  });

  it("sessão expirada volta ao login", async () => {
    setup({
      play: () => {
        throw { code: "session_expired", message: "Sua sessão expirou. Entre de novo." };
      },
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    expect(await screen.findByRole("button", { name: "Entrar com Discord" })).toBeInTheDocument();
    expect(get(auth)).toMatchObject({ status: "anonymous", error: { code: "session_expired" } });
  });

  it("servidor indisponível e disco cheio com textos próprios", async () => {
    let code = "server_unavailable";
    setup({
      play: () => {
        throw { code, message: code === "disk_full" ? "Não foi possível gravar em D:\\cache: disco cheio" : "x" };
      },
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    expect(await screen.findByRole("alert")).toHaveTextContent("Servidor indisponível");
    code = "disk_full";
    await clickPlay("Castelo Bruxo RP");
    await vi.waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent(/Disco cheio/));
    expect(screen.getByRole("alert")).toHaveTextContent("D:\\cache");
  });
});

describe("Jogar — mod na pasta do jogo", () => {
  beforeEach(() => {
    stopServers();
    auth.set({ status: "checking" });
    playState.set({ status: "idle" });
    blocked.set(new Map());
    notice.set(null);
  });
  afterEach(() => stopServers());

  const CONFLICT_ERR = { code: "mod_conflict", message: "Já existe outro carregador de mods (UE4SS) na pasta do jogo." };

  it("conflito: explica, usa o carregador do CB Multiplayer e repete o Jogar", async () => {
    let plays = 0;
    const { b } = setup({
      play: () => {
        plays += 1;
        if (plays === 1) throw CONFLICT_ERR;
        return { kind: "started" };
      },
      resolve_mod_conflict: () => ({ state: "installed", version: "0.2.0", otherFolders: [], goneFolders: [] }),
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    const panel = (await screen.findByRole("alert")).closest("section")!;
    expect(panel).toHaveTextContent("Já existe outro carregador de mods (UE4SS) na pasta do jogo.");
    expect(panel).toHaveTextContent(/Phoenix\\Binaries\\Win64\\cbmp-backup/);
    expect(panel).toHaveTextContent(/seus outros mods do UE4SS ficam desligados/);
    await fireEvent.click(within(panel).getByRole("button", { name: "Usar o carregador do CB Multiplayer" }));
    await vi.waitFor(() => expect(b.calls("resolve_mod_conflict")).toHaveLength(1));
    expect(await screen.findByText("Jogo aberto. Bom jogo!")).toBeInTheDocument();
    expect(b.calls("play")).toEqual([{ serverId: "cb" }, { serverId: "cb" }]);
  });

  it("conflito cancelado no diálogo: não repete o Jogar", async () => {
    const { b } = setup({
      play: () => {
        throw CONFLICT_ERR;
      },
      resolve_mod_conflict: () => ({ state: "conflict", otherFolders: [], goneFolders: [] }),
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    const panel = (await screen.findByRole("alert")).closest("section")!;
    await fireEvent.click(within(panel).getByRole("button", { name: "Usar o carregador do CB Multiplayer" }));
    await vi.waitFor(() => expect(b.calls("resolve_mod_conflict")).toHaveLength(1));
    expect(b.calls("play")).toHaveLength(1);
    expect(screen.getByRole("button", { name: "Usar o carregador do CB Multiplayer" })).toBeInTheDocument();
  });

  it("sem permissão na pasta do jogo: explica o que fazer, sem botão de administrador", async () => {
    const message = "O launcher não tem permissão para gravar na pasta do jogo (C:\\Program Files\\Epic Games\\HogwartsLegacy). Nesta versão de testes, instale o jogo numa pasta em que o seu usuário possa gravar (por exemplo, outra biblioteca da Steam ou outra pasta na Epic Games) e tente de novo.";
    setup({
      play: () => {
        throw { code: "mod_folder_protected", message };
      },
    });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    const panel = (await screen.findByRole("alert")).closest("section")!;
    expect(panel).toHaveTextContent("O launcher não tem permissão para gravar na pasta do jogo.");
    expect(panel).toHaveTextContent(message);
    expect(panel).not.toHaveTextContent(/administrador/);
    expect(within(panel).queryByRole("button", { name: "Usar o carregador do CB Multiplayer" })).toBeNull();
    expect(within(panel).getAllByRole("button").map((b) => b.textContent)).toEqual(["Fechar"]);
  });

  it("texto vindo do Rust/servidor continua texto (sem HTML)", async () => {
    const evil = '<img src=x onerror="alert(1)"> pasta';
    setup({
      play: () => {
        throw { code: "mod_conflict", message: evil };
      },
    });
    const { container } = render(App);
    await clickPlay("Castelo Bruxo RP");
    const panel = (await screen.findByRole("alert")).closest("section")!;
    expect(panel).toHaveTextContent(evil);
    expect(container.querySelector("img[src='x']")).toBeNull();
  });

  it("aviso de pasta anterior do jogo aparece sem parar nada", async () => {
    const { b } = setup({ play: () => ({ kind: "started" }) });
    render(App);
    await clickPlay("Castelo Bruxo RP");
    expect(await screen.findByText("Jogo aberto. Bom jogo!")).toBeInTheDocument();
    b.emit("mod://warning", {
      code: "mod_old_folder",
      message: "O mod do CB Multiplayer continua na pasta anterior do jogo: E:\\HL.",
    });
    await vi.waitFor(() => expect(get(notice)?.code).toBe("mod_old_folder"));
    expect(await screen.findByText(/E:\\HL/)).toBeInTheDocument();
    expect(screen.getByText("Jogo aberto. Bom jogo!")).toBeInTheDocument();
  });
});
