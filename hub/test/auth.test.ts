import { describe, expect, it, vi } from 'vitest';
import { createHash, randomBytes } from 'node:crypto';
import { makeTestApp, loginAs } from './helpers';

type App = Awaited<ReturnType<typeof makeTestApp>>['app'];

const pkce = () => {
  const verifier = randomBytes(32).toString('base64url');
  return { verifier, challenge: createHash('sha256').update(verifier).digest('base64url') };
};

async function startLogin(app: App, client: 'launcher' | 'panel', challenge?: string) {
  const q = new URLSearchParams({ client });
  if (challenge) { q.set('code_challenge', challenge); q.set('code_challenge_method', 'S256'); }
  const res = await app.inject({ method: 'GET', url: `/v1/auth/discord/login?${q}` });
  expect(res.statusCode).toBe(302);
  const loc = new URL(res.headers.location as string);
  expect(loc.origin + loc.pathname).toBe('https://discord.com/oauth2/authorize');
  expect(loc.searchParams.get('scope')).toBe('identify');
  const state = loc.searchParams.get('state')!;
  const cookie = res.cookies.find((c) => c.name === 'cbmp_oauth')!;
  return { state, cookie, redirectUri: loc.searchParams.get('redirect_uri') };
}

const callback = (app: App, code: string, state: string, cookie?: string) =>
  app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=${code}&state=${state}`, cookies: cookie ? { cbmp_oauth: cookie } : {} });

describe('auth', () => {
  it('launcher: OAuth+PKCE → cbmp://auth?code → exchange → /v1/me', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c1', { id: '123', username: 'robson', global_name: 'Robson', avatar: 'av' });
    const { verifier, challenge } = pkce();
    const { state, cookie, redirectUri } = await startLogin(app, 'launcher', challenge);
    expect(redirectUri).toBe('http://localhost:8787/v1/auth/discord/callback');
    const cb = await callback(app, 'c1', state, cookie.value);
    expect(cb.statusCode).toBe(302);
    const deep = new URL(cb.headers.location as string);
    expect(deep.protocol).toBe('cbmp:');
    const code = deep.searchParams.get('code')!;
    const ex = await app.inject({ method: 'POST', url: '/v1/auth/exchange', payload: { code, codeVerifier: verifier } });
    expect(ex.statusCode).toBe(200);
    const tokens = ex.json();
    expect(tokens.accessToken).toMatch(/^at_/);
    expect(tokens.refreshToken).toMatch(/^rt_/);
    const me = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: `Bearer ${tokens.accessToken}` } });
    expect(me.json()).toMatchObject({ discordId: '123', name: 'Robson', avatar: 'av', isPlatformAdmin: false });
    const again = await app.inject({ method: 'POST', url: '/v1/auth/exchange', payload: { code, codeVerifier: verifier } });
    expect(again.json().error).toBe('bad_code');
  });

  it('exchange com verifier errado → bad_code e o código é consumido', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c1', { id: '123', username: 'r', global_name: null, avatar: null });
    const { verifier, challenge } = pkce();
    const { state, cookie } = await startLogin(app, 'launcher', challenge);
    const cb = await callback(app, 'c1', state, cookie.value);
    const code = new URL(cb.headers.location as string).searchParams.get('code')!;
    const bad = await app.inject({ method: 'POST', url: '/v1/auth/exchange', payload: { code, codeVerifier: pkce().verifier } });
    expect(bad.statusCode).toBe(400);
    expect(bad.json().error).toBe('bad_code');
    const good = await app.inject({ method: 'POST', url: '/v1/auth/exchange', payload: { code, codeVerifier: verifier } });
    expect(good.json().error).toBe('bad_code');
  });

  it('exchange sem codeVerifier é 400', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'POST', url: '/v1/auth/exchange', payload: { code: 'x' } });
    expect(res.statusCode).toBe(400);
  });

  it('login do launcher exige code_challenge S256 válido', async () => {
    const { app } = await makeTestApp();
    expect((await app.inject({ method: 'GET', url: '/v1/auth/discord/login?client=launcher' })).statusCode).toBe(400);
    expect((await app.inject({ method: 'GET', url: '/v1/auth/discord/login?client=launcher&code_challenge=curto&code_challenge_method=S256' })).statusCode).toBe(400);
    const c = pkce().challenge;
    expect((await app.inject({ method: 'GET', url: `/v1/auth/discord/login?client=launcher&code_challenge=${c}&code_challenge_method=plain` })).statusCode).toBe(400);
  });

  it('painel: OAuth grava cookie (Lax) e redireciona para /panel; limpa cbmp_oauth', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c2', { id: '999', username: 'adm', global_name: null, avatar: null });
    const { state, cookie: oc } = await startLogin(app, 'panel');
    expect(oc.httpOnly).toBe(true);
    expect(oc.sameSite).toBe('Lax');
    expect(oc.path).toBe('/v1/auth/discord');
    expect(oc.maxAge).toBe(600);
    const cb = await callback(app, 'c2', state, oc.value);
    expect(cb.headers.location).toBe('/panel');
    const cookie = cb.cookies.find((c) => c.name === 'cbmp_session')!;
    expect(cookie.httpOnly).toBe(true);
    expect(cookie.sameSite).toBe('Lax');
    expect(cookie.secure).toBeFalsy();
    const cleared = cb.cookies.find((c) => c.name === 'cbmp_oauth')!;
    expect(cleared.value).toBe('');
    const me = await app.inject({ method: 'GET', url: '/v1/me', cookies: { cbmp_session: cookie.value } });
    expect(me.json()).toMatchObject({ name: 'adm', isPlatformAdmin: true });
  });

  it('cookies são Secure com publicUrl https', async () => {
    const { app, discord } = await makeTestApp({ config: { publicUrl: 'https://hub.example.com' } });
    discord.users.set('c2', { id: '5', username: 'a', global_name: null, avatar: null });
    const { state, cookie: oc } = await startLogin(app, 'panel');
    expect(oc.secure).toBe(true);
    const cb = await callback(app, 'c2', state, oc.value);
    expect(cb.cookies.find((c) => c.name === 'cbmp_session')!.secure).toBe(true);
  });

  it('state sem cookie, com cookie diferente ou forjado → bad_state', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c3', { id: '1', username: 'x', global_name: null, avatar: null });
    const { state, cookie } = await startLogin(app, 'panel');
    const semCookie = await callback(app, 'c3', state);
    expect(semCookie.statusCode).toBe(400);
    expect(semCookie.json().error).toBe('bad_state');
    expect((await callback(app, 'c3', state, 'outro')).json().error).toBe('bad_state');
    expect((await callback(app, 'c3', 'forjado', 'forjado')).json().error).toBe('bad_state');
    // o state legítimo ainda vale uma vez (falhas anteriores não o consumiram)
    expect((await callback(app, 'c3', state, cookie.value)).statusCode).toBe(302);
    expect((await callback(app, 'c3', state, cookie.value)).json().error).toBe('bad_state');
  });

  it('negado no Discord: painel → 400 HTML; launcher → cbmp://auth?error=access_denied', async () => {
    const { app } = await makeTestApp();
    const p = await startLogin(app, 'panel');
    const rp = await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?error=access_denied&state=${p.state}`, cookies: { cbmp_oauth: p.cookie.value } });
    expect(rp.statusCode).toBe(400);
    expect(rp.headers['content-type']).toContain('text/html');
    expect(rp.body).toContain('Login cancelado no Discord');
    const l = await startLogin(app, 'launcher', pkce().challenge);
    const url = `/v1/auth/discord/callback?error=access_denied&state=${l.state}`;
    const rl = await app.inject({ method: 'GET', url, cookies: { cbmp_oauth: l.cookie.value } });
    expect(rl.statusCode).toBe(302);
    expect(rl.headers.location).toBe('cbmp://auth?error=access_denied');
    const again = await app.inject({ method: 'GET', url, cookies: { cbmp_oauth: l.cookie.value } });
    expect(again.json().error).toBe('bad_state');
  });

  it('launcher: state próprio (43 chars base64url) volta no deep link, com code ou error', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c1', { id: '7', username: 'x', global_name: null, avatar: null });
    const mine = randomBytes(32).toString('base64url');
    const go = async (extra: Record<string, string>) => {
      const q = new URLSearchParams({ client: 'launcher', code_challenge: pkce().challenge, code_challenge_method: 'S256', ...extra });
      const res = await app.inject({ method: 'GET', url: `/v1/auth/discord/login?${q}` });
      if (res.statusCode !== 302) return { status: res.statusCode };
      const loc = new URL(res.headers.location as string);
      return { status: 302, state: loc.searchParams.get('state')!, cookie: res.cookies.find((c) => c.name === 'cbmp_oauth')!.value };
    };
    const ok = await go({ state: mine });
    expect(ok.state).not.toBe(mine); // o state OAuth do Hub é outro
    const cb = await callback(app, 'c1', ok.state!, ok.cookie);
    const deep = new URL(cb.headers.location as string);
    expect(deep.searchParams.get('state')).toBe(mine);
    expect(deep.searchParams.get('code')).toBeTruthy();

    const denied = await go({ state: mine });
    const rd = await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?error=access_denied&state=${denied.state}`, cookies: { cbmp_oauth: denied.cookie! } });
    expect(rd.headers.location).toBe(`cbmp://auth?error=access_denied&state=${mine}`);

    for (const bad of ['curto', `${mine}x`, mine.slice(0, 42) + '+']) expect((await go({ state: bad })).status).toBe(400);
  });

  it('dev-login do launcher também devolve o state próprio', async () => {
    const on = await makeTestApp({ config: { devLogin: true } });
    const mine = randomBytes(32).toString('base64url');
    const l = await on.app.inject({ method: 'GET', url: `/v1/auth/dev-login?client=launcher&discordId=42&name=Dev&code_challenge=${pkce().challenge}&code_challenge_method=S256&state=${mine}` });
    expect(new URL(l.headers.location as string).searchParams.get('state')).toBe(mine);
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

  it('logout com refreshToken no corpo revoga a sessão', async () => {
    const { app, store } = await makeTestApp();
    const { tokens } = loginAs(store, '6');
    const out = await app.inject({ method: 'POST', url: '/v1/auth/logout', payload: { refreshToken: tokens.refresh } });
    expect(out.statusCode).toBe(200);
    const me = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: `Bearer ${tokens.access}` } });
    expect(me.statusCode).toBe(401);
    const r = await app.inject({ method: 'POST', url: '/v1/auth/refresh', payload: { refreshToken: tokens.refresh } });
    expect(r.json().error).toBe('bad_refresh');
  });

  it('/v1/me sem sessão é 401 unauthorized', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/v1/me' });
    expect(res.statusCode).toBe(401);
    expect(res.json().error).toBe('unauthorized');
  });

  it('dev-login só existe com devLogin=true; launcher exige code_challenge', async () => {
    const off = await makeTestApp();
    expect((await off.app.inject({ method: 'GET', url: '/v1/auth/dev-login?client=panel&discordId=1&name=A' })).statusCode).toBe(404);
    const on = await makeTestApp({ config: { devLogin: true } });
    const res = await on.app.inject({ method: 'GET', url: '/v1/auth/dev-login?client=panel&discordId=42&name=Dev' });
    expect(res.headers.location).toBe('/panel');
    expect(res.cookies.some((c) => c.name === 'cbmp_session')).toBe(true);
    expect((await on.app.inject({ method: 'GET', url: '/v1/auth/dev-login?client=launcher&discordId=42&name=Dev' })).statusCode).toBe(400);
    const { verifier, challenge } = pkce();
    const l = await on.app.inject({ method: 'GET', url: `/v1/auth/dev-login?client=launcher&discordId=42&name=Dev&code_challenge=${challenge}&code_challenge_method=S256` });
    const code = new URL(l.headers.location as string).searchParams.get('code')!;
    const ex = await on.app.inject({ method: 'POST', url: '/v1/auth/exchange', payload: { code, codeVerifier: verifier } });
    expect(ex.statusCode).toBe(200);
  });
  it('falha do Discord no callback é logada (warn) com a causa, sem o código', async () => {
    const { app, discord } = await makeTestApp();
    const { HttpError } = await import('../src/errors');
    discord.exchangeCode = async () => { throw new HttpError(502, 'discord_error', 'falha ao validar login no Discord', { service: 'discord', endpoint: 'oauth2/token', status: 503 }); };
    const warn = vi.spyOn(app.log, 'warn');
    const { state, cookie } = await startLogin(app, 'panel');
    const res = await callback(app, 'codigo-secreto', state, cookie.value);
    expect(res.statusCode).toBe(502);
    expect(warn).toHaveBeenCalled();
    const logged = JSON.stringify(warn.mock.calls);
    expect(logged).toContain('503');
    expect(logged).not.toContain('codigo-secreto');
  });
});
