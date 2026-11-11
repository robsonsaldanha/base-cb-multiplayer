// Atualização do launcher: aviso do Rust (`update://available`) e checagem manual em Configurações.
import { writable } from "svelte/store";
import { onUpdateAvailable, type UpdateStatus } from "./api";

/** Versão nova conhecida (aviso periódico do Rust ou "procurar atualização"). */
export const updateInfo = writable<UpdateStatus | null>(null);

export function initUpdateEvents(): Promise<() => void> {
  return onUpdateAvailable((s) => updateInfo.set(s.available ? s : null));
}
