import { z } from 'zod';

export const PROTOCOL_VERSION = 1;

export const Tag = z.string().min(1).max(24).regex(/^[a-z0-9-]+$/);
const Port = z.number().int().min(1).max(65535);
const Locale = z.string().regex(/^[a-z]{2}(-[A-Z]{2})?$/);

export const HeartbeatBody = z.object({
  name: z.string().min(3).max(64),
  description: z.string().max(500).default(''),
  players: z.number().int().min(0).max(2048),
  maxPlayers: z.number().int().min(1).max(2048),
  gamePort: Port,
  httpPort: Port,
  version: z.string().min(1).max(32),
  protocol: z.number().int().min(1),
  tags: z.array(Tag).max(10).default([]),
  locale: Locale.default('pt-BR'),
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
  host: z.string(),
  gamePort: Port,
  httpPort: Port,
  version: z.string(),
  protocol: z.number().int(),
});
export type ServerListing = z.infer<typeof ServerListing>;

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
