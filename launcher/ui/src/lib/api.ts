// Única ponte com o backend Rust: wrappers tipados de `invoke` com os mesmos nomes dos comandos.
import { invoke } from "@tauri-apps/api/core";

/** Espelha `settings::Settings` (serde, chaves em camelCase). */
export interface Settings {
  hubUrl: string;
  favorites: string[];
  recent: string[];
  gameDir: string | null;
  cacheLimitMB: number;
  maxServerContentMB: number;
  simulateLaunch: boolean;
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
export const setSettings = (settings: Settings): Promise<Settings> => call("set_settings", { settings });
export const appVersion = (): Promise<string> => call("app_version");
