import type { FastifyRequest } from 'fastify';
import type { HubConfig } from './config';
import type { UserRow } from './store';
import { HttpError } from './errors';

export const SESSION_COOKIE = 'cbmp_session';

export interface AuthOptions {
  /** Aceitar o cookie de sessão do painel. Só o painel, GET /v1/me e POST /v1/auth/logout passam true. */
  allowCookie?: boolean;
}

export function bearer(req: FastifyRequest, prefix: string): string | null {
  const h = req.headers.authorization;
  if (!h?.startsWith('Bearer ')) return null;
  const t = h.slice(7).trim();
  return t.startsWith(prefix) ? t : null;
}

/** Sessão via `Authorization: Bearer at_...`; o cookie do painel só vale com `allowCookie: true`. */
export function currentUser(req: FastifyRequest, opts: AuthOptions = {}): UserRow | null {
  const { store, now } = req.server.deps;
  const fromBearer = bearer(req, 'at_');
  if (fromBearer) {
    if (req.cachedUser === undefined) req.cachedUser = store.findUserByAccess(fromBearer, now());
    return req.cachedUser;
  }
  const cookie = opts.allowCookie ? req.cookies[SESSION_COOKIE] : undefined;
  return cookie ? store.findUserByAccess(cookie, now()) : null;
}

export function requireUser(req: FastifyRequest, opts: AuthOptions = {}): UserRow {
  const u = currentUser(req, opts);
  if (!u) throw new HttpError(401, 'unauthorized', 'faça login');
  return u;
}

export function isPlatformAdmin(config: HubConfig, user: UserRow): boolean {
  return config.platformAdmins.includes(user.discordId);
}

/** Sessão (Bearer) de admin da plataforma: 401 sem login, 403 se não for admin. Mesma guarda dos bans globais. */
export function requirePlatformAdmin(req: FastifyRequest, config: HubConfig): UserRow {
  const u = requireUser(req);
  if (!isPlatformAdmin(config, u)) throw new HttpError(403, 'forbidden', 'apenas administradores da plataforma');
  return u;
}
