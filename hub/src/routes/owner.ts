import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { Tag } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { requireUser } from '../auth';
import { ONLINE_WINDOW, type ServerRow, type StatusRow } from '../store';

export const MAX_SERVERS_PER_OWNER = 10;

export const CreateServerBody = z.object({
  name: z.string().trim().min(3).max(64),
  description: z.string().max(500).default(''),
  tags: z.array(Tag).max(10).default([]),
  locale: z.string().regex(/^[a-z]{2}(-[A-Z]{2})?$/).default('pt-BR'),
  useGlobalBans: z.boolean().default(false),
});
export const PatchServerBody = z.object({
  name: z.string().trim().min(3).max(64),
  description: z.string().max(500),
  tags: z.array(Tag).max(10),
  locale: z.string().regex(/^[a-z]{2}(-[A-Z]{2})?$/),
  useGlobalBans: z.boolean(),
}).partial();

export function serverView(server: ServerRow, status: StatusRow | null, now: number) {
  const online = !!status && status.verified && status.lastHeartbeat >= now - ONLINE_WINDOW;
  return {
    id: server.id, name: server.name, description: server.description, tags: server.tags, locale: server.locale,
    useGlobalBans: server.useGlobalBans, createdAt: server.createdAt,
    online, verified: status?.verified ?? false, players: status?.players ?? 0, maxPlayers: status?.maxPlayers ?? 0,
    host: status?.ip ?? null, gamePort: status?.gamePort ?? null, httpPort: status?.httpPort ?? null,
    version: status?.version ?? null, protocol: status?.protocol ?? null, lastHeartbeat: status?.lastHeartbeat ?? null,
  };
}

const IdParams = z.object({ id: z.string().min(1).max(64) });

export async function ownerRoutes(app: FastifyInstance) {
  const { store, now } = app.deps;

  app.post('/v1/owner/servers', async (req, reply) => {
    const user = requireUser(req);
    const body = CreateServerBody.parse(req.body);
    if (store.countServersByOwner(user.id) >= MAX_SERVERS_PER_OWNER) {
      throw new HttpError(409, 'too_many_servers', `limite de ${MAX_SERVERS_PER_OWNER} servidores por conta`);
    }
    const { server, key } = store.createServer(user.id, body, now());
    return reply.status(201).send({ server: serverView(server, null, now()), key });
  });

  app.get('/v1/owner/servers', async (req) => {
    const user = requireUser(req);
    return store.listServersByOwner(user.id).map(({ server, status }) => serverView(server, status, now()));
  });

  app.patch('/v1/owner/servers/:id', async (req) => {
    const user = requireUser(req);
    const { id } = IdParams.parse(req.params);
    const updated = store.updateServer(id, user.id, PatchServerBody.parse(req.body));
    if (!updated) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return serverView(updated, store.getStatus(id), now());
  });

  app.post('/v1/owner/servers/:id/rotate-key', async (req) => {
    const user = requireUser(req);
    const { id } = IdParams.parse(req.params);
    const key = store.rotateKey(id, user.id);
    if (!key) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return { key };
  });

  app.delete('/v1/owner/servers/:id', async (req, reply) => {
    const user = requireUser(req);
    const { id } = IdParams.parse(req.params);
    if (!store.deleteServer(id, user.id)) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return reply.status(204).send();
  });
}
