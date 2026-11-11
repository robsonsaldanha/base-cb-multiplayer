import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { AdminServerPatch } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { requirePlatformAdmin } from '../auth';

/**
 * Administração de servidores pela plataforma.
 *
 * PATCH /v1/admin/servers/:id {featured} — liga/desliga o destaque na lista pública. Só admin da plataforma
 * (HUB_PLATFORM_ADMINS; 401 sem login, 403 caso contrário). Auditado no log do Hub (did do admin, serverId, valor e
 * resultado) com nível próprio, que não some com HUB_LOG_LEVEL=warn/error. A lista pública tem cache de 5 s: a
 * mudança aparece em até 5 s.
 */
export async function adminServerRoutes(app: FastifyInstance) {
  const { config, store } = app.deps;
  const auditLog = app.log.child({ audit: 'platform_admin' }, { level: 'info' });

  app.patch('/v1/admin/servers/:id', async (req) => {
    const admin = requirePlatformAdmin(req, config);
    const { id } = z.object({ id: z.string().min(1).max(64) }).parse(req.params);
    const { featured } = AdminServerPatch.parse(req.body);
    const updated = store.setFeatured(id, featured);
    auditLog.info({ action: 'server_featured', did: admin.discordId, serverId: id, featured, result: updated ? 'ok' : 'not_found' },
      'admin da plataforma: destaque de servidor');
    if (!updated) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return { id: updated.id, featured: updated.featured };
  });
}
