import { z } from 'zod';

export const PROTOCOL_VERSION = 1;

export const Tag = z.string().min(1).max(24).regex(/^[a-z0-9-]+$/);
const Port = z.number().int().min(1).max(65535);
const Locale = z.string().regex(/^[a-z]{2}(-[A-Z]{2})?$/);

export const ADMIN_URL_MAX = 256;

/** Hosts loopback (127.0.0.0/8, ::1, localhost) já normalizados — únicos que podem usar http no `adminUrl`. */
export function isLoopbackHost(hostname: string): boolean {
  return hostname === 'localhost' || hostname === '[::1]' || /^127\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(hostname);
}

/** IPv4 decimal estrito (4 octetos, sem zero à esquerda, ≤ 255) ou null. */
function strictIpv4(host: string): string | null {
  const parts = host.split('.');
  if (parts.length !== 4) return null;
  for (const p of parts) if (!/^(0|[1-9][0-9]{0,2})$/.test(p) || Number(p) > 255) return null;
  return host;
}

/** Conteúdo entre colchetes → IPv6 canônico (RFC 5952 / WHATWG: minúsculas, maior sequência ≥ 2 de zeros vira ::). */
function canonicalIpv6(inner: string): string | null {
  if (!/^[0-9a-f:]+$/.test(inner)) return null;
  const first = inner.indexOf('::');
  if (first !== inner.lastIndexOf('::')) return null;
  const groupsOf = (s: string) => (s === '' ? [] : s.split(':'));
  let groups: string[];
  if (first >= 0) {
    const head = groupsOf(inner.slice(0, first));
    const tail = groupsOf(inner.slice(first + 2));
    if (head.length + tail.length > 7) return null;
    groups = [...head, ...Array<string>(8 - head.length - tail.length).fill('0'), ...tail];
  } else {
    groups = inner.split(':');
    if (groups.length !== 8) return null;
  }
  if (!groups.every((g) => /^[0-9a-f]{1,4}$/.test(g))) return null;
  const n = groups.map((g) => parseInt(g, 16));
  let bestAt = -1, bestLen = 0;
  for (let i = 0; i < 8;) {
    if (n[i] !== 0) { i++; continue; }
    let j = i;
    while (j < 8 && n[j] === 0) j++;
    if (j - i > bestLen) { bestAt = i; bestLen = j - i; }
    i = j;
  }
  const hex = n.map((v) => v.toString(16));
  if (bestLen < 2) return hex.join(':');
  return `${hex.slice(0, bestAt).join(':')}::${hex.slice(bestAt + bestLen).join(':')}`;
}

/** Nome DNS em ASCII: rótulos 1..63 de [a-z0-9-] sem hífen nas pontas, sem `xn--` (sem IDN: evita homógrafos), ≤ 253. */
function strictHostName(host: string): string | null {
  if (host.length === 0 || host.length > 253) return null;
  const labels = host.split('.');
  const last = labels[labels.length - 1]!;
  // Último rótulo numérico (ou 0x…) = endereço IPv4 para o parser de URL: só a forma decimal estrita.
  if (/^([0-9]+|0x[0-9a-f]*)$/.test(last)) return strictIpv4(host);
  for (const l of labels) {
    if (!/^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/.test(l) || l.startsWith('xn--')) return null;
  }
  return host;
}

/**
 * Normaliza a URL pública do painel admin de um servidor para uma ORIGEM (`scheme://host[:porta]`), ou null se
 * inválida. MESMO algoritmo de `normalizeAdminOrigin` em server/src/config.cpp (vetores compartilhados em
 * test-vectors/admin-origin.json): só ASCII visível, ≤ 256; `http(s)://autoridade` com "/" final opcional (sem
 * caminho, query, fragmento, credenciais); porta 1..65535 (1 a 5 dígitos; padrão removida); IPv4 decimal estrito,
 * IPv6 canônico, nomes DNS estritos sem IDN; https obrigatório, http só em loopback. O Hub só redireciona para
 * `<origem>/auth/callback#…`.
 */
export function normalizeAdminOrigin(raw: string): string | null {
  if (raw.length === 0 || raw.length > ADMIN_URL_MAX || !/^[!-~]+$/.test(raw)) return null;
  const m = /^(https?):\/\/([a-z0-9.\-[\]:]+)\/?$/.exec(raw.toLowerCase());
  if (!m) return null;
  const scheme = m[1]!;
  const authority = m[2]!;
  let host: string | null;
  let port: string | undefined;
  if (authority.startsWith('[')) {
    const close = authority.indexOf(']');
    if (close < 0) return null;
    const v6 = canonicalIpv6(authority.slice(1, close));
    host = v6 === null ? null : `[${v6}]`;
    const rest = authority.slice(close + 1);
    if (rest !== '') {
      if (!rest.startsWith(':')) return null;
      port = rest.slice(1);
    }
  } else {
    const colon = authority.indexOf(':');
    host = strictHostName(colon < 0 ? authority : authority.slice(0, colon));
    if (colon >= 0) port = authority.slice(colon + 1);
  }
  if (host === null) return null;
  let portOut = '';
  if (port !== undefined) {
    if (!/^[0-9]{1,5}$/.test(port)) return null;
    const p = Number(port);
    if (p < 1 || p > 65535) return null;
    if (!((scheme === 'https' && p === 443) || (scheme === 'http' && p === 80))) portOut = `:${p}`;
  }
  if (scheme === 'http' && !isLoopbackHost(host)) return null;
  const out = `${scheme}://${host}${portOut}`;
  // Defesa extra: o parser WHATWG precisa concordar (é ele que o navegador usa no redirecionamento).
  try { if (new URL(out).origin !== out) return null; } catch { return null; }
  return out;
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
  /** Destaque ligado por admin da plataforma (PATCH /v1/admin/servers/:id); a lista vem com os destacados primeiro. */
  featured: z.boolean(),
});
export type ServerListing = z.infer<typeof ServerListing>;

/** Resposta de GET /v1/servers/:id. Offline: só dados básicos; online: tudo que o launcher precisa para conectar. */
export const ServerDetail = z.discriminatedUnion('online', [
  ServerListing.pick({ id: true, name: true, description: true, tags: true, locale: true, featured: true }).extend({ online: z.literal(false) }),
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

/** Corpo de PATCH /v1/admin/servers/:id (só admin da plataforma). */
export const AdminServerPatch = z.object({ featured: z.boolean() }).strict();
export type AdminServerPatch = z.infer<typeof AdminServerPatch>;

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
