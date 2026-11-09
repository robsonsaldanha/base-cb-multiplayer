import { describe, expect, it } from 'vitest';
import { makeTestApp, loginAs, T0 } from './helpers';

/** O cookie de sessão do painel só vale no painel, em /v1/me e em /v1/auth/logout; o resto da API é só Bearer. */
async function withPanelCookie() {
  const t = await makeTestApp();
  const user = t.store.upsertUserFromDiscord({ discordId: '1', name: 'Dono', avatar: null }, T0);
  const s = t.store.createSession(user.id, 'panel', T0);
  const { server } = t.store.createServer(user.id, { name: 'Srv', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
  return { ...t, user, server, access: s.access, cookies: { cbmp_session: s.access } };
}

describe('API: cookie de sessão não autentica rotas Bearer', () => {
  it('POST /v1/owner/servers só com cookie → 401', async () => {
    const { app, cookies } = await withPanelCookie();
    const r = await app.inject({ method: 'POST', url: '/v1/owner/servers', cookies, payload: { name: 'Castelo' } });
    expect(r.statusCode).toBe(401);
  });
  it('GET /v1/owner/servers só com cookie → 401', async () => {
    const { app, cookies } = await withPanelCookie();
    expect((await app.inject({ method: 'GET', url: '/v1/owner/servers', cookies })).statusCode).toBe(401);
  });
  it('POST /v1/tickets só com cookie → 401', async () => {
    const { app, cookies, server } = await withPanelCookie();
    const r = await app.inject({ method: 'POST', url: '/v1/tickets', cookies, payload: { serverId: server.id } });
    expect(r.statusCode).toBe(401);
  });
  it('GET /v1/me com cookie continua funcionando', async () => {
    const { app, cookies } = await withPanelCookie();
    const r = await app.inject({ method: 'GET', url: '/v1/me', cookies });
    expect(r.statusCode).toBe(200);
    expect(r.json().discordId).toBe('1');
  });
  it('POST /v1/auth/logout com cookie encerra a sessão', async () => {
    const { app, cookies, store, access } = await withPanelCookie();
    expect((await app.inject({ method: 'POST', url: '/v1/auth/logout', cookies })).statusCode).toBe(200);
    expect(store.findUserByAccess(access, T0)).toBeNull();
  });
  it('corpo form-urlencoded em /v1/owner/servers não é processado', async () => {
    const { app, store } = await withPanelCookie();
    const owner = loginAs(store, '2');
    const r = await app.inject({
      method: 'POST', url: '/v1/owner/servers',
      headers: { ...owner.auth, 'content-type': 'application/x-www-form-urlencoded' }, payload: 'name=Castelo',
    });
    expect([400, 415]).toContain(r.statusCode);
    expect(store.countServersByOwner(owner.user.id)).toBe(0);
  });
});
