import type { FastifyRequest } from 'fastify';
import type { HubConfig } from './config';
import type { UserRow } from './store';
import { HttpError } from './errors';

export const SESSION_COOKIE = 'cbmp_session';

export function bearer(req: FastifyRequest, prefix: string): string | null {
  const h = req.headers.authorization;
  if (!h?.startsWith('Bearer ')) return null;
  const t = h.slice(7).trim();
  return t.startsWith(prefix) ? t : null;
}

/** Sessão via `Authorization: Bearer at_...` (launcher) ou cookie (painel). */
export function currentUser(req: FastifyRequest): UserRow | null {
  if (req.cachedUser !== undefined) return req.cachedUser;
  const { store, now } = req.server.deps;
  const token = bearer(req, 'at_') ?? req.cookies[SESSION_COOKIE] ?? null;
  req.cachedUser = token ? store.findUserByAccess(token, now()) : null;
  return req.cachedUser;
}

export function requireUser(req: FastifyRequest): UserRow {
  const u = currentUser(req);
  if (!u) throw new HttpError(401, 'unauthorized', 'faça login');
  return u;
}

export function isPlatformAdmin(config: HubConfig, user: UserRow): boolean {
  return config.platformAdmins.includes(user.discordId);
}
