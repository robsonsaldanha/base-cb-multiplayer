import { fireEvent, render, screen, within } from "@testing-library/svelte";
import { beforeEach, describe, expect, it, vi } from "vitest";

const invoke = vi.hoisted(() => vi.fn());
const listen = vi.hoisted(() => vi.fn());
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/event", () => ({ listen }));

import Settings from "./Settings.svelte";
import { loadSettings, settings } from "../lib/settings";
import { notice } from "../lib/notice";
import { get } from "svelte/store";
import { updateInfo } from "../lib/update";
import { baseSettings, fakeBackend, type Handler } from "../test-utils";
import type { Settings as S } from "../lib/api";

function setup(extra: Record<string, Handler> = {}) {
  let saved: S = { ...baseSettings };
  const b = fakeBackend(invoke, listen, {
    get_settings: () => saved,
    set_settings: (a) => (saved = { ...saved, ...(a!.settings as object) }),
    app_version: () => "0.1.0",
    mod_status: () => NOT_INSTALLED,
    ...extra,
  });
  return { b, saved: () => saved };
}

const NOT_INSTALLED = { state: "notInstalled", otherFolders: [] };
const INSTALLED = { state: "installed", version: "0.2.0", otherFolders: [] };
const CONFLICT = { state: "conflict", otherFolders: [] };

function modSection() {
  return screen.getByRole("region", { name: "Mod do jogo" });
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

  const EPIC = "C:\\Program Files\\Epic Games\\HogwartsLegacy";
  const epic = () => ({ settings: baseSettings, candidate: { dir: EPIC, store: "epic" }, ambiguous: false });

  it("sugestão: mostra a pasta sem dizer de onde veio e só grava pela confirmação do Rust", async () => {
    const { b } = setup({
      detect_game: epic,
      use_detected_game: () => ({ ...baseSettings, gameDir: EPIC }),
    });
    await loadSettings();
    render(Settings);
    const group = await screen.findByRole("group", { name: "Pasta do jogo encontrada" });
    expect(group).toHaveTextContent("O launcher encontrou uma pasta que parece ser do Hogwarts Legacy:");
    // Caminho completo do Windows, com as barras.
    expect(within(group).getByText(EPIC)).toBeInTheDocument();
    expect(EPIC).toContain("\\Epic Games\\");
    expect(group).toHaveTextContent(/não conseguiu verificar quem controla esta pasta/);
    expect(group).toHaveTextContent(/só se foi nela que você instalou o jogo/);
    expect(group).not.toHaveTextContent(/\(Epic Games\)|encontrada pela/);
    expect(screen.getByText("Não encontrada")).toBeInTheDocument();
    await fireEvent.click(within(group).getByRole("button", { name: "Usar esta pasta" }));
    // A UI só repete a pasta que o Rust achou; quem grava é o Rust (nunca set_settings).
    await vi.waitFor(() => expect(b.calls("use_detected_game")).toEqual([{ dir: EPIC }]));
    expect(await screen.findByText(EPIC)).toBeInTheDocument();
    expect(screen.queryByRole("group", { name: "Pasta do jogo encontrada" })).not.toBeInTheDocument();
    expect(b.calls("set_settings")).toHaveLength(0);
  });

  it("cancelar no diálogo do Windows mantém a sugestão", async () => {
    const { b } = setup({ detect_game: epic, use_detected_game: () => baseSettings });
    await loadSettings();
    render(Settings);
    const group = await screen.findByRole("group", { name: "Pasta do jogo encontrada" });
    await fireEvent.click(within(group).getByRole("button", { name: "Usar esta pasta" }));
    await vi.waitFor(() => expect(b.calls("use_detected_game")).toHaveLength(1));
    expect(screen.getByRole("group", { name: "Pasta do jogo encontrada" })).toBeInTheDocument();
    expect(screen.getByText("Não encontrada")).toBeInTheDocument();
  });

  it("pasta sugerida que mudou ou ficou ambígua: avisa e some", async () => {
    for (const code of ["game_candidate_changed", "game_ambiguous"]) {
      notice.set(null);
      setup({
        detect_game: epic,
        use_detected_game: () => {
          throw { code, message: "x" };
        },
      });
      await loadSettings();
      const r = render(Settings);
      await fireEvent.click(await screen.findByRole("button", { name: "Usar esta pasta" }));
      await vi.waitFor(() => expect(get(notice)?.code).toBe(code));
      expect(screen.queryByRole("group", { name: "Pasta do jogo encontrada" })).not.toBeInTheDocument();
      r.unmount();
    }
  });

  it("várias pastas: nenhuma sugestão, pede o seletor", async () => {
    const { b } = setup({ detect_game: () => ({ settings: baseSettings, candidate: null, ambiguous: true }) });
    await loadSettings();
    render(Settings);
    expect(await screen.findByText(/Mais de uma pasta parece ser do Hogwarts Legacy/)).toBeInTheDocument();
    expect(screen.queryByRole("group", { name: "Pasta do jogo encontrada" })).not.toBeInTheDocument();
    expect(screen.queryByRole("button", { name: "Usar esta pasta" })).not.toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Escolher pasta do jogo" })).toBeInTheDocument();
    expect(b.calls("use_detected_game")).toHaveLength(0);
  });

  it("sem candidata: nenhuma sugestão; pasta configurada que não vale mais: sugestão aparece", async () => {
    const { b } = setup({ detect_game: () => ({ settings: baseSettings, candidate: null, ambiguous: false }) });
    await loadSettings();
    const r = render(Settings);
    await vi.waitFor(() => expect(b.calls("detect_game")).toHaveLength(1));
    expect(screen.queryByRole("group", { name: "Pasta do jogo encontrada" })).not.toBeInTheDocument();
    r.unmount();

    // O Rust só devolve candidata quando a pasta configurada deixou de ser válida: a sugestão aparece mesmo assim.
    const stale = { ...baseSettings, gameDir: "D:\\Steam\\Hogwarts Legacy" };
    setup({ detect_game: () => ({ settings: stale, candidate: { dir: EPIC, store: "epic" }, ambiguous: false }) });
    await loadSettings();
    render(Settings);
    expect(await screen.findByText("D:\\Steam\\Hogwarts Legacy")).toBeInTheDocument();
    expect(await screen.findByRole("group", { name: "Pasta do jogo encontrada" })).toBeInTheDocument();
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
    expect(box).not.toBeChecked();
    await fireEvent.click(box);
    await vi.waitFor(() => expect(saved().simulateLaunch).toBe(true));
  });

  it("modo simular fica em Avançado e vem desmarcado sem configurações", async () => {
    setup();
    render(Settings);
    const adv = screen.getByRole("region", { name: "Avançado" });
    const box = within(adv).getByRole("checkbox", { name: "Modo simular: prepara tudo e não abre o jogo" });
    expect(box).not.toBeChecked();
    expect(box).toBeDisabled();
    expect(screen.queryByRole("heading", { name: "Modo simular" })).toBeNull();
  });

  it("estado do mod nos três valores", async () => {
    for (const [st, text] of [
      [NOT_INSTALLED, "Não instalado"],
      [INSTALLED, "Instalado 0.2.0"],
      [CONFLICT, "Conflito com outro carregador de mods"],
    ] as const) {
      setup({ mod_status: () => st });
      await loadSettings();
      const r = render(Settings);
      expect(await within(modSection()).findByText(text)).toBeInTheDocument();
      const remove = within(modSection()).getByRole("button", { name: "Remover mod do jogo" });
      if (st === NOT_INSTALLED) expect(remove).toBeDisabled();
      else expect(remove).toBeEnabled();
      r.unmount();
    }
  });

  it("remover o mod pede confirmação e chama remove_mod", async () => {
    const { b } = setup({
      mod_status: () => INSTALLED,
      remove_mod: () => ({ status: NOT_INSTALLED, folders: [{ gameDir: "D:\\HL", error: null }] }),
    });
    await loadSettings();
    render(Settings);
    const sec = modSection();
    await fireEvent.click(await within(sec).findByRole("button", { name: "Remover mod do jogo" }));
    expect(b.calls("remove_mod")).toHaveLength(0);
    expect(within(sec).getByText(/Feche o jogo antes/)).toBeInTheDocument();
    await fireEvent.click(within(sec).getByRole("button", { name: "Remover agora" }));
    expect(await within(sec).findByText("Mod removido do jogo.")).toBeInTheDocument();
    expect(b.calls("remove_mod")).toEqual([undefined]);
    expect(within(sec).getByText("Não instalado")).toBeInTheDocument();
  });

  it("remover com o jogo aberto mostra o erro e mantém o estado", async () => {
    setup({
      mod_status: () => INSTALLED,
      remove_mod: () => {
        throw { code: "game_running", message: "O Hogwarts Legacy está aberto. Feche o jogo e tente de novo." };
      },
    });
    await loadSettings();
    render(Settings);
    const sec = modSection();
    await fireEvent.click(await within(sec).findByRole("button", { name: "Remover mod do jogo" }));
    await fireEvent.click(within(sec).getByRole("button", { name: "Remover agora" }));
    await vi.waitFor(() =>
      expect(screen.getByRole("alert")).toHaveTextContent("O Hogwarts Legacy está aberto. Feche o jogo e tente de novo."),
    );
    expect(within(sec).getByText("Instalado 0.2.0")).toBeInTheDocument();
  });

  it("mod em pasta anterior do jogo: aparece, deixa remover e mostra o resultado por pasta", async () => {
    const OLD = "E:\\Jogos\\Hogwarts Legacy";
    setup({
      mod_status: () => ({ ...NOT_INSTALLED, otherFolders: [OLD] }),
      remove_mod: () => ({
        status: { ...NOT_INSTALLED, otherFolders: [OLD] },
        folders: [
          {
            gameDir: OLD,
            error: { code: "mod_folder_unreachable", message: `A pasta do jogo ${OLD} não está acessível agora.` },
          },
        ],
      }),
    });
    await loadSettings();
    render(Settings);
    const sec = modSection();
    expect(await within(sec).findByText(OLD)).toBeInTheDocument();
    expect(within(sec).getByText(/pasta anterior do jogo/)).toBeInTheDocument();
    await fireEvent.click(within(sec).getByRole("button", { name: "Remover mod do jogo" }));
    await fireEvent.click(within(sec).getByRole("button", { name: "Remover agora" }));
    const failed = await within(sec).findByRole("list", { name: "Pastas onde o mod não saiu" });
    expect(failed).toHaveTextContent(OLD);
    expect(failed).toHaveTextContent(/não está acessível/);
    expect(within(sec).queryByText("Mod removido do jogo.")).toBeNull();
  });

  it("abrir a pasta de logs do mod chama o Rust; pasta ausente avisa", async () => {
    let missing = false;
    const { b } = setup({
      open_mod_logs: () => {
        if (missing) throw { code: "mod_logs_missing", message: "A pasta de logs do mod ainda não existe." };
        return undefined;
      },
    });
    await loadSettings();
    render(Settings);
    const btn = within(modSection()).getByRole("button", { name: "Abrir pasta de logs do mod" });
    await fireEvent.click(btn);
    await vi.waitFor(() => expect(b.calls("open_mod_logs")).toEqual([undefined]));
    missing = true;
    await fireEvent.click(btn);
    await vi.waitFor(() =>
      expect(screen.getByRole("alert")).toHaveTextContent("A pasta de logs do mod ainda não existe."),
    );
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

  it("modo simular volta ao valor salvo se a gravação falhar", async () => {
    setup({
      set_settings: () => {
        throw { code: "settings_unreadable", message: "x" };
      },
    });
    await loadSettings();
    render(Settings);
    const box = screen.getByRole("checkbox", { name: /Modo simular/ });
    await fireEvent.click(box);
    await vi.waitFor(() => expect(box).not.toBeChecked());
  });

  it("mudar o modo simular não apaga o limite que está sendo digitado", async () => {
    const { saved } = setup();
    await loadSettings();
    render(Settings);
    const cache = screen.getByRole("spinbutton", { name: "Limite do cache (MB)" });
    await fireEvent.input(cache, { target: { value: "4096" } });
    await fireEvent.click(screen.getByRole("checkbox", { name: /Modo simular/ }));
    await vi.waitFor(() => expect(saved().simulateLaunch).toBe(true));
    expect(cache).toHaveValue(4096);
  });
});
