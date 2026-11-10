import { get } from "svelte/store";
import { beforeEach, describe, expect, it, vi } from "vitest";

const invoke = vi.hoisted(() => vi.fn());
const listen = vi.hoisted(() => vi.fn());
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/event", () => ({ listen }));

import { auth, cancelLogin, handleAuthChanged, initAuth, refreshProfile, signOut, startLogin } from "./stores";
import { safeAvatar, type Profile } from "./api";

const profile: Profile = { name: "Robson", discordId: "123", avatar: null };

describe("stores de login", () => {
  beforeEach(() => {
    invoke.mockReset();
    listen.mockReset();
    auth.set({ status: "checking" });
  });

  it("com sessão válida vai para logado com o perfil", async () => {
    invoke.mockResolvedValueOnce(profile);
    await refreshProfile();
    expect(get(auth)).toEqual({ status: "logged_in", profile });
    expect(invoke).toHaveBeenCalledWith("me", undefined);
  });

  it("sem sessão vai para a tela de login sem erro; sessão expirada mostra o motivo", async () => {
    invoke.mockRejectedValueOnce({ code: "not_logged_in", message: "Entre" });
    await refreshProfile();
    expect(get(auth)).toEqual({ status: "anonymous", error: null });

    invoke.mockRejectedValueOnce({ code: "session_expired", message: "Sua sessão expirou." });
    await refreshProfile();
    expect(get(auth)).toMatchObject({ status: "anonymous", error: { code: "session_expired" } });
  });

  it("Hub fora ou cofre com erro não manda para o login: fica indisponível (sessão preservada)", async () => {
    for (const code of ["hub_unreachable", "vault_error", "hub_busy"]) {
      invoke.mockRejectedValueOnce({ code, message: "m" });
      await refreshProfile();
      expect(get(auth)).toMatchObject({ status: "unavailable", error: { code } });
    }
  });

  it("login: aguarda o navegador; falha ao abrir volta com erro; cancelar volta ao início", async () => {
    invoke.mockResolvedValueOnce(undefined);
    await startLogin();
    expect(invoke).toHaveBeenCalledWith("login", undefined);
    expect(get(auth)).toEqual({ status: "waiting" });
    cancelLogin();
    expect(get(auth)).toEqual({ status: "anonymous", error: null });

    invoke.mockRejectedValueOnce({ code: "browser_failed", message: "Não foi possível abrir o navegador." });
    await startLogin();
    expect(get(auth)).toMatchObject({ status: "anonymous", error: { code: "browser_failed" } });
  });

  it("auth://changed: sucesso busca o perfil; erro mostra o motivo; logout volta ao login", async () => {
    invoke.mockResolvedValueOnce(profile);
    await handleAuthChanged({ loggedIn: true, error: null });
    expect(get(auth)).toEqual({ status: "logged_in", profile });

    await handleAuthChanged({ loggedIn: false, error: { code: "login_denied", message: "Login cancelado no Discord." } });
    expect(get(auth)).toMatchObject({ status: "anonymous", error: { code: "login_denied", message: "Login cancelado no Discord." } });

    await handleAuthChanged({ loggedIn: false, error: null });
    expect(get(auth)).toEqual({ status: "anonymous", error: null });
  });

  it("signOut chama logout e volta ao login mesmo se o Rust falhar", async () => {
    auth.set({ status: "logged_in", profile });
    invoke.mockRejectedValueOnce({ code: "vault_error", message: "x" });
    await signOut();
    expect(invoke).toHaveBeenCalledWith("logout", undefined);
    expect(get(auth)).toEqual({ status: "anonymous", error: null });
  });

  it("initAuth escuta auth://changed antes de perguntar o perfil", async () => {
    const unlisten = vi.fn();
    let handler: ((e: { payload: unknown }) => void) | undefined;
    listen.mockImplementation(async (name: string, cb: (e: { payload: unknown }) => void) => {
      expect(name).toBe("auth://changed");
      expect(invoke).not.toHaveBeenCalled();
      handler = cb;
      return unlisten;
    });
    invoke.mockRejectedValueOnce({ code: "not_logged_in", message: "x" });
    const stop = await initAuth();
    expect(get(auth)).toEqual({ status: "anonymous", error: null });

    invoke.mockResolvedValueOnce(profile);
    handler!({ payload: { loggedIn: true, error: null } });
    await vi.waitFor(() => expect(get(auth)).toEqual({ status: "logged_in", profile }));
    stop();
    expect(unlisten).toHaveBeenCalled();
  });
});

describe("safeAvatar", () => {
  it("só aceita imagem em data: URL (o Rust baixa do CDN do Discord)", () => {
    expect(safeAvatar("data:image/png;base64,iVBORw==")).toBe("data:image/png;base64,iVBORw==");
    expect(safeAvatar("data:image/webp;base64,AAAA")).toBe("data:image/webp;base64,AAAA");
    for (const bad of [
      null,
      "",
      "https://cdn.discordapp.com/avatars/1/a.png",
      "https://evil.example/a.png",
      "data:image/svg+xml;base64,PHN2Zz4=",
      "data:text/html;base64,PHNjcmlwdD4=",
      "javascript:alert(1)",
      "data:image/png;base64,iVBOR w==",
    ]) {
      expect(safeAvatar(bad)).toBeNull();
    }
  });
});
