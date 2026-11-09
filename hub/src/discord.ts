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

const fail = (msg: string) => new HttpError(502, 'discord_error', msg);

export function createDiscordClient(clientId: string, clientSecret: string, fetchImpl: typeof fetch = fetch): DiscordClient {
  /** fetch + JSON, convertendo falha de rede/timeout/status/JSON em 502 discord_error. */
  async function call(url: string, init: RequestInit, msg: string): Promise<unknown> {
    try {
      const res = await fetchImpl(url, { ...init, signal: AbortSignal.timeout(10_000) });
      if (!res.ok) throw fail(msg);
      return await res.json();
    } catch (e) {
      if (e instanceof HttpError) throw e;
      throw fail(msg);
    }
  }
  return {
    async exchangeCode(code, redirectUri) {
      const body = (await call(`${API}/oauth2/token`, {
        method: 'POST',
        headers: { 'content-type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ client_id: clientId, client_secret: clientSecret, grant_type: 'authorization_code', code, redirect_uri: redirectUri }),
      }, 'falha ao validar login no Discord')) as { access_token?: unknown } | null;
      if (!body || typeof body.access_token !== 'string' || !body.access_token) throw fail('resposta inesperada do Discord');
      return body.access_token;
    },
    async getUser(accessToken) {
      const raw = await call(`${API}/users/@me`, { headers: { authorization: `Bearer ${accessToken}` } }, 'falha ao ler usuário do Discord');
      const p = UserSchema.safeParse(raw);
      if (!p.success) throw fail('resposta inesperada do Discord');
      return p.data;
    },
  };
}
