import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { HttpError } from '../errors';
import { bearer, requirePlatformAdmin } from '../auth';

export async function banRoutes(app: FastifyInstance) {
  const { config, store, now } = app.deps;

  app.get('/v1/bans/global', async (req) => {
    const key = bearer(req, 'sk_');
    if (!key) throw new HttpError(401, 'no_key', 'informe a chave do servidor');
    const server = store.getServerByKey(key);
    if (!server) throw new HttpError(401, 'bad_key', 'chave do servidor inválida');
    if (!server.useGlobalBans) throw new HttpError(403, 'not_opted_in', 'ative useGlobalBans no servidor para usar a lista global');
    return store.listGlobalBans().map(({ discordId, reason, createdAt }) => ({ discordId, reason, createdAt }));
  });

  const requireAdmin = (req: Parameters<typeof requirePlatformAdmin>[0]) => requirePlatformAdmin(req, config);

  app.post('/v1/admin/bans/global', async (req, reply) => {
    const admin = requireAdmin(req);
    const body = z.object({ discordId: z.string().regex(/^\d{1,20}$/), reason: z.string().min(1).max(200) }).parse(req.body);
    store.addGlobalBan(body.discordId, body.reason, admin.discordId, now());
    return reply.status(201).send({ ok: true });
  });

  app.delete('/v1/admin/bans/global/:discordId', async (req, reply) => {
    requireAdmin(req);
    const { discordId } = z.object({ discordId: z.string().regex(/^\d{1,20}$/) }).parse(req.params);
    if (!store.removeGlobalBan(discordId)) throw new HttpError(404, 'not_found', 'ban não encontrado');
    return reply.status(204).send();
  });
}
