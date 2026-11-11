import { PassThrough } from 'node:stream';
import Database from 'better-sqlite3';
import { describe, expect, it } from 'vitest';
import { ServerDetail, ServerListing } from '@cbmp/contracts';
import { makeTestApp, loginAs, T0 } from './helpers';
import { migrate } from '../src/db';

const hb = { name: 'Srv', players: 0, maxPlayers: 512, gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1 };

async function setup() {
  const lines: string[] = [];
  const logStream = new PassThrough();
  logStream.on('data', (b: Buffer) => lines.push(...b.toString('utf8').split('\n').filter(Boolean)));
  const t = await makeTestApp({ logStream });
  const owner = loginAs(t.store, '1');
  const admin = loginAs(t.store, '999'); // platformAdmins: ['999'] em testConfig
  let ip = 10;
  /** Cria um servidor e o põe online (heartbeat + probe ok) com o nome e jogadores dados. */
  const online = async (name: string, players: number) => {
    const { server, key } = t.store.createServer(owner.user.id, { name, description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
    const addr = `203.0.113.${ip++}`;
    t.probeAnswers.set(`${addr}:30500`, server.id);
    const r = await t.app.inject({
      method: 'POST', url: '/v1/servers/heartbeat', headers: { authorization: `Bearer ${key}` },
      payload: { ...hb, name, players }, remoteAddress: addr,
    });
    expect(r.json().listed).toBe(true);
    return server;
  };
  const patch = (id: string, body: unknown, headers: Record<string, string> = admin.auth) =>
    t.app.inject({ method: 'PATCH', url: `/v1/admin/servers/${id}`, headers, payload: body as object });
  const list = async () => {
    t.clock.t += 5; // vence o cache de 5 s da lista
    return (await t.app.inject({ method: 'GET', url: '/v1/servers' })).json() as ServerListing[];
  };
  const audits = () => lines.map((l) => JSON.parse(l) as Record<string, unknown>).filter((l) => l.audit === 'platform_admin');
  return { ...t, owner, admin, online, patch, list, audits };
}

describe('servidores em destaque (featured)', () => {
  it('servidor novo nasce com featured false; listagem e detalhe trazem o campo (contrato)', async () => {
    const t = await setup();
    const s = await t.online('Alfa', 3);
    expect(t.store.getServer(s.id)!.featured).toBe(false);
    const l = await t.list();
    expect(l).toHaveLength(1);
    expect(ServerListing.parse(l[0])).toEqual(l[0]);
    expect(l[0]!.featured).toBe(false);
    const d = (await t.app.inject({ method: 'GET', url: `/v1/servers/${s.id}` })).json();
    expect(ServerDetail.parse(d)).toEqual(d);
    expect(d).toMatchObject({ online: true, featured: false });
    // offline também traz featured
    const { server: off } = t.store.createServer(t.owner.user.id, { name: 'Offline', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
    const od = (await t.app.inject({ method: 'GET', url: `/v1/servers/${off.id}` })).json();
    expect(ServerDetail.parse(od)).toEqual(od);
    expect(od).toEqual({ id: off.id, name: 'Offline', description: '', tags: [], locale: 'pt-BR', featured: false, online: false });
  });

  it('GET /v1/servers: destacados primeiro; desempate por jogadores desc e depois nome', async () => {
    const t = await setup();
    const a = await t.online('Bravo', 5);
    const b = await t.online('Alfa', 5);
    const c = await t.online('Charlie', 50);
    const d = await t.online('Delta', 1);
    const e = await t.online('Echo', 1);
    expect((await t.list()).map((s) => s.name)).toEqual(['Charlie', 'Alfa', 'Bravo', 'Delta', 'Echo']);
    for (const s of [d, e, a]) expect((await t.patch(s.id, { featured: true })).statusCode).toBe(200);
    const l = await t.list();
    expect(l.map((s) => s.name)).toEqual(['Bravo', 'Delta', 'Echo', 'Charlie', 'Alfa']);
    expect(l.map((s) => s.featured)).toEqual([true, true, true, false, false]);
    expect([b.id, c.id].every((id) => !t.store.getServer(id)!.featured)).toBe(true);
    // desligar volta à ordem normal
    expect((await t.patch(a.id, { featured: false })).json()).toEqual({ id: a.id, featured: false });
    expect((await t.list()).map((s) => s.name)).toEqual(['Delta', 'Echo', 'Charlie', 'Alfa', 'Bravo']);
  });

  it('PATCH /v1/admin/servers/:id exige sessão de admin da plataforma (401 sem login, 403 usuário comum)', async () => {
    const t = await setup();
    const s = await t.online('Alfa', 1);
    expect((await t.patch(s.id, { featured: true }, {})).statusCode).toBe(401);
    const r = await t.patch(s.id, { featured: true }, t.owner.auth); // nem o dono do servidor
    expect(r.statusCode).toBe(403);
    expect(r.json().error).toBe('forbidden');
    expect(t.store.getServer(s.id)!.featured).toBe(false);
    expect(t.audits()).toEqual([]);
    const ok = await t.patch(s.id, { featured: true });
    expect(ok.statusCode).toBe(200);
    expect(ok.json()).toEqual({ id: s.id, featured: true });
    expect(t.store.getServer(s.id)!.featured).toBe(true);
  });

  it('PATCH: servidor inexistente → 404; corpo inválido → 400', async () => {
    const t = await setup();
    const s = await t.online('Alfa', 1);
    const nf = await t.patch('srv_naoexiste12', { featured: true });
    expect(nf.statusCode).toBe(404);
    expect(nf.json().error).toBe('not_found');
    for (const body of [{}, { featured: 'true' }, { featured: 1 }, { featured: true, name: 'x' }]) {
      expect((await t.patch(s.id, body)).statusCode, JSON.stringify(body)).toBe(400);
    }
    expect(t.store.getServer(s.id)!.featured).toBe(false);
  });

  it('PATCH é auditado com did do admin, serverId e valor (log do Hub, nível próprio)', async () => {
    const t = await setup();
    const s = await t.online('Alfa', 1);
    await t.patch(s.id, { featured: true });
    await t.patch('srv_naoexiste12', { featured: true });
    expect(t.audits().map(({ did, serverId, featured, result }) => ({ did, serverId, featured, result }))).toEqual([
      { did: '999', serverId: s.id, featured: true, result: 'ok' },
      { did: '999', serverId: 'srv_naoexiste12', featured: true, result: 'not_found' },
    ]);
  });

  it('dono não liga o destaque pelo PATCH de dono (campo ignorado)', async () => {
    const t = await setup();
    const s = await t.online('Alfa', 1);
    const r = await t.app.inject({ method: 'PATCH', url: `/v1/owner/servers/${s.id}`, headers: t.owner.auth, payload: { featured: true } });
    expect(r.statusCode).toBeLessThan(500);
    expect(t.store.getServer(s.id)!.featured).toBe(false);
  });

  it('migração de featured é idempotente (rodar de novo não quebra nem duplica)', () => {
    const db = new Database(':memory:');
    migrate(db);
    const cols = () => (db.prepare("PRAGMA table_info('servers')").all() as { name: string; dflt_value: string; notnull: number }[])
      .filter((c) => c.name === 'featured');
    expect(cols()).toEqual([expect.objectContaining({ name: 'featured', dflt_value: '0', notnull: 1 })]);
    const v = db.pragma('user_version', { simple: true }) as number;
    db.pragma(`user_version = ${v - 1}`); // simula a migração reaplicada
    expect(() => migrate(db)).not.toThrow();
    expect(cols()).toHaveLength(1);
    expect(db.pragma('user_version', { simple: true })).toBe(v);
    db.close();
  });
});
