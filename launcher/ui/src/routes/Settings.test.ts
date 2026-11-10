import { fireEvent, render, screen } from "@testing-library/svelte";
import { beforeEach, describe, expect, it, vi } from "vitest";

const invoke = vi.hoisted(() => vi.fn());
const listen = vi.hoisted(() => vi.fn());
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/event", () => ({ listen }));

import Settings from "./Settings.svelte";
import { loadSettings, settings } from "../lib/settings";
import { notice } from "../lib/notice";
import { updateInfo } from "../lib/update";
import { baseSettings, fakeBackend, type Handler } from "../test-utils";
import type { Settings as S } from "../lib/api";

function setup(extra: Record<string, Handler> = {}) {
  let saved: S = { ...baseSettings };
  const b = fakeBackend(invoke, listen, {
    get_settings: () => saved,
    set_settings: (a) => (saved = { ...saved, ...(a!.settings as object) }),
    app_version: () => "0.1.0",
    ...extra,
  });
  return { b, saved: () => saved };
}

describe("Configurações", () => {
  beforeEach(() => {
    settings.set(null);
    notice.set(null);
    updateInfo.set(null);
  });

  it("pasta do jogo só muda pelo seletor do Rust", async () => {
    const { b } = setup({ pick_game_dir: () => ({ ...baseSettings, gameDir: "D:\\Jogos\\Hogwarts Legacy" }) });
    await loadSettings();
    render(Settings);
    expect(screen.getByText("Não encontrada")).toBeInTheDocument();
    await fireEvent.click(screen.getByRole("button", { name: "Escolher pasta do jogo" }));
    expect(await screen.findByText("D:\\Jogos\\Hogwarts Legacy")).toBeInTheDocument();
    expect(b.calls("pick_game_dir")).toEqual([undefined]);
    expect(b.calls("set_settings")).toHaveLength(0);
  });

  it("pasta inválida no seletor mostra o erro", async () => {
    setup({
      pick_game_dir: () => {
        throw { code: "game_invalid", message: "A pasta escolhida não tem o Hogwarts Legacy." };
      },
    });
    await loadSettings();
    render(Settings);
    await fireEvent.click(screen.getByRole("button", { name: "Escolher pasta do jogo" }));
    await vi.waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent(/não é do Hogwarts Legacy/));
  });

  it("limites do cache e do conteúdo salvos via set_settings", async () => {
    const { saved } = setup();
    await loadSettings();
    render(Settings);
    await fireEvent.input(screen.getByRole("spinbutton", { name: "Limite do cache (MB)" }), {
      target: { value: "4096" },
    });
    await fireEvent.input(screen.getByRole("spinbutton", { name: "Conteúdo máximo por servidor (MB)" }), {
      target: { value: "1024" },
    });
    await fireEvent.click(screen.getByRole("button", { name: "Salvar limites" }));
    await vi.waitFor(() => expect(saved().cacheLimitMB).toBe(4096));
    expect(saved().maxServerContentMB).toBe(1024);
    expect(await screen.findByText("Limites salvos.")).toBeInTheDocument();
  });

  it("modo simular liga/desliga e persiste", async () => {
    const { saved } = setup();
    await loadSettings();
    render(Settings);
    const box = screen.getByRole("checkbox", { name: /Modo simular/ });
    expect(box).toBeChecked();
    await fireEvent.click(box);
    await vi.waitFor(() => expect(saved().simulateLaunch).toBe(false));
  });

  it("limpar cache avisa sobre o jogo aberto e pede confirmação", async () => {
    const { b } = setup({ clear_cache: () => undefined });
    await loadSettings();
    render(Settings);
    await fireEvent.click(screen.getByRole("button", { name: "Limpar cache" }));
    expect(b.calls("clear_cache")).toHaveLength(0);
    expect(screen.getByText(/Feche o jogo antes/)).toBeInTheDocument();
    await fireEvent.click(screen.getByRole("button", { name: "Limpar agora" }));
    expect(await screen.findByText("Cache limpo.")).toBeInTheDocument();
    expect(b.calls("clear_cache")).toHaveLength(1);
  });

  it("versão e procurar atualização (sem e com versão nova)", async () => {
    let available = false;
    const { b } = setup({
      check_update: () =>
        available ? { available: true, version: "0.2.0", notes: "Correções" } : { available: false, version: null, notes: null },
      install_update: () => undefined,
    });
    await loadSettings();
    render(Settings);
    expect(await screen.findByText("v0.1.0")).toBeInTheDocument();
    await fireEvent.click(screen.getByRole("button", { name: "Procurar atualização" }));
    expect(await screen.findByText("Você já está na versão mais recente.")).toBeInTheDocument();

    available = true;
    await fireEvent.click(screen.getByRole("button", { name: "Procurar atualização" }));
    expect(await screen.findByText("Versão 0.2.0 disponível.")).toBeInTheDocument();
    expect(screen.getByText("Correções")).toBeInTheDocument();
    await fireEvent.click(screen.getByRole("button", { name: "Instalar atualização" }));
    await vi.waitFor(() => expect(b.calls("install_update")).toHaveLength(1));
  });
});
