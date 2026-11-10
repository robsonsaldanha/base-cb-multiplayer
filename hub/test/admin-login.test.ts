import { PassThrough } from 'node:stream';
import { describe, expect, it } from 'vitest';
import { HeartbeatBody, publicKeyFromRaw, verifyTicket } from '@cbmp/contracts';
import { makeTestApp, loginAs, T0 } from './helpers';
import type { HubConfig } from '../src/config';

const hb = { name: 'Castelo Bruxo', players: 1, maxPlayers: 512, gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1 };
const ADMIN = 'https://painel.example.com:8443';
const IP = '93.184.216.34'; // IP público (faixas de documentação contam como privadas)
const HUB = 'http://localhost:8787';
const STATE = 'Abc_def-0123456789xyz';

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
  const admin = t.store.upsertUserFromDiscord({ discordId: '555', name: 'Admin <b>', avatar: null }, T0);
  const panel = t.store.createSession(admin.id, 'panel', T0);
  return { ...t, owner, admin, server, key, beat, lines, cookies: { cbmp_session: panel.access } };
}

const url = (serverId: string, state?: string) =>
  `/v1/auth/server-admin?serverId=${encodeURIComponent(serverId)}${state === undefined ? '' : `&state=${encodeURIComponent(state)}`}`;

const view = (t: T, serverId: string, opts: { cookies?: Record<string, string>; ip?: string; state?: string } = {}) =>
  t.app.inject({ method: 'GET', url: url(serverId, opts.state), cookies: opts.cookies ?? t.cookies, remoteAddress: opts.ip ?? '198.51.100.20' });

const csrfFrom = (html: string) => /name="csrf" value="([^"]+)"/.exec(html)?.[1];

const confirm = (t: T, fields: Record<string, string | undefined>, opts: { cookies?: Record<string, string>; origin?: string | null; ip?: string } = {}) => {
  const headers: Record<string, string> = { 'content-type': 'application/x-www-form-urlencoded' };
  const origin = opts.origin === undefined ? HUB : opts.origin;
  if (origin !== null) headers.origin = origin;
  const body = new URLSearchParams(Object.entries(fields).filter(([, v]) => v !== undefined) as [string, string][]).toString();
  return t.app.inject({ method: 'POST', url: '/v1/auth/server-admin', headers, payload: body, cookies: opts.cookies ?? t.cookies, remoteAddress: opts.ip ?? '198.51.100.20' });
};

/** GET de confirmação + POST com o token da página. */
async function login(t: T, opts: { cookies?: Record<string, string>; state?: string; ip?: string } = {}) {
  const page = await view(t, t.server.id, opts);
  expect(page.statusCode).toBe(200);
  return confirm(t, { serverId: t.server.id, csrf: csrfFrom(page.body), state: opts.state }, opts);
}

const keysOf = async (t: T) => {
  const pubs = (await t.app.inject({ method: 'GET', url: '/v1/keys/public' })).json() as { kid: string; publicKey: string }[];
  return new Map(pubs.map((k) => [k.kid, publicKeyFromRaw(k.publicKey)] as const));
};

const ticketFrom = (location: string) => {
  const m = /#ticket=([A-Za-z0-9_-]+\.[A-Za-z0-9_-]+)(&state=[A-Za-z0-9_-]+)?$/.exec(location);
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

  it('adminUrl inválida é recusada pelo schema, mas o heartbeat segue (campo ignorado com aviso)', async () => {
    const t = await setup();
    for (const adminUrl of ['http://painel.example.com', 'https://u:p@painel.example.com', 'https://painel.example.com#x',
      'https://painel.example.com/auth/callback', 'https://painel.example.com?a=1', 'javascript:alert(1)', 'https://999.1.1.1', 42]) {
      expect(HeartbeatBody.safeParse({ ...hb, adminUrl }).success, String(adminUrl)).toBe(false);
      const r = await t.beat({ ...hb, adminUrl });
      expect(r.statusCode, String(adminUrl)).toBe(200);
      expect(r.json()).toMatchObject({ ok: true, listed: true, adminUrlIgnored: 'invalid' });
      expect(t.store.getStatus(t.server.id)!.adminUrl).toBeNull();
      expect((await t.app.inject({ method: 'GET', url: `/v1/servers/${t.server.id}` })).json().online).toBe(true);
    }
    const ok = await t.beat({ ...hb, adminUrl: ADMIN });
    expect(ok.json().adminUrlIgnored).toBeUndefined();
  });

  it('adminUrl em loopback/IP privado só é gravada com HUB_ALLOW_PRIVATE_SERVERS (dev)', async () => {
    const dev = await setup({ adminUrl: 'http://127.0.0.1:30510' });
    expect(dev.store.getStatus(dev.server.id)!.adminUrl).toBe('http://127.0.0.1:30510');
    // O heartbeat tem limite de 10/min por chave (relógio real): um servidor novo a cada 7 valores.
    const priv = ['http://127.0.0.1:30510', 'https://localhost:30510', 'https://foo.localhost', 'https://[::ffff:7f00:1]',
      'https://[::1]', 'https://[::]', 'https://0.0.0.0', 'https://10.0.0.5', 'https://192.168.1.1', 'https://169.254.169.254',
      'https://[fe80::1]', 'https://[fc00::1]', 'https://100.64.0.1'];
    for (let i = 0; i < priv.length; i += 7) {
      const prod = await setup({ config: { allowPrivateServers: false }, adminUrl: ADMIN });
      for (const u of priv.slice(i, i + 7)) {
        const r = await prod.beat({ ...hb, adminUrl: u });
        expect(r.json(), u).toMatchObject({ listed: true, adminUrlIgnored: 'private_host' });
        expect(prod.store.getStatus(prod.server.id)!.adminUrl, u).toBeNull();
      }
    }
    const prod = await setup({ config: { allowPrivateServers: false }, adminUrl: ADMIN });
    for (const u of ['https://8.8.8.8', 'https://[2606:4700::1111]', ADMIN]) {
      await prod.beat({ ...hb, adminUrl: u });
      expect(prod.store.getStatus(prod.server.id)!.adminUrl, u).toBe(u);
    }
  });
});

describe('GET /v1/auth/server-admin (confirmação)', () => {
  it('com sessão: 200 com página de confirmação, sem ticket e sem redirecionar', async () => {
    const t = await setup();
    const res = await view(t, t.server.id);
    expect(res.statusCode).toBe(200);
    expect(res.headers['content-type']).toContain('text/html');
    expect(res.headers.location).toBeUndefined();
    expect(res.body).not.toMatch(/ticket/i);
    expect(res.body).toContain('Castelo Bruxo');
    expect(res.body).toContain(t.server.id);
    expect(res.body).toContain(ADMIN);
    expect(res.body).toContain('555');
    expect(res.body).toContain('Admin &lt;b&gt;');
    expect(res.body).toContain('Entrar como admin');
    expect(res.body).toContain('method="post" action="/v1/auth/server-admin"');
    expect(csrfFrom(res.body)).toMatch(/^[0-9]+\.[A-Za-z0-9_-]{43}$/);
    expect(res.headers['content-security-policy']).toBe(`default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self' ${ADMIN}; frame-ancestors 'none'`);
    expect(res.headers['x-frame-options']).toBe('DENY');
    expect(res.headers['cache-control']).toBe('no-store');
    // Regra do navegador (Fetch, "append a request Origin header"): POST vindo de documento com
    // Referrer-Policy no-referrer leva `Origin: null` — e o POST exige Origin == Hub. Por isso a página de
    // confirmação usa same-origin (o form é same-origin, então o Origin vai; nada vaza para fora).
    expect(res.headers['referrer-policy']).toBe('same-origin');
  });

  it('respostas que levam a outra origem ou páginas de erro continuam sem referrer', async () => {
    const t = await setup({ adminUrl: null });
    expect((await view(t, t.server.id)).headers['referrer-policy']).toBe('no-referrer'); // 409
    expect((await view(t, 'x')).headers['referrer-policy']).toBe('no-referrer'); // 400
    expect((await view(t, t.server.id, { cookies: {} })).headers['referrer-policy']).toBe('no-referrer'); // 302 login
  });

  it('adminUrl IPv6 literal: CSP form-action usa o esquema (a gramática host-source da CSP não aceita [..])', async () => {
    const t = await setup({ adminUrl: 'https://[2606:4700::1111]:8443' });
    const res = await view(t, t.server.id);
    expect(res.statusCode).toBe(200);
    expect(res.headers['content-security-policy']).toBe("default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self' https:; frame-ancestors 'none'");
    expect(res.headers['content-security-policy']).not.toContain('[');
    const dev = await setup({ adminUrl: 'http://[::1]:30510' });
    expect((await view(dev, dev.server.id)).headers['content-security-policy']).toContain("form-action 'self' http:;");
    const ok = await confirm(dev, { serverId: dev.server.id, csrf: csrfFrom((await view(dev, dev.server.id)).body) });
    expect(ok.headers.location).toMatch(/^http:\/\/\[::1\]:30510\/auth\/callback#ticket=/);
  });

  it('nome do servidor e do usuário são escapados', async () => {
    const t = await setup();
    await t.beat({ ...hb, name: 'Mau <img src=x onerror=alert(1)>', adminUrl: ADMIN });
    const res = await view(t, t.server.id);
    expect(res.body).toContain('Mau &lt;img src=x onerror=alert(1)&gt;');
    expect(res.body).not.toContain('<img');
    expect(res.body).not.toContain('<b>');
  });

  it('avisa quando o destino é um endereço IP', async () => {
    const t = await setup({ adminUrl: 'https://8.8.8.8:30510' });
    expect((await view(t, t.server.id)).body).toContain('endereço IP');
    const u = await setup();
    expect((await view(u, u.server.id)).body).not.toContain('endereço IP');
  });

  it('sem sessão: manda para o login do painel com o retorno amarrado ao state OAuth (sem cookie extra)', async () => {
    const t = await setup();
    const res = await view(t, t.server.id, { cookies: {}, state: STATE });
    expect(res.statusCode).toBe(302);
    expect(res.headers.location).toBe(`/v1/auth/discord/login?client=panel&return=${t.server.id}&return_state=${STATE}`);
    expect(res.cookies).toEqual([]);
    // Sessão de launcher (Bearer) não vale aqui: só o cookie do painel.
    const player = loginAs(t.store, '777');
    const viaBearer = await t.app.inject({ method: 'GET', url: url(t.server.id), headers: player.auth });
    expect(viaBearer.statusCode).toBe(302);
  });

  it('fluxo completo prompt=none: login silencioso termina na CONFIRMAÇÃO, não em ticket', async () => {
    const t = await setup();
    t.discord.users.set('c9', { id: '555', username: 'admin', global_name: 'Admin', avatar: null });
    const first = await view(t, t.server.id, { cookies: {}, state: STATE });
    const login = await t.app.inject({ method: 'GET', url: first.headers.location as string });
    expect(login.statusCode).toBe(302);
    const discordUrl = new URL(login.headers.location as string);
    expect(discordUrl.searchParams.get('prompt')).toBe('none');
    const state = discordUrl.searchParams.get('state')!;
    const oauth = login.cookies.find((x) => x.name === 'cbmp_oauth')!.value;
    const cb = await t.app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c9&state=${state}`, cookies: { cbmp_oauth: oauth } });
    expect(cb.statusCode).toBe(302);
    expect(cb.headers.location).toBe(`/v1/auth/server-admin?serverId=${t.server.id}&state=${STATE}`);
    const session = cb.cookies.find((x) => x.name === 'cbmp_session')!.value;
    const page = await t.app.inject({ method: 'GET', url: cb.headers.location as string, cookies: { cbmp_session: session } });
    expect(page.statusCode).toBe(200);
    expect(page.headers.location).toBeUndefined();
    expect(page.body).not.toMatch(/ticket=/);
  });

  it('o retorno só vale para o state que nasceu com ele; login comum vai para /panel', async () => {
    const t = await setup();
    t.discord.users.set('c9', { id: '555', username: 'admin', global_name: 'Admin', avatar: null });
    await view(t, t.server.id, { cookies: {} }); // intenção abandonada
    const login = await t.app.inject({ method: 'GET', url: '/v1/auth/discord/login?client=panel' });
    const state = new URL(login.headers.location as string).searchParams.get('state')!;
    const oauth = login.cookies.find((x) => x.name === 'cbmp_oauth')!.value;
    const cb = await t.app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c9&state=${state}`, cookies: { cbmp_oauth: oauth } });
    expect(cb.headers.location).toBe('/panel');
  });

  it('retorno inválido no login é recusado; no launcher é ignorado', async () => {
    const t = await setup();
    for (const q of ['return=https://evil.com', 'return=//evil.com', 'return=srv_x', `return=${t.server.id}&return_state=curto`, `return_state=${STATE}`]) {
      const r = await t.app.inject({ method: 'GET', url: `/v1/auth/discord/login?client=panel&${q}` });
      expect(r.statusCode, q).toBe(400);
    }
    t.discord.users.set('c9', { id: '555', username: 'admin', global_name: 'Admin', avatar: null });
    const ch = 'a'.repeat(43);
    const login = await t.app.inject({ method: 'GET', url: `/v1/auth/discord/login?client=launcher&code_challenge=${ch}&code_challenge_method=S256&return=${t.server.id}` });
    const state = new URL(login.headers.location as string).searchParams.get('state')!;
    const oauth = login.cookies.find((x) => x.name === 'cbmp_oauth')!.value;
    const cb = await t.app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c9&state=${state}`, cookies: { cbmp_oauth: oauth } });
    expect((cb.headers.location as string).startsWith('cbmp://auth?code=')).toBe(true);
  });

  it('serverId ou state com formato inválido → 400 HTML, sem consultar nem emitir', async () => {
    const t = await setup();
    for (const id of ['', 'x', 'srv_', 'srv_<script>alert(1)</script>', 'https://evil.com', `${t.server.id}#admin`, 'srv_' + 'a'.repeat(60)]) {
      const res = await view(t, id);
      expect(res.statusCode, id).toBe(400);
      expect(res.headers['content-type']).toContain('text/html');
      expect(res.body).not.toContain('<script>');
      expect(res.headers.location).toBeUndefined();
    }
    for (const st of ['curto', 'a'.repeat(65), 'tem espaço aqui 1234', '<script>alert(1)</script>']) {
      expect((await view(t, t.server.id, { state: st })).statusCode, st).toBe(400);
    }
    expect((await view(t, 'https://evil.com', { cookies: {} })).statusCode).toBe(400);
  });

  it('servidor inexistente → 404; sem adminUrl → 409 explicando admin.publicUrl; offline/não verificado → 503', async () => {
    const t = await setup({ adminUrl: null });
    const nf = await view(t, 'srv_AAAAAAAAAAAA');
    expect(nf.statusCode).toBe(404);
    const no = await view(t, t.server.id);
    expect(no.statusCode).toBe(409);
    expect(no.body).toContain('admin.publicUrl');
    expect(no.headers['content-security-policy']).toBe("default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    const u = await setup();
    u.clock.t += 91;
    expect((await view(u, u.server.id)).statusCode).toBe(503);
    const v = await setup();
    v.store.setVerified(v.server.id, false, T0);
    expect((await view(v, v.server.id)).statusCode).toBe(503);
  });
});

describe('POST /v1/auth/server-admin (emite o ticket)', () => {
  it('passa: 303 exato para <adminUrl>/auth/callback#ticket=…, sem cache e sem referrer', async () => {
    const t = await setup();
    const res = await login(t);
    expect(res.statusCode).toBe(303);
    const loc = res.headers.location as string;
    expect(loc).toBe(`${ADMIN}/auth/callback#ticket=${ticketFrom(loc)}`);
    expect(res.headers['cache-control']).toBe('no-store');
    expect(res.headers['referrer-policy']).toBe('no-referrer');
  });

  it('repassa o state do painel no fragmento', async () => {
    const t = await setup();
    const loc = (await login(t, { state: STATE })).headers.location as string;
    expect(loc).toBe(`${ADMIN}/auth/callback#ticket=${ticketFrom(loc)}&state=${STATE}`);
  });

  it('ticket verifica com aud "<serverId>#admin", TTL 300 s, did/sub do usuário e jti novo', async () => {
    const t = await setup();
    const a = ticketFrom((await login(t)).headers.location as string);
    const b = ticketFrom((await login(t)).headers.location as string);
    const keys = await keysOf(t);
    const r = verifyTicket(a, { publicKeys: keys, audience: `${t.server.id}#admin`, nowSeconds: T0 + 5 });
    expect(r.ok && r.payload).toMatchObject({ v: 1, kid: 'kt', sub: t.admin.id, did: '555', name: 'Admin <b>', aud: `${t.server.id}#admin`, iat: T0, exp: T0 + 300 });
    const jti = (x: string) => JSON.parse(Buffer.from(x.split('.')[0]!, 'base64url').toString()).jti;
    expect(jti(a)).not.toBe(jti(b));
  });

  it('ticket #admin é recusado pela verificação de jogo e o ticket de jogo pela de admin', async () => {
    const t = await setup();
    const keys = await keysOf(t);
    const adminTicket = ticketFrom((await login(t)).headers.location as string);
    expect(verifyTicket(adminTicket, { publicKeys: keys, audience: t.server.id, nowSeconds: T0 })).toEqual({ ok: false, error: 'wrong_audience' });
    const player = loginAs(t.store, '555');
    const game = (await t.app.inject({ method: 'POST', url: '/v1/tickets', headers: player.auth, payload: { serverId: t.server.id } })).json().ticket as string;
    expect(verifyTicket(game, { publicKeys: keys, audience: t.server.id, nowSeconds: T0 }).ok).toBe(true);
    expect(verifyTicket(game, { publicKeys: keys, audience: `${t.server.id}#admin`, nowSeconds: T0 })).toEqual({ ok: false, error: 'wrong_audience' });
  });

  it('servidor de outro dono funciona igual (o painel do servidor decide quem é admin)', async () => {
    const t = await setup();
    expect(t.server.ownerId).not.toBe(t.admin.id);
    expect((await login(t)).statusCode).toBe(303);
    expect((await login(t, { cookies: { cbmp_session: t.store.createSession(t.owner.user.id, 'panel', T0).access } })).statusCode).toBe(303);
  });

  it('sem token, token ruim, token de outra sessão/servidor/state, Origin estrangeiro ou ausente → 403 sem ticket', async () => {
    const t = await setup();
    const page = await view(t, t.server.id, { state: STATE });
    const csrf = csrfFrom(page.body)!;
    const other = t.store.createSession(t.owner.user.id, 'panel', T0).access;
    const cases: [string, Record<string, string | undefined>, { cookies?: Record<string, string>; origin?: string | null }][] = [
      ['sem token', { serverId: t.server.id, state: STATE }, {}],
      ['token vazio', { serverId: t.server.id, csrf: '', state: STATE }, {}],
      ['token ruim', { serverId: t.server.id, csrf: 'x.y', state: STATE }, {}],
      ['token adulterado', { serverId: t.server.id, csrf: csrf.slice(0, -2) + (csrf.endsWith('AA') ? 'BB' : 'AA'), state: STATE }, {}],
      ['outra sessão', { serverId: t.server.id, csrf, state: STATE }, { cookies: { cbmp_session: other } }],
      ['outro state', { serverId: t.server.id, csrf, state: 'Zzz_def-0123456789xyz' }, {}],
      ['sem state', { serverId: t.server.id, csrf }, {}],
      ['Origin estrangeiro', { serverId: t.server.id, csrf, state: STATE }, { origin: 'https://evil.example' }],
      ['Origin null', { serverId: t.server.id, csrf, state: STATE }, { origin: 'null' }],
      ['sem Origin', { serverId: t.server.id, csrf, state: STATE }, { origin: null }],
    ];
    for (const [name, fields, opts] of cases) {
      const r = await confirm(t, fields, opts);
      expect(r.statusCode, name).toBe(403);
      expect(r.headers.location, name).toBeUndefined();
      expect(r.body, name).not.toMatch(/ticket=/);
    }
    // O mesmo token, certo, passa.
    expect((await confirm(t, { serverId: t.server.id, csrf, state: STATE })).statusCode).toBe(303);
  });

  it('token de outro servidor não vale', async () => {
    const t = await setup();
    const { server: s2, key: k2 } = t.store.createServer(t.owner.user.id, { name: 'Outro', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
    t.probeAnswers.set(`${IP}:30501`, s2.id);
    await t.app.inject({ method: 'POST', url: '/v1/servers/heartbeat', headers: { authorization: `Bearer ${k2}` }, payload: { ...hb, httpPort: 30501, adminUrl: ADMIN }, remoteAddress: IP });
    const csrf = csrfFrom((await view(t, s2.id)).body)!;
    expect((await confirm(t, { serverId: t.server.id, csrf })).statusCode).toBe(403);
    expect((await confirm(t, { serverId: s2.id, csrf })).statusCode).toBe(303);
  });

  it('token vence em 10 min e deixa de valer se o adminUrl mudar depois da confirmação', async () => {
    const t = await setup();
    const csrf = csrfFrom((await view(t, t.server.id)).body)!;
    await t.beat({ ...hb, adminUrl: 'https://outro.example.com' });
    expect((await confirm(t, { serverId: t.server.id, csrf })).statusCode).toBe(403);
    const csrf2 = csrfFrom((await view(t, t.server.id)).body)!;
    t.clock.t += 601;
    await t.beat({ ...hb, adminUrl: 'https://outro.example.com' }); // continua online
    expect((await confirm(t, { serverId: t.server.id, csrf: csrf2 })).statusCode).toBe(403);
  });

  it('POST sem sessão → 401 sem ticket; servidor offline → 503', async () => {
    const t = await setup();
    const csrf = csrfFrom((await view(t, t.server.id)).body)!;
    expect((await confirm(t, { serverId: t.server.id, csrf }, { cookies: {} })).statusCode).toBe(401);
    t.clock.t += 91;
    expect((await confirm(t, { serverId: t.server.id, csrf })).statusCode).toBe(503);
  });

  it('rate limit: 10 tickets/min por usuário (11º → 429); outro usuário segue', async () => {
    const t = await setup({ config: { rateLimit: true } });
    for (let i = 0; i < 10; i++) expect((await login(t, { ip: `198.51.100.${i}` })).statusCode).toBe(303);
    const r = await login(t, { ip: '198.51.100.99' });
    expect(r.statusCode).toBe(429);
    expect(r.headers.location).toBeUndefined();
    const other = t.store.createSession(t.owner.user.id, 'panel', T0).access;
    expect((await login(t, { cookies: { cbmp_session: other }, ip: '198.51.100.98' })).statusCode).toBe(303);
    t.clock.t += 120; // duas janelas depois
    await t.beat({ ...hb, adminUrl: ADMIN }); // continua online
    expect((await login(t, { ip: '198.51.100.97' })).statusCode).toBe(303);
  });

  it('rate limit por IP: 30/min por IP (IPv6 agrupado por /64), mesmo sem sessão', async () => {
    const t = await setup({ config: { rateLimit: true } });
    for (let i = 0; i < 30; i++) expect((await view(t, t.server.id, { cookies: {}, ip: '198.51.100.7' })).statusCode).toBe(302);
    expect((await view(t, t.server.id, { cookies: {}, ip: '198.51.100.7' })).statusCode).toBe(429);
    expect((await view(t, t.server.id, { cookies: {}, ip: '198.51.100.8' })).statusCode).toBe(302);
    for (let i = 0; i < 30; i++) expect((await view(t, t.server.id, { cookies: {}, ip: `2001:db8:1:2::${i + 1}` })).statusCode).toBe(302);
    expect((await view(t, t.server.id, { cookies: {}, ip: '2001:db8:1:2:ffff::9' })).statusCode).toBe(429);
    expect((await view(t, t.server.id, { cookies: {}, ip: '2001:db8:1:3::1' })).statusCode).toBe(302);
  });

  it('auditoria no log do Hub mesmo com HUB_LOG_LEVEL=error: did, serverId e resultado; nunca o ticket nem o token', async () => {
    const t = await setup({ adminUrl: null, config: { logLevel: 'error' } });
    await view(t, t.server.id);
    await t.beat({ ...hb, adminUrl: ADMIN });
    const page = await view(t, t.server.id);
    const csrf = csrfFrom(page.body)!;
    const ok = await confirm(t, { serverId: t.server.id, csrf });
    const ticket = ticketFrom(ok.headers.location as string);
    await confirm(t, { serverId: t.server.id, csrf }, { origin: 'https://evil.example' });
    await view(t, 'srv_AAAAAAAAAAAA');
    await view(t, t.server.id, { cookies: {} });
    const a = audits(t);
    expect(a.map((x) => [x.did, x.serverId, x.result])).toEqual([
      ['555', t.server.id, 'no_admin_url'],
      ['555', t.server.id, 'confirm_shown'],
      ['555', t.server.id, 'ticket_issued'],
      [null, t.server.id, 'bad_origin'],
      ['555', 'srv_AAAAAAAAAAAA', 'not_found'],
      [null, t.server.id, 'login_required'],
    ]);
    const all = t.lines.join('\n');
    expect(all).not.toContain(ticket);
    expect(all).not.toContain(ticket.split('.')[1]!);
    expect(all).not.toContain(csrf.split('.')[1]!);
  });
});
