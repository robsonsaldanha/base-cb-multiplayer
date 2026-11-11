import { createHash, randomBytes, randomUUID } from 'node:crypto';
import type { HeartbeatBody } from '@cbmp/contracts';
import type { DB } from './db';

export type ClientKind = 'launcher' | 'panel';
export interface ReturnTo { serverId: string; state: string | null }
export interface UserRow { id: string; discordId: string; name: string; avatar: string | null }
export interface ServerRow {
  id: string; ownerId: string; name: string; description: string; tags: string[]; locale: string;
  useGlobalBans: boolean; createdAt: number;
  /** Destaque na lista pública (só admin da plataforma liga/desliga). */
  featured: boolean;
}
export interface StatusRow {
  serverId: string; ip: string; gamePort: number; httpPort: number; players: number; maxPlayers: number;
  version: string; protocol: number; lastHeartbeat: number; verified: boolean; verifiedAt: number;
  /** Origem normalizada do painel admin do servidor (null = não anunciada). */
  adminUrl: string | null;
}
export interface SessionTokens { access: string; accessExpiresAt: number; refresh: string | null; refreshExpiresAt: number | null }
export interface ServerInput { name: string; description: string; tags: string[]; locale: string; useGlobalBans: boolean }
export type ServerPatch = Partial<ServerInput>;
export interface GlobalBan { discordId: string; reason: string; createdBy: string; createdAt: number }

export const LAUNCHER_ACCESS_TTL = 3600;
export const LAUNCHER_REFRESH_TTL = 30 * 24 * 3600;
export const PANEL_ACCESS_TTL = 12 * 3600;
export const OAUTH_STATE_TTL = 600;
export const LOGIN_CODE_TTL = 60;
export const ONLINE_WINDOW = 90;

const sha256 = (s: string) => createHash('sha256').update(s).digest('hex');
const token = (prefix: string) => prefix + randomBytes(32).toString('base64url');

type Raw = Record<string, unknown>;
const toUser = (r: Raw): UserRow => ({ id: r.id as string, discordId: r.discord_id as string, name: r.name as string, avatar: (r.avatar as string | null) ?? null });
const toServer = (r: Raw): ServerRow => ({
  id: r.id as string, ownerId: r.owner_id as string, name: r.name as string, description: r.description as string,
  tags: JSON.parse(r.tags as string) as string[], locale: r.locale as string, useGlobalBans: r.use_global_bans === 1,
  createdAt: r.created_at as number, featured: r.featured === 1,
});
const toStatus = (r: Raw): StatusRow => ({
  serverId: r.server_id as string, ip: r.ip as string, gamePort: r.game_port as number, httpPort: r.http_port as number,
  players: r.players as number, maxPlayers: r.max_players as number, version: r.version as string,
  protocol: r.protocol as number, lastHeartbeat: r.last_heartbeat as number, verified: r.verified === 1,
  verifiedAt: r.verified_at as number, adminUrl: (r.admin_url as string | null) ?? null,
});

export class Store {
  constructor(private readonly db: DB) {}

  close(): void { this.db.close(); }

  // ---- usuários
  upsertUserFromDiscord(u: { discordId: string; name: string; avatar: string | null }, now: number): UserRow {
    this.db.prepare(`INSERT INTO users (id, discord_id, name, avatar, created_at) VALUES (?, ?, ?, ?, ?)
      ON CONFLICT(discord_id) DO UPDATE SET name = excluded.name, avatar = excluded.avatar`)
      .run(`usr_${randomUUID()}`, u.discordId, u.name, u.avatar, now);
    return toUser(this.db.prepare('SELECT * FROM users WHERE discord_id = ?').get(u.discordId) as Raw);
  }

  getUserById(id: string): UserRow | null {
    const r = this.db.prepare('SELECT * FROM users WHERE id = ?').get(id) as Raw | undefined;
    return r ? toUser(r) : null;
  }

  // ---- sessões
  createSession(userId: string, client: ClientKind, now: number): SessionTokens {
    const access = token('at_');
    const refresh = client === 'launcher' ? token('rt_') : null;
    const accessExpiresAt = now + (client === 'launcher' ? LAUNCHER_ACCESS_TTL : PANEL_ACCESS_TTL);
    const refreshExpiresAt = refresh ? now + LAUNCHER_REFRESH_TTL : null;
    this.db.prepare(`INSERT INTO sessions (id, user_id, client, access_hash, access_exp, refresh_hash, refresh_exp, created_at)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?)`)
      .run(randomUUID(), userId, client, sha256(access), accessExpiresAt, refresh ? sha256(refresh) : null, refreshExpiresAt, now);
    return { access, accessExpiresAt, refresh, refreshExpiresAt };
  }

  findUserByAccess(access: string, now: number): UserRow | null {
    const r = this.db.prepare(`SELECT u.* FROM sessions s JOIN users u ON u.id = s.user_id
      WHERE s.access_hash = ? AND s.access_exp >= ?`).get(sha256(access), now) as Raw | undefined;
    return r ? toUser(r) : null;
  }

  /** Gira access + refresh. O refresh usado deixa de valer. */
  refreshSession(refresh: string, now: number): SessionTokens | null {
    const s = this.db.prepare(`SELECT id, user_id FROM sessions WHERE refresh_hash = ? AND refresh_exp >= ?`)
      .get(sha256(refresh), now) as Raw | undefined;
    if (!s) return null;
    this.db.prepare('DELETE FROM sessions WHERE id = ?').run(s.id);
    return this.createSession(s.user_id as string, 'launcher', now);
  }

  deleteSessionByRefresh(refresh: string): void {
    this.db.prepare('DELETE FROM sessions WHERE refresh_hash = ?').run(sha256(refresh));
  }

  deleteSessionByAccess(access: string): void {
    this.db.prepare('DELETE FROM sessions WHERE access_hash = ?').run(sha256(access));
  }

  /** Apaga states/códigos vencidos e sessões cujo access e refresh (se houver) já expiraram. */
  purgeExpired(now: number): void {
    this.db.transaction(() => {
      this.db.prepare('DELETE FROM oauth_states WHERE created_at < ?').run(now - OAUTH_STATE_TTL);
      this.db.prepare('DELETE FROM login_codes WHERE created_at < ?').run(now - LOGIN_CODE_TTL);
      this.db.prepare('DELETE FROM sessions WHERE access_exp < ? AND (refresh_exp IS NULL OR refresh_exp < ?)').run(now, now);
    })();
  }

  // ---- OAuth state e códigos de login (uso único)
  /** `returnTo` (só painel): volta para a confirmação de login de admin daquele servidor; morre com o state. */
  /** `clientState` (só launcher): state do próprio launcher, devolvido no deep link. */
  createOauthState(client: ClientKind, now: number, codeChallenge?: string, returnTo?: ReturnTo, clientState?: string): string {
    const state = randomBytes(24).toString('base64url');
    this.db.prepare('INSERT INTO oauth_states (state, client, created_at, code_challenge, return_server_id, return_state, client_state) VALUES (?, ?, ?, ?, ?, ?, ?)')
      .run(state, client, now, codeChallenge ?? null, returnTo?.serverId ?? null, returnTo?.state ?? null, clientState ?? null);
    return state;
  }

  consumeOauthState(state: string, now: number): { client: ClientKind; codeChallenge: string | null; returnTo: ReturnTo | null; clientState: string | null } | null {
    const r = this.db.prepare('DELETE FROM oauth_states WHERE state = ? RETURNING client, created_at, code_challenge, return_server_id, return_state, client_state').get(state) as Raw | undefined;
    if (!r || now - (r.created_at as number) > OAUTH_STATE_TTL) return null;
    const sid = (r.return_server_id as string | null) ?? null;
    return {
      client: r.client as ClientKind, codeChallenge: (r.code_challenge as string | null) ?? null,
      returnTo: sid ? { serverId: sid, state: (r.return_state as string | null) ?? null } : null,
      clientState: (r.client_state as string | null) ?? null,
    };
  }

  createLoginCode(userId: string, now: number, codeChallenge: string): string {
    const code = randomBytes(24).toString('base64url');
    this.db.prepare('INSERT INTO login_codes (code_hash, user_id, created_at, code_challenge) VALUES (?, ?, ?, ?)').run(sha256(code), userId, now, codeChallenge);
    return code;
  }

  consumeLoginCode(code: string, now: number): { userId: string; codeChallenge: string } | null {
    const r = this.db.prepare('DELETE FROM login_codes WHERE code_hash = ? RETURNING user_id, created_at, code_challenge').get(sha256(code)) as Raw | undefined;
    if (!r || now - (r.created_at as number) > LOGIN_CODE_TTL) return null;
    return { userId: r.user_id as string, codeChallenge: (r.code_challenge as string | null) ?? '' };
  }

  // ---- servidores
  createServer(ownerId: string, input: ServerInput, now: number): { server: ServerRow; key: string } {
    const id = `srv_${randomBytes(9).toString('base64url')}`;
    const key = token('sk_');
    this.db.prepare(`INSERT INTO servers (id, owner_id, key_hash, name, description, tags, locale, use_global_bans, created_at)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`)
      .run(id, ownerId, sha256(key), input.name, input.description, JSON.stringify(input.tags), input.locale, input.useGlobalBans ? 1 : 0, now);
    return { server: this.getServer(id)!, key };
  }

  getServer(id: string): ServerRow | null {
    const r = this.db.prepare('SELECT * FROM servers WHERE id = ?').get(id) as Raw | undefined;
    return r ? toServer(r) : null;
  }

  getServerByKey(key: string): ServerRow | null {
    const r = this.db.prepare('SELECT * FROM servers WHERE key_hash = ?').get(sha256(key)) as Raw | undefined;
    return r ? toServer(r) : null;
  }

  countServersByOwner(ownerId: string): number {
    return (this.db.prepare('SELECT COUNT(*) AS n FROM servers WHERE owner_id = ?').get(ownerId) as { n: number }).n;
  }

  listServersByOwner(ownerId: string): { server: ServerRow; status: StatusRow | null }[] {
    const rows = this.db.prepare('SELECT * FROM servers WHERE owner_id = ? ORDER BY created_at').all(ownerId) as Raw[];
    return rows.map((r) => ({ server: toServer(r), status: this.getStatus(r.id as string) }));
  }

  updateServer(id: string, ownerId: string, patch: ServerPatch): ServerRow | null {
    const cur = this.getServer(id);
    if (!cur || cur.ownerId !== ownerId) return null;
    const defined = Object.fromEntries(Object.entries(patch).filter(([, v]) => v !== undefined));
    const next = { ...cur, ...defined };
    this.db.prepare('UPDATE servers SET name = ?, description = ?, tags = ?, locale = ?, use_global_bans = ? WHERE id = ?')
      .run(next.name, next.description, JSON.stringify(next.tags), next.locale, next.useGlobalBans ? 1 : 0, id);
    return this.getServer(id);
  }

  rotateKey(id: string, ownerId: string): string | null {
    const key = token('sk_');
    const res = this.db.prepare('UPDATE servers SET key_hash = ? WHERE id = ? AND owner_id = ?').run(sha256(key), id, ownerId);
    return res.changes === 1 ? key : null;
  }

  /** Liga/desliga o destaque (admin da plataforma); null se o servidor não existe. */
  setFeatured(id: string, featured: boolean): ServerRow | null {
    const res = this.db.prepare('UPDATE servers SET featured = ? WHERE id = ?').run(featured ? 1 : 0, id);
    return res.changes === 1 ? this.getServer(id) : null;
  }

  deleteServer(id: string, ownerId: string): boolean {
    return this.db.prepare('DELETE FROM servers WHERE id = ? AND owner_id = ?').run(id, ownerId).changes === 1;
  }

  // ---- status / heartbeat
  getStatus(serverId: string): StatusRow | null {
    const r = this.db.prepare('SELECT * FROM server_status WHERE server_id = ?').get(serverId) as Raw | undefined;
    return r ? toStatus(r) : null;
  }

  /**
   * Grava status e atualiza nome/descrição/tags/locale (server.cfg é a fonte da verdade).
   * `adminUrl` já vem normalizada (origem) e filtrada pela rota; ausente apaga a anterior.
   */
  recordHeartbeat(serverId: string, hb: HeartbeatBody, ip: string, now: number, adminUrl: string | null = null): void {
    this.db.transaction(() => {
      this.db.prepare(`INSERT INTO server_status (server_id, ip, game_port, http_port, players, max_players, version, protocol, last_heartbeat, admin_url)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(server_id) DO UPDATE SET ip = excluded.ip, game_port = excluded.game_port, http_port = excluded.http_port,
          players = excluded.players, max_players = excluded.max_players, version = excluded.version,
          protocol = excluded.protocol, last_heartbeat = excluded.last_heartbeat, admin_url = excluded.admin_url,
          verified = CASE WHEN server_status.ip != excluded.ip OR server_status.http_port != excluded.http_port THEN 0 ELSE server_status.verified END`)
        .run(serverId, ip, hb.gamePort, hb.httpPort, hb.players, hb.maxPlayers, hb.version, hb.protocol, now, adminUrl);
      this.db.prepare('UPDATE servers SET name = ?, description = ?, tags = ?, locale = ? WHERE id = ?')
        .run(hb.name, hb.description, JSON.stringify(hb.tags), hb.locale, serverId);
    })();
  }

  setVerified(serverId: string, verified: boolean, now: number): void {
    this.db.prepare('UPDATE server_status SET verified = ?, verified_at = ? WHERE server_id = ?').run(verified ? 1 : 0, now, serverId);
  }

  /** Resultado de probe: só vale se o endpoint ainda for o sondado. */
  setVerifiedFor(serverId: string, ip: string, httpPort: number, verified: boolean, now: number): void {
    this.db.prepare('UPDATE server_status SET verified = ?, verified_at = ? WHERE server_id = ? AND ip = ? AND http_port = ?')
      .run(verified ? 1 : 0, now, serverId, ip, httpPort);
  }

  listOnline(now: number): { server: ServerRow; status: StatusRow }[] {
    const rows = this.db.prepare(`SELECT s.*, st.* FROM servers s JOIN server_status st ON st.server_id = s.id
      WHERE st.verified = 1 AND st.last_heartbeat >= ? ORDER BY s.featured DESC, st.players DESC, s.name`).all(now - ONLINE_WINDOW) as Raw[];
    return rows.map((r) => ({ server: toServer(r), status: toStatus(r) }));
  }

  // ---- bans globais
  addGlobalBan(discordId: string, reason: string, createdBy: string, now: number): void {
    this.db.prepare(`INSERT INTO global_bans (discord_id, reason, created_by, created_at) VALUES (?, ?, ?, ?)
      ON CONFLICT(discord_id) DO UPDATE SET reason = excluded.reason, created_by = excluded.created_by, created_at = excluded.created_at`)
      .run(discordId, reason, createdBy, now);
  }

  removeGlobalBan(discordId: string): boolean {
    return this.db.prepare('DELETE FROM global_bans WHERE discord_id = ?').run(discordId).changes === 1;
  }

  isGloballyBanned(discordId: string): boolean {
    return this.db.prepare('SELECT 1 FROM global_bans WHERE discord_id = ?').get(discordId) !== undefined;
  }

  listGlobalBans(): GlobalBan[] {
    return (this.db.prepare('SELECT * FROM global_bans ORDER BY created_at').all() as Raw[]).map((r) => ({
      discordId: r.discord_id as string, reason: r.reason as string, createdBy: r.created_by as string, createdAt: r.created_at as number,
    }));
  }
}
