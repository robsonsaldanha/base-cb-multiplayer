import { describe, expect, it } from 'vitest';
import { makeTestApp, loginAs } from './helpers';

async function startLogin(app: Awaited<ReturnType<typeof makeTestApp>>['app'], client: 'launcher' | 'panel') {
  const res = await app.inject({ method: 'GET', url: `/v1/auth/discord/login?client=${client}` });
  expect(res.statusCode).toBe(302);
  const loc = new URL(res.headers.location as string);
  expect(loc.origin + loc.pathname).toBe('https://discord.com/oauth2/authorize');
  expect(loc.searchParams.get('scope')).toBe('identify');
  expect(loc.searchParams.get('redirect_uri')).toBe('http://localhost:8787/v1/auth/discord/callback');
  return loc.searchParams.get('state')!;
}

describe('auth', () => {
  it('launcher: OAuth → cbmp://auth?code → exchange → /v1/me', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c1', { id: '123', username: 'robson', global_name: 'Robson', avatar: 'av' });
    const state = await startLogin(app, 'launcher');
    const cb = await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c1&state=${state}` });
    expect(cb.statusCode).toBe(302);
    const deep = new URL(cb.headers.location as string);
    expect(deep.protocol).toBe('cbmp:');
    const code = deep.searchParams.get('code')!;
    const ex = await app.inject({ method: 'POST', url: '/v1/auth/exchange', payload: { code } });
    expect(ex.statusCode).toBe(200);
    const tokens = ex.json();
    expect(tokens.accessToken).toMatch(/^at_/);
    expect(tokens.refreshToken).toMatch(/^rt_/);
    const me = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: `Bearer ${tokens.accessToken}` } });
    expect(me.json()).toMatchObject({ discordId: '123', name: 'Robson', avatar: 'av', isPlatformAdmin: false });
    const again = await app.inject({ method: 'POST', url: '/v1/auth/exchange', payload: { code } });
    expect(again.json().error).toBe('bad_code');
  });

  it('painel: OAuth grava cookie e redireciona para /panel', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c2', { id: '999', username: 'adm', global_name: null, avatar: null });
    const state = await startLogin(app, 'panel');
    const cb = await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c2&state=${state}` });
    expect(cb.headers.location).toBe('/panel');
    const cookie = cb.cookies.find((c) => c.name === 'cbmp_session')!;
    expect(cookie.httpOnly).toBe(true);
    expect(cookie.sameSite).toBe('Strict');
    const me = await app.inject({ method: 'GET', url: '/v1/me', cookies: { cbmp_session: cookie.value } });
    expect(me.json()).toMatchObject({ name: 'adm', isPlatformAdmin: true });
  });

  it('state inválido ou reutilizado é recusado', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c3', { id: '1', username: 'x', global_name: null, avatar: null });
    const state = await startLogin(app, 'launcher');
    await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c3&state=${state}` });
    const res = await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c3&state=${state}` });
    expect(res.statusCode).toBe(400);
    expect(res.json().error).toBe('bad_state');
  });

  it('client inválido no login é 400', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/v1/auth/discord/login?client=outro' });
    expect(res.statusCode).toBe(400);
  });

  it('refresh gira tokens; logout invalida', async () => {
    const { app, store } = await makeTestApp();
    const { tokens } = loginAs(store, '5');
    const r = await app.inject({ method: 'POST', url: '/v1/auth/refresh', payload: { refreshToken: tokens.refresh } });
    expect(r.statusCode).toBe(200);
    const novo = r.json();
    expect(novo.accessToken).not.toBe(tokens.access);
    const bad = await app.inject({ method: 'POST', url: '/v1/auth/refresh', payload: { refreshToken: tokens.refresh } });
    expect(bad.json().error).toBe('bad_refresh');
    const out = await app.inject({ method: 'POST', url: '/v1/auth/logout', headers: { authorization: `Bearer ${novo.accessToken}` } });
    expect(out.statusCode).toBe(200);
    const me = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: `Bearer ${novo.accessToken}` } });
    expect(me.statusCode).toBe(401);
  });

  it('/v1/me sem sessão é 401 unauthorized', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/v1/me' });
    expect(res.statusCode).toBe(401);
    expect(res.json().error).toBe('unauthorized');
  });

  it('dev-login só existe com devLogin=true', async () => {
    const off = await makeTestApp();
    expect((await off.app.inject({ method: 'GET', url: '/v1/auth/dev-login?client=panel&discordId=1&name=A' })).statusCode).toBe(404);
    const on = await makeTestApp({ config: { devLogin: true } });
    const res = await on.app.inject({ method: 'GET', url: '/v1/auth/dev-login?client=panel&discordId=42&name=Dev' });
    expect(res.headers.location).toBe('/panel');
    expect(res.cookies.some((c) => c.name === 'cbmp_session')).toBe(true);
  });
});
