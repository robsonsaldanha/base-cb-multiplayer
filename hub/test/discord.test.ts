import { describe, expect, it } from 'vitest';
import { createDiscordClient } from '../src/discord';

const json = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json' } });
const client = (f: unknown) => createDiscordClient('cid', 'sec', f as typeof fetch);

describe('createDiscordClient', () => {
  it('sucesso', async () => {
    const c = client(async (url: string) => String(url).endsWith('/token') ? json({ access_token: 'tk' }) : json({ id: '1', username: 'u', global_name: null, avatar: null, extra: 1 }));
    expect(await c.exchangeCode('c', 'http://x')).toBe('tk');
    expect(await c.getUser('tk')).toMatchObject({ id: '1', username: 'u', global_name: null, avatar: null });
  });
  it('resposta não-200 → discord_error 502', async () => {
    const c = client(async () => json({}, 400));
    await expect(c.exchangeCode('c', 'x')).rejects.toMatchObject({ status: 502, code: 'discord_error' });
    await expect(c.getUser('t')).rejects.toMatchObject({ status: 502, code: 'discord_error' });
  });
  it('forma inválida do usuário → discord_error', async () => {
    for (const bad of [{ id: 'abc', username: 'u', global_name: null, avatar: null }, { id: '1', username: 5, global_name: null, avatar: null }, {}]) {
      const c = client(async () => json(bad));
      await expect(c.getUser('t')).rejects.toMatchObject({ status: 502, code: 'discord_error' });
    }
  });
  it('JSON inválido → discord_error', async () => {
    const c = client(async () => new Response('não é json', { status: 200 }));
    await expect(c.getUser('t')).rejects.toMatchObject({ status: 502, code: 'discord_error' });
    await expect(c.exchangeCode('c', 'x')).rejects.toMatchObject({ status: 502, code: 'discord_error' });
  });
  it('erro de rede → discord_error', async () => {
    const c = client(async () => { throw new TypeError('fetch failed'); });
    await expect(c.exchangeCode('c', 'x')).rejects.toMatchObject({ status: 502, code: 'discord_error' });
    await expect(c.getUser('t')).rejects.toMatchObject({ status: 502, code: 'discord_error' });
  });
});
