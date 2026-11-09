import { describe, expect, it } from 'vitest';
import { makeTestApp, loginAs } from './helpers';

describe('owner servers', () => {
  it('cria, lista, edita, gira chave e apaga', async () => {
    const { app, store } = await makeTestApp();
    const { auth } = loginAs(store, '1');
    const c = await app.inject({ method: 'POST', url: '/v1/owner/servers', headers: auth, payload: { name: 'Castelo', tags: ['rp'] } });
    expect(c.statusCode).toBe(201);
    const { server, key } = c.json();
    expect(key).toMatch(/^sk_/);
    expect(server).toMatchObject({ name: 'Castelo', description: '', tags: ['rp'], locale: 'pt-BR', useGlobalBans: false, online: false });

    const l = await app.inject({ method: 'GET', url: '/v1/owner/servers', headers: auth });
    expect(l.json().map((s: { id: string }) => s.id)).toEqual([server.id]);
    expect(JSON.stringify(l.json())).not.toContain('sk_');

    const p = await app.inject({ method: 'PATCH', url: `/v1/owner/servers/${server.id}`, headers: auth, payload: { description: 'RP bruxo', useGlobalBans: true } });
    expect(p.json()).toMatchObject({ description: 'RP bruxo', useGlobalBans: true, name: 'Castelo' });

    const r = await app.inject({ method: 'POST', url: `/v1/owner/servers/${server.id}/rotate-key`, headers: auth });
    expect(r.json().key).not.toBe(key);

    const d = await app.inject({ method: 'DELETE', url: `/v1/owner/servers/${server.id}`, headers: auth });
    expect(d.statusCode).toBe(204);
  });

  it('outro usuário não vê nem mexe no servidor', async () => {
    const { app, store } = await makeTestApp();
    const a = loginAs(store, '1');
    const b = loginAs(store, '2');
    const { server } = (await app.inject({ method: 'POST', url: '/v1/owner/servers', headers: a.auth, payload: { name: 'Castelo' } })).json();
    expect((await app.inject({ method: 'GET', url: '/v1/owner/servers', headers: b.auth })).json()).toEqual([]);
    expect((await app.inject({ method: 'PATCH', url: `/v1/owner/servers/${server.id}`, headers: b.auth, payload: { name: 'Roubado' } })).statusCode).toBe(404);
    expect((await app.inject({ method: 'POST', url: `/v1/owner/servers/${server.id}/rotate-key`, headers: b.auth })).statusCode).toBe(404);
    expect((await app.inject({ method: 'DELETE', url: `/v1/owner/servers/${server.id}`, headers: b.auth })).statusCode).toBe(404);
  });

  it('valida corpo e limita 10 servidores por dono', async () => {
    const { app, store } = await makeTestApp();
    const { auth } = loginAs(store, '1');
    const bad = await app.inject({ method: 'POST', url: '/v1/owner/servers', headers: auth, payload: { name: 'x' } });
    expect(bad.statusCode).toBe(400);
    expect(bad.json().error).toBe('invalid_body');
    for (let i = 0; i < 10; i++) {
      expect((await app.inject({ method: 'POST', url: '/v1/owner/servers', headers: auth, payload: { name: `Srv ${i}` } })).statusCode).toBe(201);
    }
    const over = await app.inject({ method: 'POST', url: '/v1/owner/servers', headers: auth, payload: { name: 'Srv 11' } });
    expect(over.statusCode).toBe(409);
    expect(over.json().error).toBe('too_many_servers');
  });

  it('exige login', async () => {
    const { app } = await makeTestApp();
    expect((await app.inject({ method: 'GET', url: '/v1/owner/servers' })).statusCode).toBe(401);
  });
});
