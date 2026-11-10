import { fireEvent, render, screen } from "@testing-library/svelte";
import { beforeEach, describe, expect, it, vi } from "vitest";

const invoke = vi.hoisted(() => vi.fn());
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));

import Login from "./Login.svelte";
import { ApiError } from "../lib/api";
import { auth } from "../lib/stores";

describe("Login", () => {
  beforeEach(() => {
    invoke.mockReset();
    auth.set({ status: "anonymous", error: null });
  });

  it("botão Entrar com Discord chama login e mostra que aguarda o navegador", async () => {
    invoke.mockResolvedValueOnce(undefined);
    render(Login);
    await fireEvent.click(screen.getByRole("button", { name: "Entrar com Discord" }));
    expect(invoke).toHaveBeenCalledWith("login", undefined);
    expect(await screen.findByText(/Aguardando o navegador/)).toBeInTheDocument();
    expect(screen.queryByRole("button", { name: "Entrar com Discord" })).not.toBeInTheDocument();

    await fireEvent.click(screen.getByRole("button", { name: "Cancelar" }));
    expect(screen.getByRole("button", { name: "Entrar com Discord" })).toBeInTheDocument();
  });

  it("mostra o erro como texto", async () => {
    auth.set({ status: "anonymous", error: new ApiError("login_denied", "<b>Login cancelado</b> no Discord.") });
    render(Login);
    const alert = screen.getByRole("alert");
    expect(alert).toHaveTextContent("<b>Login cancelado</b> no Discord.");
    expect(alert.querySelector("b")).toBeNull();
  });
});
