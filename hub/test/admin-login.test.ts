import { PassThrough } from 'node:stream';
import { describe, expect, it } from 'vitest';
import { HeartbeatBody, publicKeyFromRaw, verifyTicket } from '@cbmp/contracts';
import { makeTestApp, loginAs, T0 } from './helpers';
import type { HubConfig } from '../src/config';

const hb = { name: 'Castelo Bruxo', players: 1, maxPlayers: 512, gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1 };
const ADMIN = 'https://painel.example.com:8443';
const IP = '93.184.216.34'; // IP público (faixas de documentação contam como privadas)

type T = Awaited<ReturnType<typeof setup>>;

async function setup(opts: { config?: Partial<HubConfig>; adminUrl?: string | null } = {}) {
  const lines: string[] = [];
  const logStream = new PassThrough();
  logStream.on('data', (b: Buffer) => lines.push(...b.toString('utf8').split('\n').filter(Boolean)));
  const t = await makeTestApp({ config: opts.config, logStream });
  const owner = loginAs(t.store, '1', 'Dono');
  const { server, key } = t.store.createServer(owner.user.id, { name: 'Castelo', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
  t.probeAnswers.set(`${IP}:30500`, server.id);
  const beat = (body: object) => t.app.inject({
    method: 'POST', url: '/v1/servers/heartbeat', headers: { authorization: `Bearer ${key}` }, payload: body, remoteAddress: IP,
  });
  const adminUrl = opts.adminUrl === undefined ? ADMIN : opts.adminUrl;
  const r = await beat(adminUrl === null ? hb : { ...hb, adminUrl });
  expect(r.statusCode).toBe(200);
  expect(r.json().listed).toBe(true);
  // Outro usuário (não é o dono): o painel do servidor decide quem é admin, o Hub só identifica.
  const admin = t.store.upsertUserFromDiscord({ discordId: '555', name: 'Admin', avatar: null }, T0);
  const panel = t.store.createSession(admin.id, 'panel', T0);
  return { ...t, owner, admin, server, key, beat, lines, cookies: { cbmp_session: panel.access } };
}

const go = (t: T, serverId: string, cookies: Record<string, string> = t.cookies, remoteAddress = '198.51.100.20') =>
  t.app.inject({ method: 'GET', url: `/v1/auth/server-admin?serverId=${encodeURIComponent(serverId)}`, cookies, remoteAddress });

const keysOf = async (t: T) => {
  const pubs = (await t.app.inject({ method: 'GET', url: '/v1/keys/public' })).json() as { kid: string; publicKey: string }[];
  return new Map(pubs.map((k) => [k.kid, publicKeyFromRaw(k.publicKey)] as const));
};

const ticketFrom = (location: string) => {
  const m = /#ticket=([A-Za-z0-9_-]+\.[A-Za-z0-9_-]+)$/.exec(location);
  expect(m).not.toBeNull();
  return m![1]!;
};

const audits = (t: T) => t.lines.map((l) => JSON.parse(l) as Record<string, unknown>).filter((l) => l.audit === 'server_admin_login');

describe('heartbeat com adminUrl', () => {
  it('grava a origem normalizada; heartbeat sem adminUrl apaga', async () => {
    const t = await setup({ adminUrl: 'https://Painel.Example.com:8443/' });
    expect(t.store.getStatus(t.server.id)!.adminUrl).toBe(ADMIN);
    await t.beat(hb);
    expect(t.store.getStatus(t.server.id)!.adminUrl).toBeNull();
  });

  it('adminUrl maliciosa é recusada pelo schema (400) e não grava nada', async () => {
    const t = await setup();
    for (const adminUrl of ['http://painel.example.com', 'https://u:p@painel.example.com', 'https://painel.example.com#x',
      'https://painel.example.com/auth/callback', 'https://painel.example.com?a=1', 'javascript:alert(1)']) {
      expect(HeartbeatBody.safeParse({ ...hb, adminUrl }).success, adminUrl).toBe(false);
      const r = await t.beat({ ...hb, adminUrl });
      expect(r.statusCode, adminUrl).toBe(400);
    }
    expect(t.store.getStatus(t.server.id)!.adminUrl).toBe(ADMIN);
  });

  it('adminUrl em loopback só é aceita com HUB_ALLOW_PRIVATE_SERVERS (dev); senão é ignorada', async () => {
    const dev = await setup({ adminUrl: 'http://127.0.0.1:30510' });
    expect(dev.store.getStatus(dev.server.id)!.adminUrl).toBe('http://127.0.0.1:30510');
    const prod = await setup({ config: { allowPrivateServers: false }, adminUrl: 'http://127.0.0.1:30510' });
    expect(prod.store.getStatus(prod.server.id)!.adminUrl).toBeNull();
    const prod2 = await setup({ config: { allowPrivateServers: false }, adminUrl: 'https://localhost:30510' });
    expect(prod2.store.getStatus(prod2.server.id)!.adminUrl).toBeNull();
  });
});

describe('GET /v1/auth/server-admin', () => {
  it('com sessão: 303 para <adminUrl>/auth/callback#ticket=…, sem cache e sem referrer', async () => {
    const t = await setup();
    const res = await go(t, t.server.id);
    expect(res.statusCode).toBe(303);
    const loc = res.headers.location as string;
    expect(loc.startsWith(`${ADMIN}/auth/callback#ticket=`)).toBe(true);
    expect(loc).toBe(`${ADMIN}/auth/callback#ticket=${ticketFrom(loc)}`);
    expect(res.headers['cache-control']).toBe('no-store');
    expect(res.headers['referrer-policy']).toBe('no-referrer');
  });

  it('ticket verifica com aud "<serverId>#admin", TTL 300 s, did/sub do usuário e jti novo', async () => {
    const t = await setup();
    const a = ticketFrom((await go(t, t.server.id)).headers.location as string);
    const b = ticketFrom((await go(t, t.server.id)).headers.location as string);
    const keys = await keysOf(t);
    const r = verifyTicket(a, { publicKeys: keys, audience: `${t.server.id}#admin`, nowSeconds: T0 + 5 });
    expect(r.ok && r.payload).toMatchObject({ v: 1, kid: 'kt', sub: t.admin.id, did: '555', name: 'Admin', aud: `${t.server.id}#admin`, iat: T0, exp: T0 + 300 });
    const jti = (x: string) => JSON.parse(Buffer.from(x.split('.')[0]!, 'base64url').toString()).jti;
    expect(jti(a)).not.toBe(jti(b));
  });

  it('ticket #admin é recusado pela verificação de jogo e o ticket de jogo pela de admin', async () => {
    const t = await setup();
    const keys = await keysOf(t);
    const adminTicket = ticketFrom((await go(t, t.server.id)).headers.location as string);
    expect(verifyTicket(adminTicket, { publicKeys: keys, audience: t.server.id, nowSeconds: T0 })).toEqual({ ok: false, error: 'wrong_audience' });
    const player = loginAs(t.store, '555');
    const game = (await t.app.inject({ method: 'POST', url: '/v1/tickets', headers: player.auth, payload: { serverId: t.server.id } })).json().ticket as string;
    expect(verifyTicket(game, { publicKeys: keys, audience: t.server.id, nowSeconds: T0 }).ok).toBe(true);
    expect(verifyTicket(game, { publicKeys: keys, audience: `${t.server.id}#admin`, nowSeconds: T0 })).toEqual({ ok: false, error: 'wrong_audience' });
  });

  it('servidor de outro dono funciona igual (o painel do servidor decide quem é admin)', async () => {
    const t = await setup();
    expect(t.server.ownerId).not.toBe(t.admin.id);
    expect((await go(t, t.server.id)).statusCode).toBe(303);
    const own = await go(t, t.server.id, { cbmp_session: t.store.createSession(t.owner.user.id, 'panel', T0).access });
    expect(own.statusCode).toBe(303);
  });

  it('sem sessão: guarda o serverId em cbmp_return e manda para o login do painel', async () => {
    const t = await setup();
    const res = await go(t, t.server.id, {});
    expect(res.statusCode).toBe(302);
    expect(res.headers.location).toBe('/v1/auth/discord/login?client=panel');
    const c = res.cookies.find((x) => x.name === 'cbmp_return')!;
    expect(c.value).toBe(t.server.id);
    expect(c.httpOnly).toBe(true);
    expect(c.sameSite).toBe('Lax');
    expect(c.path).toBe('/v1/auth');
    expect(c.maxAge).toBeLessThanOrEqual(600);
    expect(c.secure).toBeFalsy(); // Hub em http nos testes
    // Sessão de launcher (Bearer) não vale aqui: só o cookie do painel.
    const player = loginAs(t.store, '777');
    const viaBearer = await t.app.inject({ method: 'GET', url: `/v1/auth/server-admin?serverId=${t.server.id}`, headers: player.auth });
    expect(viaBearer.statusCode).toBe(302);
  });

  it('cbmp_return é Secure quando o Hub é https', async () => {
    const t = await setup({ config: { publicUrl: 'https://hub.example.com' } });
    const c = (await go(t, t.server.id, {})).cookies.find((x) => x.name === 'cbmp_return')!;
    expect(c.secure).toBe(true);
  });

  it('fluxo completo: sem sessão → Discord → callback volta para server-admin com o serverId validado', async () => {
    const t = await setup();
    t.discord.users.set('c9', { id: '555', username: 'admin', global_name: 'Admin', avatar: null });
    const first = await go(t, t.server.id, {});
    const ret = first.cookies.find((x) => x.name === 'cbmp_return')!.value;
    const login = await t.app.inject({ method: 'GET', url: '/v1/auth/discord/login?client=panel' });
    const state = new URL(login.headers.location as string).searchParams.get('state')!;
    const oauth = login.cookies.find((x) => x.name === 'cbmp_oauth')!.value;
    const cb = await t.app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c9&state=${state}`, cookies: { cbmp_oauth: oauth, cbmp_return: ret } });
    expect(cb.statusCode).toBe(302);
    expect(cb.headers.location).toBe(`/v1/auth/server-admin?serverId=${t.server.id}`);
    const cleared = cb.cookies.find((x) => x.name === 'cbmp_return')!;
    expect(cleared.value).toBe('');
    expect(cleared.path).toBe('/v1/auth');
    const session = cb.cookies.find((x) => x.name === 'cbmp_session')!.value;
    const final = await go(t, t.server.id, { cbmp_session: session });
    expect(final.statusCode).toBe(303);
  });

  it('cbmp_return inválido (URL arbitrária) é ignorado: callback vai para /panel', async () => {
    const t = await setup();
    t.discord.users.set('c9', { id: '555', username: 'admin', global_name: 'Admin', avatar: null });
    for (const ret of ['https://evil.com', '//evil.com', 'srv_x/../../evil', `${t.server.id}&x=1`, '']) {
      const login = await t.app.inject({ method: 'GET', url: '/v1/auth/discord/login?client=panel' });
      const state = new URL(login.headers.location as string).searchParams.get('state')!;
      const oauth = login.cookies.find((x) => x.name === 'cbmp_oauth')!.value;
      const cb = await t.app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c9&state=${state}`, cookies: { cbmp_oauth: oauth, cbmp_return: ret } });
      expect(cb.headers.location, ret).toBe('/panel');
    }
  });

  it('serverId com formato inválido → 400 HTML, sem consultar nem emitir', async () => {
    const t = await setup();
    for (const id of ['', 'x', 'srv_', 'srv_<script>alert(1)</script>', 'https://evil.com', `${t.server.id}#admin`, 'srv_' + 'a'.repeat(60)]) {
      const res = await go(t, id);
      expect(res.statusCode, id).toBe(400);
      expect(res.headers['content-type']).toContain('text/html');
      expect(res.body).not.toContain('<script>');
      expect(res.headers.location).toBeUndefined();
    }
    // Sem sessão e serverId inválido: nem guarda o retorno.
    const anon = await go(t, 'https://evil.com', {});
    expect(anon.statusCode).toBe(400);
    expect(anon.cookies.find((x) => x.name === 'cbmp_return')).toBeUndefined();
  });

  it('servidor inexistente → 404 HTML', async () => {
    const t = await setup();
    const res = await go(t, 'srv_AAAAAAAAAAAA');
    expect(res.statusCode).toBe(404);
    expect(res.headers['content-type']).toContain('text/html');
    expect(res.headers.location).toBeUndefined();
  });

  it('sem adminUrl registrado → página explicando, com CSP estrita e sem ticket', async () => {
    const t = await setup({ adminUrl: null });
    const res = await go(t, t.server.id);
    expect(res.statusCode).toBe(409);
    expect(res.headers['content-type']).toContain('text/html');
    expect(res.body).toContain('admin.publicUrl');
    expect(res.body).toContain('Castelo Bruxo'); // nome do servidor, escapado
    expect(res.headers['content-security-policy']).toBe("default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    expect(res.headers['x-frame-options']).toBe('DENY');
    expect(res.headers['cache-control']).toBe('no-store');
    expect(res.headers['referrer-policy']).toBe('no-referrer');
    expect(res.headers.location).toBeUndefined();
    expect(res.body).not.toMatch(/ticket=/);
  });

  it('nome do servidor é escapado na página', async () => {
    const t = await setup({ adminUrl: null });
    await t.beat({ ...hb, name: 'Mau <img src=x onerror=alert(1)>' });
    const res = await go(t, t.server.id);
    expect(res.body).toContain('Mau &lt;img src=x onerror=alert(1)&gt;');
    expect(res.body).not.toContain('<img');
  });

  it('servidor offline (sem heartbeat recente) ou não verificado → página, sem ticket', async () => {
    const t = await setup();
    t.clock.t += 91;
    const res = await go(t, t.server.id);
    expect(res.statusCode).toBe(503);
    expect(res.headers['content-type']).toContain('text/html');
    expect(res.headers.location).toBeUndefined();

    const u = await setup();
    u.store.setVerified(u.server.id, false, T0);
    expect((await go(u, u.server.id)).statusCode).toBe(503);
  });

  it('rate limit: 10/min por usuário (11º → 429); outro usuário segue', async () => {
    const t = await setup({ config: { rateLimit: true } });
    for (let i = 0; i < 10; i++) expect((await go(t, t.server.id, t.cookies, `198.51.100.${i}`)).statusCode).toBe(303);
    const r = await go(t, t.server.id, t.cookies, '198.51.100.99');
    expect(r.statusCode).toBe(429);
    expect(r.headers.location).toBeUndefined();
    const other = t.store.createSession(t.owner.user.id, 'panel', T0).access;
    expect((await go(t, t.server.id, { cbmp_session: other })).statusCode).toBe(303);
    t.clock.t += 60; // janela nova
    expect((await go(t, t.server.id)).statusCode).toBe(303);
  });

  it('rate limit por IP: 30/min por IP, mesmo sem sessão', async () => {
    const t = await setup({ config: { rateLimit: true } });
    for (let i = 0; i < 30; i++) expect((await go(t, t.server.id, {}, '198.51.100.7')).statusCode).toBe(302);
    expect((await go(t, t.server.id, {}, '198.51.100.7')).statusCode).toBe(429);
    expect((await go(t, t.server.id, {}, '198.51.100.8')).statusCode).toBe(302);
  });

  it('auditoria no log do Hub: did, serverId e resultado; nunca o ticket', async () => {
    const t = await setup({ adminUrl: null });
    await go(t, t.server.id);
    await t.beat({ ...hb, adminUrl: ADMIN });
    const ok = await go(t, t.server.id);
    const ticket = ticketFrom(ok.headers.location as string);
    await go(t, 'srv_AAAAAAAAAAAA');
    await go(t, t.server.id, {});
    const a = audits(t);
    expect(a.map((x) => [x.did, x.serverId, x.result])).toEqual([
      ['555', t.server.id, 'no_admin_url'],
      ['555', t.server.id, 'ticket_issued'],
      ['555', 'srv_AAAAAAAAAAAA', 'not_found'],
      [null, t.server.id, 'login_required'],
    ]);
    expect(t.lines.join('\n')).not.toContain(ticket);
    expect(t.lines.join('\n')).not.toContain(ticket.split('.')[1]!);
  });
});
