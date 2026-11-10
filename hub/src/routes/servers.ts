import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { AdminUrl, HeartbeatBody, type ServerDetail, type ServerListing } from '@cbmp/contracts';
import { isIP } from 'node:net';
import { HttpError } from '../errors';
import { createHash } from 'node:crypto';
import { bearer } from '../auth';
import { isPublicIp, normalizeIp } from '../probe';
import { serverView } from './owner';

export const HEARTBEAT_INTERVAL = 30;
export const PROBE_INTERVAL = 600;
export const LIST_CACHE_SECONDS = 5;
export const PROBE_BACKOFF = 120;

/**
 * Origem do painel admin aceitável em produção: IP literal só se público (mesma lista do probe; IPv4 mapeado em IPv6
 * e loopback/ULA/link-local/não especificado recusados); nomes `localhost`/`*.localhost` recusados. Nomes DNS não são
 * resolvidos (um nome público pode apontar para 127.0.0.1 — só o fragmento vai ao destino escolhido pelo dono).
 */
export function isPublicAdminOrigin(origin: string): boolean {
  const h = new URL(origin).hostname;
  const bare = h.startsWith('[') ? h.slice(1, -1) : h;
  if (isIP(bare)) return !bare.toLowerCase().startsWith('::ffff:') && isPublicIp(bare);
  return h !== 'localhost' && !h.endsWith('.localhost');
}

export async function serverRoutes(app: FastifyInstance) {
  const { config, store, probe, now } = app.deps;
  const inflight = new Map<string, Promise<boolean>>();
  let cache: { at: number; body: ServerListing[] } | null = null;

  app.post('/v1/servers/heartbeat', {
    config: { rateLimit: { max: 10, timeWindow: '1 minute', keyGenerator: (req) => {
      // Só chave válida ganha balde próprio; chaves inventadas caem no balde do IP.
      const k = bearer(req, 'sk_');
      return k && store.getServerByKey(k) ? 'sk:' + createHash('sha256').update(k).digest('hex') : 'ip:' + req.ip;
    } } },
  }, async (req) => {
    const key = bearer(req, 'sk_');
    if (!key) throw new HttpError(401, 'no_key', 'informe a chave do servidor');
    const server = store.getServerByKey(key);
    if (!server) throw new HttpError(401, 'bad_key', 'chave do servidor inválida');
    // adminUrl é opcional e tratada à parte: valor ruim ou proibido é ignorado com aviso, sem derrubar o heartbeat.
    const { adminUrl: rawAdmin, ...rest } = (req.body && typeof req.body === 'object' ? req.body : {}) as Record<string, unknown>;
    const hb = HeartbeatBody.parse(req.body && typeof req.body === 'object' ? rest : req.body);
    let adminUrl: string | null = null;
    let adminUrlIgnored: 'invalid' | 'private_host' | undefined;
    if (rawAdmin !== undefined && rawAdmin !== null) {
      const a = AdminUrl.safeParse(rawAdmin);
      if (!a.success) adminUrlIgnored = 'invalid';
      else if (!config.allowPrivateServers && !isPublicAdminOrigin(a.data)) adminUrlIgnored = 'private_host';
      else adminUrl = a.data;
      if (adminUrlIgnored) req.log.warn({ serverId: server.id, reason: adminUrlIgnored }, 'heartbeat: adminUrl ignorada');
    }
    const extra = adminUrlIgnored ? { adminUrlIgnored } : {};
    const ip = normalizeIp(req.ip);
    const t = now();
    const prev = store.getStatus(server.id);
    store.recordHeartbeat(server.id, hb, ip, t, adminUrl);
    const sameEndpoint = !!prev && prev.ip === ip && prev.httpPort === hb.httpPort;
    const failedRecently = sameEndpoint && !prev!.verified && prev!.verifiedAt > 0 && t - prev!.verifiedAt < PROBE_BACKOFF;
    if (!config.allowPrivateServers && !isPublicIp(ip)) {
      store.setVerified(server.id, false, t);
      return { ok: true, listed: false, nextHeartbeatSeconds: HEARTBEAT_INTERVAL, reason: 'private_ip', ...extra };
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
      ? { ok: true, listed: true, nextHeartbeatSeconds: HEARTBEAT_INTERVAL, ...extra }
      : { ok: true, listed: false, nextHeartbeatSeconds: HEARTBEAT_INTERVAL, reason: 'probe_failed', ...extra };
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
