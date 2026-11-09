import { z } from 'zod';
import { HttpError } from './errors';

export interface DiscordUser { id: string; username: string; global_name: string | null; avatar: string | null }
export interface DiscordClient {
  exchangeCode(code: string, redirectUri: string): Promise<string>;
  getUser(accessToken: string): Promise<DiscordUser>;
}

const API = 'https://discord.com/api/v10';

const UserSchema = z.object({
  id: z.string().regex(/^\d{1,20}$/),
  username: z.string(),
  global_name: z.string().nullable().optional().transform((v) => v ?? null),
  avatar: z.string().nullable().optional().transform((v) => v ?? null),
});

const fail = (msg: string, endpoint: string, detail: { status?: number; error?: string } = {}) =>
  new HttpError(502, 'discord_error', msg, { service: 'discord', endpoint, ...detail });

export function createDiscordClient(clientId: string, clientSecret: string, fetchImpl: typeof fetch = fetch): DiscordClient {
  /** fetch + JSON, convertendo falha de rede/timeout/status/JSON em 502 discord_error. */
  // Só status HTTP e nome do erro vão para `upstream` (logado): nada de URL com query, corpo, código ou token.
  async function call(endpoint: string, init: RequestInit, msg: string): Promise<unknown> {
    let status: number | undefined;
    try {
      const res = await fetchImpl(`${API}/${endpoint}`, { ...init, signal: AbortSignal.timeout(10_000) });
      status = res.status;
      if (!res.ok) throw fail(msg, endpoint, { status });
      return await res.json();
    } catch (e) {
      if (e instanceof HttpError) throw e;
      throw fail(msg, endpoint, { status, error: e instanceof Error ? e.name : 'unknown' });
    }
  }
  return {
    async exchangeCode(code, redirectUri) {
      const body = (await call('oauth2/token', {
        method: 'POST',
        headers: { 'content-type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ client_id: clientId, client_secret: clientSecret, grant_type: 'authorization_code', code, redirect_uri: redirectUri }),
      }, 'falha ao validar login no Discord')) as { access_token?: unknown } | null;
      if (!body || typeof body.access_token !== 'string' || !body.access_token) throw fail('resposta inesperada do Discord', 'oauth2/token', { error: 'bad_shape' });
      return body.access_token;
    },
    async getUser(accessToken) {
      const raw = await call('users/@me', { headers: { authorization: `Bearer ${accessToken}` } }, 'falha ao ler usuário do Discord');
      const p = UserSchema.safeParse(raw);
      if (!p.success) throw fail('resposta inesperada do Discord', 'users/@me', { error: 'bad_shape' });
      return p.data;
    },
  };
}
