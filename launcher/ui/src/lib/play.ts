// Estado do Jogar: etapas (`play://stage`), progresso (`content://progress`), resultado e erros por código.
import { get, writable } from "svelte/store";
import {
  cancelPlay,
  onPlayStage,
  onProgress,
  play,
  toApiError,
  type ApiError,
  type LaunchOutcome,
  type PlayStage,
  type Progress,
} from "./api";
import { blocksPlay } from "./errors";
import { pushRecent } from "./settings";
import { auth } from "./stores";

export const STAGES: readonly { id: PlayStage; label: string; doing: string }[] = [
  { id: "ticket", label: "Ticket", doing: "Pedindo ticket ao Hub…" },
  { id: "manifest", label: "Conteúdo", doing: "Lendo o conteúdo do servidor…" },
  { id: "download", label: "Download", doing: "Baixando o conteúdo do servidor…" },
  { id: "mount", label: "Montagem", doing: "Montando os arquivos…" },
  { id: "mod", label: "Mod", doing: "Conferindo o mod…" },
  { id: "connect", label: "Conexão", doing: "Preparando a conexão…" },
  { id: "launch", label: "Abrir", doing: "Abrindo o jogo…" },
];

export interface PlayTarget {
  id: string;
  name: string;
}

export type PlayState =
  | { status: "idle" }
  | { status: "running"; server: PlayTarget; stage: PlayStage | null; progress: Progress | null; cancelling: boolean }
  | { status: "done"; server: PlayTarget; outcome: LaunchOutcome }
  | { status: "failed"; server: PlayTarget; error: ApiError };

export const playState = writable<PlayState>({ status: "idle" });

/** Jogar desativado: `"*"` (banimento) ou id do servidor → código do erro. */
export const blocked = writable<Record<string, string>>({});

export function blockReason(map: Record<string, string>, id: string): string | null {
  return map["*"] ?? map[id] ?? null;
}

/** Escuta etapas e progresso do play em andamento (eventos de outro servidor são ignorados). */
export async function initPlayEvents(): Promise<() => void> {
  const offs = await Promise.all([
    onPlayStage((p) =>
      playState.update((s) =>
        s.status === "running" && s.server.id === p.serverId ? { ...s, stage: p.stage } : s,
      ),
    ),
    onProgress((p) => playState.update((s) => (s.status === "running" ? { ...s, progress: p } : s))),
  ]);
  return () => offs.forEach((off) => off());
}

export async function startPlay(server: PlayTarget): Promise<void> {
  if (get(playState).status === "running" || blockReason(get(blocked), server.id)) return;
  playState.set({ status: "running", server, stage: null, progress: null, cancelling: false });
  try {
    const outcome = await play(server.id);
    playState.set({ status: "done", server, outcome });
    void pushRecent(server.id);
  } catch (e) {
    const error = toApiError(e);
    if (error.code === "session_expired" || error.code === "not_logged_in") {
      playState.set({ status: "idle" });
      auth.set({ status: "anonymous", error });
      return;
    }
    const scope = blocksPlay(error.code);
    if (scope) blocked.update((b) => ({ ...b, [scope === "all" ? "*" : server.id]: error.code }));
    playState.set({ status: "failed", server, error });
  }
}

export async function cancel(): Promise<void> {
  playState.update((s) => (s.status === "running" ? { ...s, cancelling: true } : s));
  try {
    await cancelPlay();
  } catch {
    // O play termina sozinho com o próprio erro.
  }
}

export function dismissPlay(): void {
  playState.update((s) => (s.status === "running" ? s : { status: "idle" }));
}

/** Lista recarregada: bloqueios por servidor (protocolo) podem ter mudado; o banimento continua. */
export function clearServerBlocks(): void {
  blocked.update((b): Record<string, string> => {
    const all = b["*"];
    return all ? { "*": all } : {};
  });
}
