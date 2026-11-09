import { describe, expect, it } from 'vitest';
import { publicKeyFromRaw, verifyTicket } from '@cbmp/contracts';
import { makeTestApp, loginAs, T0 } from './helpers';

async function setup(useGlobalBans = false) {
  const t = await makeTestApp();
  const owner = loginAs(t.store, '1');
  const { server } = t.store.createServer(owner.user.id, { name: 'Srv', description: '', tags: [], locale: 'pt-BR', useGlobalBans }, T0);
  const player = loginAs(t.store, '77', 'Jogador');
  return { ...t, server, player };
}

describe('tickets', () => {
  it('emite ticket válido para o servidor, verificável com /v1/keys/public', async () => {
    const { app, server, player } = await setup();
    const res = await app.inject({ method: 'POST', url: '/v1/tickets', headers: player.auth, payload: { serverId: server.id } });
    expect(res.statusCode).toBe(200);
    const { ticket, expiresAt } = res.json();
    expect(expiresAt).toBe(T0 + 300);
    const pubs = (await app.inject({ method: 'GET', url: '/v1/keys/public' })).json() as { kid: string; publicKey: string }[];
    const keys = new Map(pubs.map((k) => [k.kid, publicKeyFromRaw(k.publicKey)] as const));
    const r = verifyTicket(ticket, { publicKeys: keys, audience: server.id, nowSeconds: T0 + 5 });
    expect(r.ok && r.payload).toMatchObject({ v: 1, kid: 'kt', sub: player.user.id, did: '77', name: 'Jogador', aud: server.id, iat: T0, exp: T0 + 300 });
  });

  it('jti é único por ticket', async () => {
    const { app, server, player } = await setup();
    const a = (await app.inject({ method: 'POST', url: '/v1/tickets', headers: player.auth, payload: { serverId: server.id } })).json().ticket;
    const b = (await app.inject({ method: 'POST', url: '/v1/tickets', headers: player.auth, payload: { serverId: server.id } })).json().ticket;
    const jti = (t: string) => JSON.parse(Buffer.from(t.split('.')[0]!, 'base64url').toString()).jti;
    expect(jti(a)).not.toBe(jti(b));
  });

  it('servidor inexistente → 404; sem login → 401', async () => {
    const { app, player } = await setup();
    expect((await app.inject({ method: 'POST', url: '/v1/tickets', headers: player.auth, payload: { serverId: 'srv_nao' } })).statusCode).toBe(404);
    expect((await app.inject({ method: 'POST', url: '/v1/tickets', payload: { serverId: 'srv_nao' } })).statusCode).toBe(401);
  });

  it('ban global só bloqueia em servidor que optou', async () => {
    const optIn = await setup(true);
    optIn.store.addGlobalBan('77', 'cheat', '999', T0);
    const r1 = await optIn.app.inject({ method: 'POST', url: '/v1/tickets', headers: optIn.player.auth, payload: { serverId: optIn.server.id } });
    expect(r1.statusCode).toBe(403);
    expect(r1.json().error).toBe('banned_global');

    const optOut = await setup(false);
    optOut.store.addGlobalBan('77', 'cheat', '999', T0);
    const r2 = await optOut.app.inject({ method: 'POST', url: '/v1/tickets', headers: optOut.player.auth, payload: { serverId: optOut.server.id } });
    expect(r2.statusCode).toBe(200);
  });
});

describe('rate limit de tickets', () => {
  it('limita por usuário: 31º → 429; outro usuário no mesmo IP segue 200', async () => {
    const t = await makeTestApp({ config: { rateLimit: true } });
    const owner = loginAs(t.store, '1');
    const { server } = t.store.createServer(owner.user.id, { name: 'Srv', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
    const a = loginAs(t.store, '77', 'A');
    const b = loginAs(t.store, '78', 'B');
    const req = (u: typeof a) => t.app.inject({ method: 'POST', url: '/v1/tickets', headers: u.auth, payload: { serverId: server.id }, remoteAddress: '203.0.113.9' });
    for (let i = 0; i < 30; i++) expect((await req(a)).statusCode).toBe(200);
    const r = await req(a);
    expect(r.statusCode).toBe(429);
    expect(r.json().error).toBe('rate_limited');
    expect((await req(b)).statusCode).toBe(200);
  });
});
