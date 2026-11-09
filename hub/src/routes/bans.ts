import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { HttpError } from '../errors';
import { bearer, isPlatformAdmin, requireUser } from '../auth';

export async function banRoutes(app: FastifyInstance) {
  const { config, store, now } = app.deps;

  app.get('/v1/bans/global', async (req) => {
    const key = bearer(req, 'sk_');
    if (!key || !store.getServerByKey(key)) throw new HttpError(401, 'bad_key', 'chave do servidor inválida');
    return store.listGlobalBans();
  });

  const requireAdmin = (req: Parameters<typeof requireUser>[0]) => {
    const u = requireUser(req);
    if (!isPlatformAdmin(config, u)) throw new HttpError(403, 'forbidden', 'apenas administradores da plataforma');
    return u;
  };

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
