import { createHash } from 'node:crypto';
import type { FastifyInstance, FastifyReply } from 'fastify';
import { z } from 'zod';
import { ExchangeRequest, RefreshRequest, type TokenResponse } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { SESSION_COOKIE, bearer, isPlatformAdmin, requireUser } from '../auth';
import type { ClientKind, ReturnTo, SessionTokens } from '../store';
import { ADMIN_STATE_RE, SERVER_ID_RE, adminConfirmPath } from './admin-login';

const OAUTH_COOKIE = 'cbmp_oauth';
const OAUTH_COOKIE_PATH = '/v1/auth/discord';

const ClientQuery = z.object({
  client: z.enum(['launcher', 'panel']),
  code_challenge: z.string().regex(/^[A-Za-z0-9_-]{43}$/).optional(),
  code_challenge_method: z.literal('S256').optional(),
  /** Só painel: volta para a confirmação de login de admin deste servidor (fica no state OAuth, não em cookie). */
  return: z.string().regex(SERVER_ID_RE).optional(),
  return_state: z.string().regex(ADMIN_STATE_RE).optional(),
  /** Só launcher: state próprio (32 bytes em base64url), devolvido em cbmp://auth para ele conferir. */
  state: z.string().regex(/^[A-Za-z0-9_-]{43}$/).optional(),
});

/** State do launcher para devolver no deep link; o painel não usa. */
const clientStateOf = (q: z.infer<typeof ClientQuery>) => (q.client === 'launcher' ? q.state ?? null : null);

/** Deep link do launcher; o `state` dele (se veio) vai junto. */
function launcherLink(params: Record<string, string>, clientState: string | null): string {
  const q = new URLSearchParams(params);
  if (clientState) q.set('state', clientState);
  return `cbmp://auth?${q}`;
}

/** Retorno pós-login: só no painel; o launcher ignora. */
function returnOf(q: z.infer<typeof ClientQuery>): ReturnTo | undefined {
  if (q.return_state !== undefined && q.return === undefined) throw new HttpError(400, 'invalid_body', 'return_state exige return');
  return q.client === 'panel' && q.return ? { serverId: q.return, state: q.return_state ?? null } : undefined;
}

/** O launcher é cliente público: PKCE (S256) é obrigatório. O painel não usa. */
function checkPkce(q: z.infer<typeof ClientQuery>): string | undefined {
  if (q.client !== 'launcher') return undefined;
  if (!q.code_challenge || q.code_challenge_method !== 'S256') throw new HttpError(400, 'invalid_body', 'code_challenge (S256) é obrigatório para o launcher');
  return q.code_challenge;
}

const escapeHtml = (v: string) => v.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);

const deniedPage = (reason: string) => `<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><title>Login cancelado</title></head>
<body><h1>Login cancelado no Discord</h1><p>Não foi possível concluir o login (${escapeHtml(reason)}).</p><p><a href="/panel">Tentar de novo</a></p></body></html>`;

/**
 * Hub de teste só com dev-login (sem app Discord): o "Entrar" do launcher/painel cai aqui em vez do Discord. Formulário
 * GET para /v1/auth/dev-login que repete os parâmetros do cliente (PKCE e state do launcher; retorno do painel) e pede
 * um Discord ID e nome fictícios. Os valores já passaram pelo zod (regex), e mesmo assim são escapados.
 */
function devLoginPage(q: Record<string, string | undefined>): string {
  const hidden = Object.entries(q)
    .filter((e): e is [string, string] => typeof e[1] === 'string')
    .map(([k, v]) => `<input type="hidden" name="${escapeHtml(k)}" value="${escapeHtml(v)}">`).join('');
  return `<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><title>Entrar (Hub de teste)</title></head>
<body><h1>Hub de teste: entrar sem Discord</h1>
<p>Este Hub roda com <code>HUB_DEV_LOGIN=1</code> e sem app Discord. Escolha uma conta fictícia.</p>
<form method="get" action="/v1/auth/dev-login">${hidden}
<p><label>Discord ID (fictício) <input name="discordId" value="111111111111111111" pattern="[0-9]{1,20}" required></label></p>
<p><label>Nome <input name="name" value="Dev Local" maxlength="32" required></label></p>
<p><button type="submit">Entrar como dev</button></p></form></body></html>`;
}

const tokenResponse = (s: SessionTokens): TokenResponse => ({
  accessToken: s.access, accessExpiresAt: s.accessExpiresAt, refreshToken: s.refresh, refreshExpiresAt: s.refreshExpiresAt,
});

export async function authRoutes(app: FastifyInstance) {
  const { config, store, discord, now } = app.deps;
  const redirectUri = `${config.publicUrl}/v1/auth/discord/callback`;

  const secure = config.publicUrl.startsWith('https://');

  function finishLogin(reply: FastifyReply, userId: string, client: ClientKind, codeChallenge: string | null, returnTo: ReturnTo | null = null, clientState: string | null = null) {
    if (client === 'panel') {
      const s = store.createSession(userId, 'panel', now());
      reply.setCookie(SESSION_COOKIE, s.access, {
        path: '/', httpOnly: true, sameSite: 'lax', secure,
        maxAge: s.accessExpiresAt - now(),
      });
      // Retorno amarrado ao state OAuth (uso único): sempre para a CONFIRMAÇÃO, nunca direto para um ticket.
      return reply.redirect(returnTo ? adminConfirmPath(returnTo.serverId, returnTo.state) : '/panel');
    }
    const code = store.createLoginCode(userId, now(), codeChallenge ?? '');
    return reply.redirect(launcherLink({ code }, clientState));
  }

  app.get('/v1/auth/discord/login', async (req, reply) => {
    const q = ClientQuery.parse(req.query);
    const challenge = checkPkce(q);
    if (config.devLogin && !config.discordClientId) {
      returnOf(q); // mesma validação do caminho normal
      return reply
        // form-action vale também para o redirecionamento depois do envio (Chrome): o do launcher vai para cbmp://.
        .header('content-security-policy', "default-src 'none'; form-action 'self' cbmp:; base-uri 'none'; frame-ancestors 'none'")
        .header('x-frame-options', 'DENY')
        .header('cache-control', 'no-store')
        .type('text/html; charset=utf-8')
        .send(devLoginPage(q));
    }
    const state = store.createOauthState(q.client, now(), challenge, returnOf(q), clientStateOf(q) ?? undefined);
    reply.setCookie(OAUTH_COOKIE, state, { path: OAUTH_COOKIE_PATH, httpOnly: true, sameSite: 'lax', secure, maxAge: 600 });
    const url = new URL('https://discord.com/oauth2/authorize');
    url.search = new URLSearchParams({
      client_id: config.discordClientId, redirect_uri: redirectUri, response_type: 'code', scope: 'identify',
      state, prompt: 'none',
    }).toString();
    return reply.redirect(url.toString());
  });

  app.get('/v1/auth/discord/callback', async (req, reply) => {
    const q = z.object({ code: z.string().min(1).optional(), error: z.string().max(100).optional(), state: z.string().min(1) }).parse(req.query);
    // O state precisa ter sido emitido para ESTE navegador (cookie) — evita login CSRF.
    if (req.cookies[OAUTH_COOKIE] !== q.state) throw new HttpError(400, 'bad_state', 'login expirado ou inválido, tente de novo');
    const st = store.consumeOauthState(q.state, now());
    reply.clearCookie(OAUTH_COOKIE, { path: OAUTH_COOKIE_PATH });
    if (!st) throw new HttpError(400, 'bad_state', 'login expirado ou inválido, tente de novo');
    if (q.error || !q.code) {
      if (st.client === 'launcher') return reply.redirect(launcherLink({ error: 'access_denied' }, st.clientState));
      return reply.status(400).type('text/html; charset=utf-8').send(deniedPage(q.error ?? 'access_denied'));
    }
    const du = await discord.getUser(await discord.exchangeCode(q.code, redirectUri));
    const user = store.upsertUserFromDiscord({ discordId: du.id, name: du.global_name ?? du.username, avatar: du.avatar }, now());
    return finishLogin(reply, user.id, st.client, st.codeChallenge, st.returnTo, st.clientState);
  });

  if (config.devLogin) {
    app.get('/v1/auth/dev-login', async (req, reply) => {
      const q = ClientQuery.extend({ discordId: z.string().regex(/^\d{1,20}$/), name: z.string().min(1).max(32) }).parse(req.query);
      const challenge = checkPkce(q);
      const user = store.upsertUserFromDiscord({ discordId: q.discordId, name: q.name, avatar: null }, now());
      return finishLogin(reply, user.id, q.client, challenge ?? null, returnOf(q) ?? null, clientStateOf(q));
    });
  }

  app.post('/v1/auth/exchange', async (req) => {
    const { code, codeVerifier } = ExchangeRequest.parse(req.body);
    const c = store.consumeLoginCode(code, now()); // consumido mesmo se o verifier falhar
    if (!c || createHash('sha256').update(codeVerifier).digest('base64url') !== c.codeChallenge) {
      throw new HttpError(400, 'bad_code', 'código de login inválido ou expirado');
    }
    return tokenResponse(store.createSession(c.userId, 'launcher', now()));
  });

  app.post('/v1/auth/refresh', async (req) => {
    const { refreshToken } = RefreshRequest.parse(req.body);
    const s = store.refreshSession(refreshToken, now());
    if (!s) throw new HttpError(401, 'bad_refresh', 'sessão expirada, faça login de novo');
    return tokenResponse(s);
  });

  app.post('/v1/auth/logout', async (req, reply) => {
    const token = bearer(req, 'at_') ?? req.cookies[SESSION_COOKIE];
    if (token) store.deleteSessionByAccess(token);
    const body = z.object({ refreshToken: z.string().min(1).optional() }).safeParse(req.body ?? {});
    if (body.success && body.data.refreshToken) store.deleteSessionByRefresh(body.data.refreshToken);
    reply.clearCookie(SESSION_COOKIE, { path: '/' });
    return { ok: true };
  });

  app.get('/v1/me', async (req) => {
    const u = requireUser(req, { allowCookie: true });
    return { id: u.id, discordId: u.discordId, name: u.name, avatar: u.avatar, isPlatformAdmin: isPlatformAdmin(config, u) };
  });
}
