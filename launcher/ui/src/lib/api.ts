// Única ponte com o backend Rust: wrappers tipados de `invoke` com os mesmos nomes dos comandos.
import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";

/** Espelha `settings::Settings` (serde, chaves em camelCase). `hubUrl` e `gameDir` são só leitura aqui. */
export interface Settings extends SettingsUpdate {
  readonly hubUrl: string;
  readonly gameDir: string | null;
  /** Versão do formato do arquivo (2 desde o launcher 0.2.0). */
  readonly settingsVersion: number;
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

// ----- servidores e ping -----

/** Espelha `hub::ServerListing`. Todos os textos vêm do servidor: sempre exibidos como texto. */
export interface ServerListing {
  id: string;
  name: string;
  description: string;
  tags: string[];
  locale: string;
  players: number;
  maxPlayers: number;
  host: string;
  gamePort: number;
  httpPort: number;
  version: string;
  protocol: number;
  featured: boolean;
}

/** Espelha `server_cache::CachedServers` (`savedAt` em segundos Unix). */
export interface CachedServers {
  savedAt: number;
  servers: ServerListing[];
}

/** Espelha `ping::PingResult`: `ms` nulo = sem resposta em 2 s. */
export interface PingResult {
  id: string;
  ms: number | null;
}

/** `GET /v1/servers`; em sucesso o Rust grava `servers-cache.json`. */
export const listServers = (): Promise<ServerListing[]> => call("list_servers");
/** Última lista gravada (Hub fora), ou `null`. */
export const cachedServers = (): Promise<CachedServers | null> => call("cached_servers");
/** Ping por id: o Rust só usa host/porta da lista do Hub. */
export const pingServers = (ids: string[]): Promise<PingResult[]> => call("ping_servers", { ids });

// ----- jogar -----

/** Espelha `play::Stage`, na ordem em que acontecem. */
export type PlayStage = "ticket" | "manifest" | "download" | "mount" | "mod" | "connect" | "launch";

/** Carga de `play://stage`. */
export interface PlayStageEvent {
  serverId: string;
  stage: PlayStage;
}

/** Carga de `content://progress` (`content::Progress`). */
export interface Progress {
  doneBytes: number;
  totalBytes: number;
  bytesPerSec: number;
  current: string;
}

/** Espelha `launch::ConnectPreview`: o arquivo de conexão sem o ticket. */
export interface ConnectPreview {
  host: string;
  port: number;
  serverId: string;
  contentDir: string;
  protocol: number;
}

/** Espelha `launch::LaunchOutcome`. */
export type LaunchOutcome = { kind: "started" } | { kind: "simulated"; preview: ConnectPreview };

export const PLAY_STAGE = "play://stage";
export const CONTENT_PROGRESS = "content://progress";

export const play = (serverId: string): Promise<LaunchOutcome> => call("play", { serverId });
export const cancelPlay = (): Promise<boolean> => call("cancel_play");

export function onPlayStage(cb: (p: PlayStageEvent) => void): Promise<UnlistenFn> {
  return listen<PlayStageEvent>(PLAY_STAGE, (e) => cb(e.payload));
}

export function onProgress(cb: (p: Progress) => void): Promise<UnlistenFn> {
  return listen<Progress>(CONTENT_PROGRESS, (e) => cb(e.payload));
}

// ----- configurações e atualização -----

/** Seletor de pasta do Rust; valida que é o Hogwarts Legacy e grava `gameDir`. Cancelar devolve o atual. */
export const pickGameDir = (): Promise<Settings> => call("pick_game_dir");

/** Espelha `game::GameCandidate`: pasta do jogo achada automaticamente, mas que só vale com confirmação. */
export interface GameCandidate {
  dir: string;
  store: "steam" | "epic" | "manual";
}

/** Espelha `commands::GameDetection`. `ambiguous`: várias pastas parecem ser o jogo; nenhuma é sugerida. */
export interface GameDetection {
  settings: Settings;
  candidate: GameCandidate | null;
  ambiguous: boolean;
}

/**
 * Pasta atual ou procura de novo (Steam/Epic). Uma pasta não confiável volta só como `candidate` (e só se for a única),
 * sem ser gravada.
 */
export const detectGame = (): Promise<GameDetection> => call("detect_game");
/**
 * "Usar esta pasta": o Rust procura de novo, só aceita se achar exatamente `dir` (e só ela), pede confirmação no diálogo do
 * Windows e grava como o seletor. Cancelar no diálogo devolve as configurações sem mudança.
 */
export const useDetectedGame = (dir: string): Promise<Settings> => call("use_detected_game", { dir });
export const clearCache = (): Promise<void> => call("clear_cache");

/** Espelha `update::UpdateStatus`. */
export interface UpdateStatus {
  available: boolean;
  version: string | null;
  notes: string | null;
}

export const UPDATE_AVAILABLE = "update://available";

export const checkUpdate = (): Promise<UpdateStatus> => call("check_update");
/** Baixa, confere a assinatura e instala (o instalador fecha o launcher). */
export const installUpdate = (): Promise<void> => call("install_update");

export function onUpdateAvailable(cb: (s: UpdateStatus) => void): Promise<UnlistenFn> {
  return listen<UpdateStatus>(UPDATE_AVAILABLE, (e) => cb(e.payload));
}

// ----- mod do jogo -----

/** Espelha `modact::ModStatus`. */
export type ModStatus = { state: "notInstalled" } | { state: "installed"; version: string } | { state: "conflict" };

/** Espelha `commands::ModInfo`: estado na pasta do jogo atual + pastas anteriores do jogo onde o mod ainda está. */
export type ModInfo = ModStatus & { otherFolders: string[] };

/** Espelha `modact::FolderRemoval`: `error` nulo = removido dessa pasta. */
export interface FolderRemoval {
  gameDir: string;
  error: { code: string; message: string } | null;
}

/** Espelha `commands::RemoveModResult`. */
export interface RemoveModResult {
  status: ModInfo;
  folders: FolderRemoval[];
}

/** Aviso que não para nada (`AppError`): o mod continua numa pasta anterior do jogo. */
export const MOD_WARNING = "mod://warning";

export const modStatus = (): Promise<ModInfo> => call("mod_status");
/** Remove de todas as pastas do jogo registradas; recusa com o jogo aberto (`game_running`). */
export const removeMod = (): Promise<RemoveModResult> => call("remove_mod");
/** Diálogo nativo do Windows sobre o outro carregador; aceito, o do CB Multiplayer é ativado. */
export const resolveModConflict = (): Promise<ModInfo> => call("resolve_mod_conflict");

/** `Instalar com permissão de administrador (uma vez)`: UAC + modo elevado no Rust; depois instala o mod sem elevação. */
export const modAdminSetup = (): Promise<ModInfo> => call("mod_admin_setup");
/** Abre `Win64\cbmp\logs` no Explorer (o Rust escolhe a pasta). */
export const openModLogs = (): Promise<void> => call("open_mod_logs");

export function onModWarning(cb: (e: { code: string; message: string }) => void): Promise<UnlistenFn> {
  return listen<{ code: string; message: string }>(MOD_WARNING, (e) => cb(e.payload));
}
