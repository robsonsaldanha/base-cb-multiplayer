import type { FastifyInstance, FastifyReply } from 'fastify';
import { z } from 'zod';
import { HttpError } from '../errors';
import { SESSION_COOKIE, bearer, isPlatformAdmin, requireUser } from '../auth';
import type { ClientKind, SessionTokens } from '../store';

const ClientQuery = z.object({ client: z.enum(['launcher', 'panel']) });

const tokenResponse = (s: SessionTokens) => ({
  accessToken: s.access, accessExpiresAt: s.accessExpiresAt, refreshToken: s.refresh, refreshExpiresAt: s.refreshExpiresAt,
});

export async function authRoutes(app: FastifyInstance) {
  const { config, store, discord, now } = app.deps;
  const redirectUri = `${config.publicUrl}/v1/auth/discord/callback`;

  function finishLogin(reply: FastifyReply, userId: string, client: ClientKind) {
    if (client === 'panel') {
      const s = store.createSession(userId, 'panel', now());
      reply.setCookie(SESSION_COOKIE, s.access, {
        path: '/', httpOnly: true, sameSite: 'strict', secure: config.publicUrl.startsWith('https://'),
        maxAge: s.accessExpiresAt - now(),
      });
      return reply.redirect('/panel');
    }
    const code = store.createLoginCode(userId, now());
    return reply.redirect(`cbmp://auth?code=${encodeURIComponent(code)}`);
  }

  app.get('/v1/auth/discord/login', async (req, reply) => {
    const { client } = ClientQuery.parse(req.query);
    const url = new URL('https://discord.com/oauth2/authorize');
    url.search = new URLSearchParams({
      client_id: config.discordClientId, redirect_uri: redirectUri, response_type: 'code', scope: 'identify',
      state: store.createOauthState(client, now()), prompt: 'none',
    }).toString();
    return reply.redirect(url.toString());
  });

  app.get('/v1/auth/discord/callback', async (req, reply) => {
    const q = z.object({ code: z.string().min(1), state: z.string().min(1) }).parse(req.query);
    const client = store.consumeOauthState(q.state, now());
    if (!client) throw new HttpError(400, 'bad_state', 'login expirado ou inválido, tente de novo');
    const du = await discord.getUser(await discord.exchangeCode(q.code, redirectUri));
    const user = store.upsertUserFromDiscord({ discordId: du.id, name: du.global_name ?? du.username, avatar: du.avatar }, now());
    return finishLogin(reply, user.id, client);
  });

  if (config.devLogin) {
    app.get('/v1/auth/dev-login', async (req, reply) => {
      const q = ClientQuery.extend({ discordId: z.string().regex(/^\d{1,20}$/), name: z.string().min(1).max(32) }).parse(req.query);
      const user = store.upsertUserFromDiscord({ discordId: q.discordId, name: q.name, avatar: null }, now());
      return finishLogin(reply, user.id, q.client);
    });
  }

  app.post('/v1/auth/exchange', async (req) => {
    const { code } = z.object({ code: z.string().min(1) }).parse(req.body);
    const userId = store.consumeLoginCode(code, now());
    if (!userId) throw new HttpError(400, 'bad_code', 'código de login inválido ou expirado');
    return tokenResponse(store.createSession(userId, 'launcher', now()));
  });

  app.post('/v1/auth/refresh', async (req) => {
    const { refreshToken } = z.object({ refreshToken: z.string().min(1) }).parse(req.body);
    const s = store.refreshSession(refreshToken, now());
    if (!s) throw new HttpError(401, 'bad_refresh', 'sessão expirada, faça login de novo');
    return tokenResponse(s);
  });

  app.post('/v1/auth/logout', async (req, reply) => {
    const token = bearer(req, 'at_') ?? req.cookies[SESSION_COOKIE];
    if (token) store.deleteSessionByAccess(token);
    reply.clearCookie(SESSION_COOKIE, { path: '/' });
    return { ok: true };
  });

  app.get('/v1/me', async (req) => {
    const u = requireUser(req);
    return { id: u.id, discordId: u.discordId, name: u.name, avatar: u.avatar, isPlatformAdmin: isPlatformAdmin(config, u) };
  });
}
