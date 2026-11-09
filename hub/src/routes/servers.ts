import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { HeartbeatBody, type ServerDetail, type ServerListing } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { createHash } from 'node:crypto';
import { bearer } from '../auth';
import { isPublicIp, normalizeIp } from '../probe';
import { serverView } from './owner';

export const HEARTBEAT_INTERVAL = 30;
export const PROBE_INTERVAL = 600;
export const LIST_CACHE_SECONDS = 5;
export const PROBE_BACKOFF = 120;

export async function serverRoutes(app: FastifyInstance) {
  const { config, store, probe, now } = app.deps;
  const inflight = new Map<string, Promise<boolean>>();
  let cache: { at: number; body: ServerListing[] } | null = null;

  app.post('/v1/servers/heartbeat', {
    config: { rateLimit: { max: 10, timeWindow: '1 minute', keyGenerator: (req) => {
      const k = bearer(req, 'sk_');
      return k ? 'sk:' + createHash('sha256').update(k).digest('hex') : 'ip:' + req.ip;
    } } },
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
    const sameEndpoint = !!prev && prev.ip === ip && prev.httpPort === hb.httpPort;
    const failedRecently = sameEndpoint && !prev!.verified && prev!.verifiedAt > 0 && t - prev!.verifiedAt < PROBE_BACKOFF;
    if (!config.allowPrivateServers && !isPublicIp(ip)) {
      store.setVerified(server.id, false, t);
      return { ok: true, listed: false, nextHeartbeatSeconds: HEARTBEAT_INTERVAL, reason: 'private_ip' };
    }
    let verified = sameEndpoint ? prev!.verified : false;
    const needsProbe = !sameEndpoint || (!prev!.verified && !failedRecently) || (prev!.verified && t - prev!.verifiedAt >= PROBE_INTERVAL);
    if (needsProbe) {
      const flightKey = `${server.id}|${ip}|${hb.httpPort}`;
      let p = inflight.get(flightKey);
      if (!p) {
        p = probe(ip, hb.httpPort).then((info) => {
          const ok = info?.serverId === server.id;
          store.setVerifiedFor(server.id, ip, hb.httpPort, ok, now());
          return ok;
        }).finally(() => inflight.delete(flightKey));
        inflight.set(flightKey, p);
      }
      verified = await p;
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

  app.get('/v1/servers/:id', async (req): Promise<ServerDetail> => {
    const { id } = z.object({ id: z.string().min(1).max(64) }).parse(req.params);
    const server = store.getServer(id);
    if (!server) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    const status = store.getStatus(id);
    const base = { id: server.id, name: server.name, description: server.description, tags: server.tags, locale: server.locale };
    if (!status || !serverView(server, status, now()).online) return { ...base, online: false };
    return {
      ...base, online: true, players: status.players, maxPlayers: status.maxPlayers, host: status.ip,
      gamePort: status.gamePort, httpPort: status.httpPort, version: status.version, protocol: status.protocol,
    };
  });
}
