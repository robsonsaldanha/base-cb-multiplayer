import Fastify, { type FastifyError, type FastifyInstance, type FastifyRequest } from 'fastify';
import cookie from '@fastify/cookie';
import rateLimit from '@fastify/rate-limit';
import { ZodError } from 'zod';
import type { HubConfig } from './config';
import type { Store, UserRow } from './store';
import type { KeyRing } from './keyring';
import type { DiscordClient } from './discord';
import type { ProbeFn } from './probe';
import type { GitHubReleases } from './github';
import { HttpError } from './errors';
import { authRoutes } from './routes/auth';
import { ownerRoutes } from './routes/owner';
import { ticketRoutes } from './routes/tickets';
import { serverRoutes } from './routes/servers';
import { banRoutes } from './routes/bans';
import { panelRoutes } from './routes/panel';
import { adminLoginRoutes } from './routes/admin-login';
import { adminServerRoutes } from './routes/admin-servers';
import { newDownloadSlots, releaseRoutes, type DownloadSlots } from './routes/releases';

export interface HubDeps {
  config: HubConfig;
  store: Store;
  keyring: KeyRing;
  discord: DiscordClient;
  probe: ProbeFn;
  /** Releases do GitHub (updates do launcher, mod, downloads); ausente ou sem token → 503 updates_disabled. */
  releases?: GitHubReleases;
  /** Relógio em segundos unix (injetável nos testes). */
  now: () => number;
  /** Destino do log (testes capturam a auditoria); ausente = stdout conforme logLevel. */
  logStream?: NodeJS.WritableStream;
}

declare module 'fastify' {
  interface FastifyInstance { deps: HubDeps; downloads: DownloadSlots }
  interface FastifyRequest { cachedUser?: UserRow | null }
}

/** URL para log: em /v1/auth/* a query (code/state do OAuth, PKCE, ids) nunca vai para o log. */
export function redactUrl(url: string): string {
  const q = url.indexOf('?');
  if (q < 0 || !url.startsWith('/v1/auth/')) return url;
  return `${url.slice(0, q)}?[redigido]`;
}

/** Serializador do pedido no log (o padrão do Fastify grava a URL inteira, com a query). */
function logReq(req: FastifyRequest) {
  return { method: req.method, url: redactUrl(req.url), host: req.headers.host, remoteAddress: req.ip, remotePort: req.socket?.remotePort };
}

export async function buildApp(deps: HubDeps): Promise<FastifyInstance> {
  const app = Fastify({
    logger: deps.logStream ? { level: deps.config.logLevel === 'silent' ? 'info' : deps.config.logLevel, stream: deps.logStream, serializers: { req: logReq } }
      : deps.config.logLevel === 'silent' ? false : { level: deps.config.logLevel, serializers: { req: logReq } },
    trustProxy: deps.config.trustProxy,
    bodyLimit: 64 * 1024,
  });
  app.decorate('deps', deps);
  app.decorate('downloads', newDownloadSlots());
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

  // Hub de teste (HUB_STAGING) se identifica no /health, para monitoração notar um staging fora do lugar.
  app.get('/health', async () => (deps.config.staging ? { ok: true, staging: true } : { ok: true }));
  await app.register(authRoutes);
  await app.register(ownerRoutes);
  await app.register(ticketRoutes);
  await app.register(serverRoutes);
  await app.register(banRoutes);
  await app.register(panelRoutes);
  await app.register(adminLoginRoutes);
  await app.register(adminServerRoutes);
  await app.register(releaseRoutes);
  // Tasks seguintes registram rotas aqui.
  return app;
}
