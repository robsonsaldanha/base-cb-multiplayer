// Única ponte com o backend Rust: wrappers tipados de `invoke` com os mesmos nomes dos comandos.
import { invoke } from "@tauri-apps/api/core";

/** Espelha `settings::Settings` (serde, chaves em camelCase). `hubUrl` e `gameDir` são só leitura aqui. */
export interface Settings extends SettingsUpdate {
  readonly hubUrl: string;
  readonly gameDir: string | null;
}

/** Espelha `settings::SettingsUpdate`: o que a UI pode mudar. A pasta do jogo só muda pelo seletor (Rust). */
export interface SettingsUpdate {
  favorites: string[];
  recent: string[];
  cacheLimitMB: number;
  maxServerContentMB: number;
  simulateLaunch: boolean;
}

/** Extrai de `Settings` só os campos editáveis (o Rust recusa campos a mais). */
export function toUpdate(s: SettingsUpdate): SettingsUpdate {
  const { favorites, recent, cacheLimitMB, maxServerContentMB, simulateLaunch } = s;
  return { favorites, recent, cacheLimitMB, maxServerContentMB, simulateLaunch };
}

/** Erro devolvido pelo Rust (`error::AppError`): código estável + mensagem pt-BR. */
export class ApiError extends Error {
  override readonly name = "ApiError";
  readonly code: string;

  constructor(code: string, message: string) {
    super(message);
    this.code = code;
  }
}

function toApiError(e: unknown): ApiError {
  if (typeof e === "object" && e !== null && "code" in e && "message" in e) {
    const { code, message } = e as { code: unknown; message: unknown };
    if (typeof code === "string" && typeof message === "string") return new ApiError(code, message);
  }
  return new ApiError("unknown", typeof e === "string" ? e : "Erro inesperado.");
}

async function call<T>(cmd: string, args?: Record<string, unknown>): Promise<T> {
  try {
    return await invoke<T>(cmd, args);
  } catch (e) {
    throw toApiError(e);
  }
}

export const getSettings = (): Promise<Settings> => call("get_settings");
export const setSettings = (update: SettingsUpdate): Promise<Settings> =>
  call("set_settings", { settings: toUpdate(update) });
export const appVersion = (): Promise<string> => call("app_version");
