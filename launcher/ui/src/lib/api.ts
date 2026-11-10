// Única ponte com o backend Rust: wrappers tipados de `invoke` com os mesmos nomes dos comandos.
import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";

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

export function toApiError(e: unknown): ApiError {
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

// ----- login -----

/** Espelha `commands::Profile`. `avatar` é `data:` URL baixada pelo Rust do CDN do Discord, ou `null`. */
export interface Profile {
  name: string;
  discordId: string;
  avatar: string | null;
}

/** Carga do evento `auth://changed` (`commands::AuthChanged`). */
export interface AuthChanged {
  loggedIn: boolean;
  error: { code: string; message: string } | null;
}

export const AUTH_CHANGED = "auth://changed";

/** Abre o navegador do sistema no login do Hub (o Rust escolhe a URL). O resultado chega por `auth://changed`. */
export const login = (): Promise<void> => call("login");
export const logout = (): Promise<void> => call("logout");
export const me = (): Promise<Profile> => call("me");

export function onAuthChanged(cb: (p: AuthChanged) => void): Promise<UnlistenFn> {
  return listen<AuthChanged>(AUTH_CHANGED, (e) => cb(e.payload));
}

const AVATAR_RE = /^data:image\/(png|jpeg|webp|gif);base64,[A-Za-z0-9+/]+={0,2}$/;

/** Só usa o avatar se for imagem raster em `data:` (a CSP também bloqueia qualquer outra origem). */
export function safeAvatar(avatar: string | null): string | null {
  return avatar && AVATAR_RE.test(avatar) ? avatar : null;
}
