import { describe, expect, it } from 'vitest';
import { makeTestApp, loginAs, T0 } from './helpers';

describe('bans globais', () => {
  it('admin da plataforma adiciona/remove; servidor lista com sua chave', async () => {
    const { app, store } = await makeTestApp();
    const admin = loginAs(store, '999');
    const { key } = store.createServer(admin.user.id, { name: 'Srv', description: '', tags: [], locale: 'pt-BR', useGlobalBans: true }, T0);
    const add = await app.inject({ method: 'POST', url: '/v1/admin/bans/global', headers: admin.auth, payload: { discordId: '77', reason: 'speedhack' } });
    expect(add.statusCode).toBe(201);
    const list = await app.inject({ method: 'GET', url: '/v1/bans/global', headers: { authorization: `Bearer ${key}` } });
    expect(list.json()).toEqual([{ discordId: '77', reason: 'speedhack', createdBy: '999', createdAt: T0 }]);
    const del = await app.inject({ method: 'DELETE', url: '/v1/admin/bans/global/77', headers: admin.auth });
    expect(del.statusCode).toBe(204);
    expect((await app.inject({ method: 'DELETE', url: '/v1/admin/bans/global/77', headers: admin.auth })).statusCode).toBe(404);
  });

  it('usuário comum não administra; lista exige chave de servidor', async () => {
    const { app, store } = await makeTestApp();
    const user = loginAs(store, '5');
    const r = await app.inject({ method: 'POST', url: '/v1/admin/bans/global', headers: user.auth, payload: { discordId: '77', reason: 'x' } });
    expect(r.statusCode).toBe(403);
    expect(r.json().error).toBe('forbidden');
    expect((await app.inject({ method: 'GET', url: '/v1/bans/global' })).statusCode).toBe(401);
  });
});
