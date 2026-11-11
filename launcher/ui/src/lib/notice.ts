// Aviso de erro geral (gravar configurações, limpar cache…). Erros do Jogar ficam no painel do Jogar.
import { writable } from "svelte/store";
import { toApiError, type ApiError } from "./api";

export const notice = writable<ApiError | null>(null);

export function notify(e: unknown): void {
  notice.set(toApiError(e));
}
