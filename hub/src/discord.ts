import { HttpError } from './errors';

export interface DiscordUser { id: string; username: string; global_name: string | null; avatar: string | null }
export interface DiscordClient {
  exchangeCode(code: string, redirectUri: string): Promise<string>;
  getUser(accessToken: string): Promise<DiscordUser>;
}

const API = 'https://discord.com/api/v10';

export function createDiscordClient(clientId: string, clientSecret: string, fetchImpl: typeof fetch = fetch): DiscordClient {
  return {
    async exchangeCode(code, redirectUri) {
      const res = await fetchImpl(`${API}/oauth2/token`, {
        method: 'POST',
        headers: { 'content-type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ client_id: clientId, client_secret: clientSecret, grant_type: 'authorization_code', code, redirect_uri: redirectUri }),
        signal: AbortSignal.timeout(10_000),
      });
      if (!res.ok) throw new HttpError(502, 'discord_error', 'falha ao validar login no Discord');
      const body = (await res.json()) as { access_token?: string };
      if (!body.access_token) throw new HttpError(502, 'discord_error', 'resposta inesperada do Discord');
      return body.access_token;
    },
    async getUser(accessToken) {
      const res = await fetchImpl(`${API}/users/@me`, { headers: { authorization: `Bearer ${accessToken}` }, signal: AbortSignal.timeout(10_000) });
      if (!res.ok) throw new HttpError(502, 'discord_error', 'falha ao ler usuário do Discord');
      return (await res.json()) as DiscordUser;
    },
  };
}
