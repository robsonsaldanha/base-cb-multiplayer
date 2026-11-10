import { z } from 'zod';

export const PROTOCOL_VERSION = 1;

export const Tag = z.string().min(1).max(24).regex(/^[a-z0-9-]+$/);
const Port = z.number().int().min(1).max(65535);
const Locale = z.string().regex(/^[a-z]{2}(-[A-Z]{2})?$/);

export const ADMIN_URL_MAX = 256;

/** Hosts loopback (127.0.0.0/8, ::1, localhost) — únicos que podem usar http no `adminUrl`. */
export function isLoopbackHost(hostname: string): boolean {
  return hostname === 'localhost' || hostname === '[::1]' || /^127\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(hostname);
}

/**
 * Normaliza a URL pública do painel admin de um servidor para uma ORIGEM (`scheme://host[:porta]`), ou null se
 * inválida. Recusa caminho (exceto "/"), query, fragmento, credenciais, espaços/controles, não-ASCII (use punycode),
 * mais de 256 chars; https obrigatório, exceto loopback. O Hub só redireciona para `<origem>/auth/callback#ticket=`.
 */
export function normalizeAdminOrigin(raw: string): string | null {
  if (raw.length === 0 || raw.length > ADMIN_URL_MAX) return null;
  // Só ASCII visível; sem @ (credenciais), \ (vira / no parser), ? e # (query/fragmento); no máximo "/" final.
  const m = /^(https?):\/\/([A-Za-z0-9.\-[\]:]+)\/?$/i.exec(raw);
  if (!m) return null;
  let u: URL;
  try { u = new URL(raw); } catch { return null; }
  if (u.username || u.password || u.search || u.hash || u.pathname !== '/' || !u.hostname) return null;
  if (u.protocol !== 'https:' && !(u.protocol === 'http:' && isLoopbackHost(u.hostname))) return null;
  // A porta explícita precisa ser 1..65535 (o parser aceita 0).
  const port = /:(\d+)$/.exec(m[2]!)?.[1];
  if (port !== undefined && !m[2]!.endsWith(']') && (Number(port) < 1 || Number(port) > 65535)) return null;
  return u.origin;
}

export const AdminUrl = z.string().transform((v, ctx) => {
  const o = normalizeAdminOrigin(v);
  if (o === null) {
    ctx.addIssue({ code: z.ZodIssueCode.custom, message: 'adminUrl deve ser uma origem https://host[:porta] (http só em loopback), sem caminho, query, fragmento ou credenciais' });
    return z.NEVER;
  }
  return o;
});

export const HeartbeatBody = z.object({
  name: z.string().trim().min(3).max(64),
  description: z.string().max(500).default(''),
  players: z.number().int().min(0).max(2048),
  maxPlayers: z.number().int().min(1).max(2048),
  gamePort: Port,
  httpPort: Port,
  version: z.string().min(1).max(32),
  protocol: z.number().int().min(1),
  tags: z.array(Tag).max(10).default([]),
  locale: Locale.default('pt-BR'),
  /** Origem pública do painel admin do servidor (opcional). Normalizada; ausente = servidor sem login de admin via Hub. */
  adminUrl: AdminUrl.optional(),
}).refine((b) => b.players <= b.maxPlayers, { message: 'players maior que maxPlayers', path: ['players'] });
export type HeartbeatBody = z.infer<typeof HeartbeatBody>;

export const ServerListing = z.object({
  id: z.string(),
  name: z.string(),
  description: z.string(),
  tags: z.array(z.string()),
  locale: z.string(),
  players: z.number().int(),
  maxPlayers: z.number().int(),
  /** IP de origem do heartbeat. Pode ser IPv6 (ex.: 2001:db8::1): o launcher deve usar colchetes ao montar host:porta ([2001:db8::1]:30500). */
  host: z.string(),
  gamePort: Port,
  httpPort: Port,
  version: z.string(),
  protocol: z.number().int(),
});
export type ServerListing = z.infer<typeof ServerListing>;

/** Resposta de GET /v1/servers/:id. Offline: só dados básicos; online: tudo que o launcher precisa para conectar. */
export const ServerDetail = z.discriminatedUnion('online', [
  ServerListing.pick({ id: true, name: true, description: true, tags: true, locale: true }).extend({ online: z.literal(false) }),
  ServerListing.extend({ online: z.literal(true) }),
]);
export type ServerDetail = z.infer<typeof ServerDetail>;

/** Resposta de GET /info do cb-server. */
export const ServerInfo = z.object({
  serverId: z.string(),
  name: z.string(),
  players: z.number().int(),
  maxPlayers: z.number().int(),
  protocol: z.number().int(),
  version: z.string(),
});
export type ServerInfo = z.infer<typeof ServerInfo>;

export const ManifestFile = z.object({
  path: z.string().min(1),
  sha256: z.string().regex(/^[0-9a-f]{64}$/),
  size: z.number().int().min(0),
});
export const ResourceManifest = z.object({
  serverId: z.string(),
  protocol: z.number().int(),
  resources: z.array(z.object({ name: z.string(), version: z.string(), files: z.array(ManifestFile) })),
  totalSize: z.number().int().min(0),
});
export type ResourceManifest = z.infer<typeof ResourceManifest>;

export const ErrorBody = z.object({ error: z.string(), message: z.string() });
export type ErrorBody = z.infer<typeof ErrorBody>;

export const TicketRequest = z.object({ serverId: z.string().min(1).max(64) });
export const TicketResponse = z.object({ ticket: z.string(), expiresAt: z.number().int() });
export const PublicKeysResponse = z.array(z.object({ kid: z.string(), publicKey: z.string() }));
export type PublicKeysResponse = z.infer<typeof PublicKeysResponse>;

/** POST /v1/auth/exchange: troca o código do deep link cbmp://auth?code=… pelos tokens (PKCE S256). */
export const ExchangeRequest = z.object({ code: z.string().min(1), codeVerifier: z.string().regex(/^[A-Za-z0-9._~-]{43,128}$/) });
export type ExchangeRequest = z.infer<typeof ExchangeRequest>;
/** POST /v1/auth/refresh. */
export const RefreshRequest = z.object({ refreshToken: z.string().min(1) });
export type RefreshRequest = z.infer<typeof RefreshRequest>;
/** Resposta de /v1/auth/exchange e /v1/auth/refresh (unix seconds). */
export const TokenResponse = z.object({
  accessToken: z.string(),
  accessExpiresAt: z.number().int(),
  refreshToken: z.string().nullable(),
  refreshExpiresAt: z.number().int().nullable(),
});
export type TokenResponse = z.infer<typeof TokenResponse>;
