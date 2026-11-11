// Estado de login da UI. Tokens nunca chegam aqui: o Rust guarda a sessão e só devolve o perfil.
import { writable } from "svelte/store";
import { ApiError, login, logout, me, onAuthChanged, toApiError, type AuthChanged, type Profile } from "./api";

export type AuthState =
  | { status: "checking" }
  | { status: "anonymous"; error: ApiError | null }
  | { status: "waiting" }
  /** Sessão pode existir, mas o Hub/cofre falhou agora: não é motivo para pedir login de novo. */
  | { status: "unavailable"; error: ApiError }
  | { status: "logged_in"; profile: Profile };

export const auth = writable<AuthState>({ status: "checking" });

/** Códigos que só significam "precisa entrar" (sem mensagem de erro na tela). */
const SILENT = new Set(["not_logged_in"]);
/** Códigos que mandam para a tela de login; qualquer outro erro do `me` é falha passageira. */
const NEEDS_LOGIN = new Set(["not_logged_in", "session_expired"]);

const anonymous = (error: ApiError | null = null): AuthState => ({ status: "anonymous", error });

/** Pergunta o perfil ao Rust (renova a sessão do cofre se preciso). */
export async function refreshProfile(): Promise<void> {
  try {
    auth.set({ status: "logged_in", profile: await me() });
  } catch (e) {
    const err = toApiError(e);
    if (!NEEDS_LOGIN.has(err.code)) auth.set({ status: "unavailable", error: err });
    else auth.set(anonymous(SILENT.has(err.code) ? null : err));
  }
}

/** Abre o navegador no login do Hub e espera o deep link (`auth://changed`). */
export async function startLogin(): Promise<void> {
  auth.set({ status: "waiting" });
  try {
    await login();
  } catch (e) {
    auth.set(anonymous(toApiError(e)));
  }
}

/** Desiste de esperar o navegador (um deep link atrasado ainda pode concluir o login). */
export function cancelLogin(): void {
  auth.set(anonymous());
}

export async function signOut(): Promise<void> {
  try {
    await logout();
  } catch {
    // O Rust limpa o que der; a UI volta ao login de qualquer jeito.
  }
  auth.set(anonymous());
}

export async function handleAuthChanged(p: AuthChanged): Promise<void> {
  if (p.loggedIn) return refreshProfile();
  auth.set(anonymous(p.error ? new ApiError(p.error.code, p.error.message) : null));
}

/** Escuta `auth://changed` e então descobre se já há sessão. Devolve a função que para de escutar. */
export async function initAuth(): Promise<() => void> {
  const unlisten = await onAuthChanged((p) => void handleAuthChanged(p));
  await refreshProfile();
  return unlisten;
}
