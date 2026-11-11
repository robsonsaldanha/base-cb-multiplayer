import { describe, expect, it } from 'vitest';
import { loadConfig } from '../src/config';
import { openDb } from '../src/db';
import { Store } from '../src/store';
import { ensureSystemServer, SYSTEM_DISCORD_ID } from '../src/admin-tool';
import { makeTestApp, T0 } from './helpers';

const stg = { HUB_STAGING: '1', HUB_PUBLIC_URL: 'https://teste.exemplo.com.br', HUB_TRUST_PROXY: '127.0.0.1' };

describe('HUB_STAGING (Hub de teste público)', () => {
  it('sobe sem o app Discord e aceita HUB_RELEASES_DIR sem dev-login', () => {
    const c = loadConfig({ ...stg, HUB_RELEASES_DIR: '/srv/releases' });
    expect(c.staging).toBe(true);
    expect(c.devLogin).toBe(false);
    expect(c.discordClientId).toBe('');
    expect(c.releasesDir).not.toBe('');
  });
  it('sem staging, continua exigindo Discord e dev-login para HUB_RELEASES_DIR', () => {
    expect(() => loadConfig({ HUB_PUBLIC_URL: 'https://x.exemplo' })).toThrow(/DISCORD_CLIENT_ID/);
    expect(() => loadConfig({ DISCORD_CLIENT_ID: 'c', DISCORD_CLIENT_SECRET: 's', HUB_RELEASES_DIR: '/srv/r' })).toThrow(/HUB_RELEASES_DIR/);
  });
  it('recusado com dev-login, NODE_ENV=production ou URL pública sem https', () => {
    expect(() => loadConfig({ ...stg, HUB_DEV_LOGIN: '1', HUB_PUBLIC_URL: 'http://localhost:8787', HUB_TRUST_PROXY: '' })).toThrow(/HUB_STAGING/);
    expect(() => loadConfig({ ...stg, NODE_ENV: 'production' })).toThrow(/HUB_STAGING/);
    expect(() => loadConfig({ ...stg, HUB_PUBLIC_URL: 'http://teste.exemplo.com.br' })).toThrow(/HUB_STAGING.*https/);
  });
  it('HUB_RELEASES_DIR continua exclusivo com HUB_GITHUB_TOKEN no staging', () => {
    expect(() => loadConfig({ ...stg, HUB_RELEASES_DIR: '/srv/r', HUB_GITHUB_TOKEN: 't', HUB_RELEASES_REPO: 'a/b' })).toThrow(/mutuamente exclusivos/);
  });
});

describe('login sem app Discord configurado', () => {
  it('sem client id: /v1/auth/discord/login responde 503 login_unavailable', async () => {
    const { app } = await makeTestApp({ config: { staging: true, discordClientId: '', discordClientSecret: '' } });
    const r = await app.inject({ method: 'GET', url: '/v1/auth/discord/login?client=panel' });
    expect(r.statusCode).toBe(503);
    expect(r.json()).toMatchObject({ error: 'login_unavailable' });
    expect(r.json().message).toMatch(/DISCORD_CLIENT_ID/);
  });
  it('com client id e sem secret: redireciona ao Discord, e o callback responde 503 antes de trocar o código', async () => {
    const { app, discord } = await makeTestApp({ config: { staging: true, discordClientId: 'cid', discordClientSecret: '' } });
    discord.users.set('c1', { id: '123', username: 'r', global_name: null, avatar: null });
    const res = await app.inject({ method: 'GET', url: '/v1/auth/discord/login?client=panel' });
    expect(res.statusCode).toBe(302);
    const loc = new URL(res.headers.location as string);
    expect(loc.searchParams.get('client_id')).toBe('cid');
    const state = loc.searchParams.get('state')!;
    const cookie = res.cookies.find((c) => c.name === 'cbmp_oauth')!.value;
    const cb = await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c1&state=${state}`, cookies: { cbmp_oauth: cookie } });
    expect(cb.statusCode).toBe(503);
    expect(cb.json()).toMatchObject({ error: 'login_unavailable' });
    expect(cb.json().message).toMatch(/DISCORD_CLIENT_SECRET/);
  });
  it('o resto do Hub funciona sem o app Discord', async () => {
    const { app } = await makeTestApp({ config: { staging: true, discordClientId: '', discordClientSecret: '' } });
    expect((await app.inject({ method: 'GET', url: '/health' })).json()).toEqual({ ok: true });
    expect((await app.inject({ method: 'GET', url: '/v1/servers' })).statusCode).toBe(200);
  });
});

describe('ensureSystemServer (cli ensure-server)', () => {
  const input = { name: 'Servidor de teste', description: 'd', tags: ['teste'], featured: true };
  it('cria no usuário de sistema, em destaque, e é idempotente com o estado anterior', () => {
    const store = new Store(openDb(':memory:'));
    const a = ensureSystemServer(store, input, T0);
    expect(a.action).toBe('created');
    expect(a.server.featured).toBe(true);
    expect(a.key).toMatch(/^sk_/);
    expect(store.getServerByKey(a.key)?.id).toBe(a.server.id);
    const b = ensureSystemServer(store, { ...input, description: 'nova', previous: { serverId: a.server.id, key: a.key } }, T0 + 1);
    expect(b).toMatchObject({ action: 'reused', key: a.key });
    expect(b.server.id).toBe(a.server.id);
    expect(b.server.description).toBe('nova');
    const owner = store.getUserById(b.server.ownerId)!;
    expect(owner.discordId).toBe(SYSTEM_DISCORD_ID);
  });
  it('sem a chave anterior (ou chave errada): mesmo servidor, chave nova; a antiga deixa de valer', () => {
    const store = new Store(openDb(':memory:'));
    const a = ensureSystemServer(store, input, T0);
    const b = ensureSystemServer(store, { ...input, previous: { serverId: a.server.id, key: 'sk_errada' } }, T0);
    expect(b.action).toBe('rotated');
    expect(b.server.id).toBe(a.server.id);
    expect(store.getServerByKey(a.key)).toBeNull();
    const c = ensureSystemServer(store, { ...input, featured: false }, T0);
    expect(c).toMatchObject({ action: 'rotated' });
    expect(c.server.featured).toBe(false);
  });
  it('nome inválido é recusado', () => {
    const store = new Store(openDb(':memory:'));
    expect(() => ensureSystemServer(store, { ...input, name: 'x' }, T0)).toThrow();
  });
});
