import { describe, expect, it } from 'vitest';
import { makeTestApp, loginAs, T0 } from './helpers';

const hb = { name: 'Castelo Bruxo', players: 12, maxPlayers: 512, gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1, tags: ['rp'] };

async function setup(config = {}) {
  const t = await makeTestApp({ config });
  const owner = loginAs(t.store, '1');
  const { server, key } = t.store.createServer(owner.user.id, { name: 'Inicial', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
  return { ...t, server, key };
}
const send = (app: Awaited<ReturnType<typeof setup>>['app'], key: string, body: object = hb) =>
  app.inject({ method: 'POST', url: '/v1/servers/heartbeat', headers: { authorization: `Bearer ${key}` }, payload: body, remoteAddress: '203.0.113.7' });

describe('heartbeat e lista', () => {
  it('lista só depois do probe confirmar o serverId', async () => {
    const { app, server, key, probeAnswers, clock } = await setup();
    const r1 = await send(app, key);
    expect(r1.json()).toEqual({ ok: true, listed: false, nextHeartbeatSeconds: 30, reason: 'probe_failed' });
    expect((await app.inject({ method: 'GET', url: '/v1/servers' })).json()).toEqual([]);

    probeAnswers.set('203.0.113.7:30500', server.id);
    clock.t += 120;
    const r2 = await send(app, key);
    expect(r2.json()).toEqual({ ok: true, listed: true, nextHeartbeatSeconds: 30 });
    clock.t += 5; // vence o cache de 5 s
    const list = (await app.inject({ method: 'GET', url: '/v1/servers' })).json();
    expect(list).toEqual([{
      id: server.id, name: 'Castelo Bruxo', description: '', tags: ['rp'], locale: 'pt-BR', players: 12, maxPlayers: 512,
      host: '203.0.113.7', gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1, featured: false,
    }]);
  });

  it('probe com serverId de outro servidor não lista', async () => {
    const { app, key, probeAnswers } = await setup();
    probeAnswers.set('203.0.113.7:30500', 'srv_outro');
    expect((await send(app, key)).json().listed).toBe(false);
  });

  it('não repete o probe antes de 10 min, repete depois', async () => {
    let calls = 0;
    const t = await makeTestApp({ probe: async () => { calls++; return { serverId: id }; } });
    const owner = loginAs(t.store, '1');
    const { server, key } = t.store.createServer(owner.user.id, { name: 'Inicial', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
    const id = server.id;
    await send(t.app, key);
    t.clock.t += 30; await send(t.app, key);
    t.clock.t += 30; await send(t.app, key);
    expect(calls).toBe(1);
    t.clock.t += 600; await send(t.app, key);
    expect(calls).toBe(2);
  });

  it('sai da lista após 90 s sem heartbeat', async () => {
    const { app, server, key, probeAnswers, clock } = await setup();
    probeAnswers.set('203.0.113.7:30500', server.id);
    await send(app, key);
    clock.t += 90;
    expect((await app.inject({ method: 'GET', url: '/v1/servers' })).json()).toHaveLength(1);
    clock.t += 6;
    expect((await app.inject({ method: 'GET', url: '/v1/servers' })).json()).toHaveLength(0);
  });

  it('IP vem da conexão, não do corpo', async () => {
    const { app, server, key, probeAnswers, clock } = await setup();
    probeAnswers.set('203.0.113.7:30500', server.id);
    await send(app, key, { ...hb, host: '1.1.1.1', ip: '1.1.1.1' });
    clock.t += 5;
    expect((await app.inject({ method: 'GET', url: '/v1/servers' })).json()[0].host).toBe('203.0.113.7');
  });

  it('chave inválida → 401; corpo inválido → 400', async () => {
    const { app, key } = await setup();
    expect((await send(app, 'sk_errada')).statusCode).toBe(401);
    const res = await app.inject({ method: 'POST', url: '/v1/servers/heartbeat', payload: hb });
    expect(res.statusCode).toBe(401);
    expect((await send(app, key, { ...hb, players: 9999 })).statusCode).toBe(400);
  });

  it('GET /v1/servers/:id mostra online/offline', async () => {
    const { app, server, key, probeAnswers } = await setup();
    const off = (await app.inject({ method: 'GET', url: `/v1/servers/${server.id}` })).json();
    expect(off).toMatchObject({ id: server.id, name: 'Inicial', online: false });
    probeAnswers.set('203.0.113.7:30500', server.id);
    await send(app, key);
    const on = (await app.inject({ method: 'GET', url: `/v1/servers/${server.id}` })).json();
    expect(on).toMatchObject({ id: server.id, online: true, players: 12, host: '203.0.113.7', protocol: 1 });
    expect(JSON.stringify(on)).not.toContain('useGlobalBans');
    expect((await app.inject({ method: 'GET', url: '/v1/servers/srv_nada' })).statusCode).toBe(404);
  });

  it('rate limit do heartbeat: 11º em 1 min → 429', async () => {
    const { app, key } = await setup({ rateLimit: true });
    for (let i = 0; i < 10; i++) expect((await send(app, key)).statusCode).toBe(200);
    const r = await send(app, key);
    expect(r.statusCode).toBe(429);
    expect(r.json().error).toBe('rate_limited');
  });
});
