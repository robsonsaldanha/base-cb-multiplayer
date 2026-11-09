import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { HeartbeatBody, type ServerListing } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { bearer } from '../auth';
import { normalizeIp } from '../probe';
import { serverView } from './owner';

export const HEARTBEAT_INTERVAL = 30;
export const PROBE_INTERVAL = 600;
export const LIST_CACHE_SECONDS = 5;

export async function serverRoutes(app: FastifyInstance) {
  const { store, probe, now } = app.deps;
  let cache: { at: number; body: ServerListing[] } | null = null;

  app.post('/v1/servers/heartbeat', {
    config: { rateLimit: { max: 10, timeWindow: '1 minute', keyGenerator: (req) => req.headers.authorization ?? req.ip } },
  }, async (req) => {
    const key = bearer(req, 'sk_');
    if (!key) throw new HttpError(401, 'no_key', 'informe a chave do servidor');
    const server = store.getServerByKey(key);
    if (!server) throw new HttpError(401, 'bad_key', 'chave do servidor inválida');
    const hb = HeartbeatBody.parse(req.body);
    const ip = normalizeIp(req.ip);
    const t = now();
    const prev = store.getStatus(server.id);
    store.recordHeartbeat(server.id, hb, ip, t);
    let verified = prev?.verified ?? false;
    const needsProbe = !prev || !prev.verified || prev.ip !== ip || prev.httpPort !== hb.httpPort || t - prev.verifiedAt >= PROBE_INTERVAL;
    if (needsProbe) {
      const info = await probe(ip, hb.httpPort);
      verified = info?.serverId === server.id;
      store.setVerified(server.id, verified, t);
    }
    return verified
      ? { ok: true, listed: true, nextHeartbeatSeconds: HEARTBEAT_INTERVAL }
      : { ok: true, listed: false, nextHeartbeatSeconds: HEARTBEAT_INTERVAL, reason: 'probe_failed' };
  });

  app.get('/v1/servers', async () => {
    const t = now();
    if (cache && t - cache.at < LIST_CACHE_SECONDS) return cache.body;
    const body: ServerListing[] = store.listOnline(t).map(({ server, status }) => ({
      id: server.id, name: server.name, description: server.description, tags: server.tags, locale: server.locale,
      players: status.players, maxPlayers: status.maxPlayers, host: status.ip, gamePort: status.gamePort,
      httpPort: status.httpPort, version: status.version, protocol: status.protocol,
    }));
    cache = { at: t, body };
    return body;
  });

  app.get('/v1/servers/:id', async (req) => {
    const { id } = z.object({ id: z.string().min(1).max(64) }).parse(req.params);
    const server = store.getServer(id);
    if (!server) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    const v = serverView(server, store.getStatus(id), now());
    return {
      id: v.id, name: v.name, description: v.description, tags: v.tags, locale: v.locale, online: v.online,
      ...(v.online ? { players: v.players, maxPlayers: v.maxPlayers, host: v.host, gamePort: v.gamePort, httpPort: v.httpPort, version: v.version } : {}),
    };
  });
}
