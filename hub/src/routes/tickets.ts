import { randomUUID } from 'node:crypto';
import type { FastifyInstance } from 'fastify';
import { TICKET_TTL_SECONDS, TicketRequest, signTicket, type TicketPayload } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { requireUser } from '../auth';

export async function ticketRoutes(app: FastifyInstance) {
  const { store, keyring, now } = app.deps;

  app.get('/v1/keys/public', async () => keyring.publicKeys());

  app.post('/v1/tickets', { config: { rateLimit: { max: 30, timeWindow: '1 minute' } } }, async (req) => {
    const user = requireUser(req);
    const { serverId } = TicketRequest.parse(req.body);
    const server = store.getServer(serverId);
    if (!server) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    if (server.useGlobalBans && store.isGloballyBanned(user.discordId)) {
      throw new HttpError(403, 'banned_global', 'sua conta está na lista global de banimentos');
    }
    const iat = now();
    const payload: TicketPayload = {
      v: 1, kid: keyring.active.kid, sub: user.id, did: user.discordId, name: user.name,
      aud: server.id, iat, exp: iat + TICKET_TTL_SECONDS, jti: randomUUID(),
    };
    return { ticket: signTicket(payload, keyring.active.privateKey), expiresAt: payload.exp };
  });
}
