import Fastify, { type FastifyError, type FastifyInstance } from 'fastify';
import cookie from '@fastify/cookie';
import rateLimit from '@fastify/rate-limit';
import { ZodError } from 'zod';
import type { HubConfig } from './config';
import type { Store, UserRow } from './store';
import type { KeyRing } from './keyring';
import type { DiscordClient } from './discord';
import type { ProbeFn } from './probe';
import { HttpError } from './errors';
import { authRoutes } from './routes/auth';
import { ownerRoutes } from './routes/owner';
import { ticketRoutes } from './routes/tickets';
import { serverRoutes } from './routes/servers';
import { banRoutes } from './routes/bans';
import { panelRoutes } from './routes/panel';
import { adminLoginRoutes } from './routes/admin-login';

export interface HubDeps {
  config: HubConfig;
  store: Store;
  keyring: KeyRing;
  discord: DiscordClient;
  probe: ProbeFn;
  /** Relógio em segundos unix (injetável nos testes). */
  now: () => number;
  /** Destino do log (testes capturam a auditoria); ausente = stdout conforme logLevel. */
  logStream?: NodeJS.WritableStream;
}

declare module 'fastify' {
  interface FastifyInstance { deps: HubDeps }
  interface FastifyRequest { cachedUser?: UserRow | null }
}

export async function buildApp(deps: HubDeps): Promise<FastifyInstance> {
  const app = Fastify({
    logger: deps.logStream ? { level: deps.config.logLevel === 'silent' ? 'info' : deps.config.logLevel, stream: deps.logStream }
      : deps.config.logLevel === 'silent' ? false : { level: deps.config.logLevel },
    trustProxy: deps.config.trustProxy,
    bodyLimit: 64 * 1024,
  });
  app.decorate('deps', deps);
  app.decorateRequest('cachedUser', undefined);
  await app.register(cookie);
  await app.register(rateLimit, {
    global: deps.config.rateLimit,
    max: 120,
    timeWindow: '1 minute',
    errorResponseBuilder: () => new HttpError(429, 'rate_limited', 'muitas requisições, aguarde um pouco'),
  });

  app.setErrorHandler((err: FastifyError | Error, req, reply) => {
    if (err instanceof HttpError) {
      // Falhas de serviços externos (5xx) são logadas só com o detalhe seguro (status/tipo), nunca tokens/códigos.
      if (err.status >= 500) req.log.warn({ code: err.code, upstream: err.upstream }, err.message);
      return reply.status(err.status).send({ error: err.code, message: err.message });
    }
    if (err instanceof ZodError) {
      return reply.status(400).send({
        error: 'invalid_body',
        message: err.issues.map((i) => `${i.path.join('.') || '(corpo)'}: ${i.message}`).join('; '),
      });
    }
    const status = (err as FastifyError).statusCode;
    if (status && status >= 400 && status < 500) return reply.status(status).send({ error: 'bad_request', message: err.message });
    req.log.error(err);
    return reply.status(500).send({ error: 'internal', message: 'erro interno' });
  });
  app.setNotFoundHandler((_req, reply) => reply.status(404).send({ error: 'not_found', message: 'rota não encontrada' }));

  app.get('/health', async () => ({ ok: true }));
  await app.register(authRoutes);
  await app.register(ownerRoutes);
  await app.register(ticketRoutes);
  await app.register(serverRoutes);
  await app.register(banRoutes);
  await app.register(panelRoutes);
  await app.register(adminLoginRoutes);
  // Tasks seguintes registram rotas aqui.
  return app;
}
