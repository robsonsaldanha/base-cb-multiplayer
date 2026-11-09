import { describe, expect, it } from 'vitest';
import { makeTestApp, loginAs, T0 } from './helpers';

const hb = { name: 'Castelo Bruxo', players: 12, maxPlayers: 512, gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1, tags: ['rp'] };

type Probe = (h: string, p: number) => Promise<{ serverId: string } | null>;
async function mk(probe: Probe, config = {}) {
  const t = await makeTestApp({ probe, config });
  const owner = loginAs(t.store, '1');
  const { server, key } = t.store.createServer(owner.user.id, { name: 'I', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
  return { ...t, server, key };
}
const from = (app: Awaited<ReturnType<typeof mk>>['app'], key: string, ip: string, body: object = hb, authorization = `Bearer ${key}`) =>
  app.inject({ method: 'POST', url: '/v1/servers/heartbeat', headers: { authorization }, payload: body, remoteAddress: ip });

describe('endurecimento do heartbeat', () => {
  it('IP privado não é sondado quando allowPrivateServers=false', async () => {
    let calls = 0;
    const t = await mk(async () => { calls++; return null; }, { allowPrivateServers: false });
    const r = await from(t.app, t.key, '10.0.0.5');
    expect(r.json()).toEqual({ ok: true, listed: false, nextHeartbeatSeconds: 30, reason: 'private_ip' });
    expect(calls).toBe(0);
    expect(t.store.getStatus(t.server.id)!.verified).toBe(false);
  });

  it('IP público é sondado com allowPrivateServers=false', async () => {
    let id = '';
    const t = await mk(async () => ({ serverId: id }), { allowPrivateServers: false });
    id = t.server.id;
    expect((await from(t.app, t.key, '8.8.8.8')).json().listed).toBe(true);
  });

  it('Bearer sk_x e "Bearer   sk_x" dividem o mesmo balde (11º → 429)', async () => {
    const t = await mk(async () => null, { rateLimit: true });
    for (let i = 0; i < 10; i++) {
      const authorization = i % 2 ? `Bearer   ${t.key}` : `Bearer ${t.key}`;
      expect((await from(t.app, t.key, '203.0.113.7', hb, authorization)).statusCode).toBe(200);
    }
    expect((await from(t.app, t.key, '203.0.113.7')).statusCode).toBe(429);
  });

  it('probe em andamento para o mesmo servidor não é duplicado', async () => {
    let calls = 0; let release!: () => void;
    const gate = new Promise<void>((r) => { release = r; });
    let id = '';
    const t = await mk(async () => { calls++; await gate; return { serverId: id }; });
    id = t.server.id;
    const a = from(t.app, t.key, '203.0.113.7');
    const b = from(t.app, t.key, '203.0.113.7');
    await new Promise((r) => setTimeout(r, 30));
    release();
    const [ra, rb] = await Promise.all([a, b]);
    expect(calls).toBe(1);
    expect(ra.json().listed).toBe(true);
    expect(rb.json().listed).toBe(true);
  });

  it('mudança de IP limpa verified antes do probe e re-sonda', async () => {
    let id = ''; let calls = 0; let verifiedDuringProbe: boolean | undefined; let fail = false;
    const t = await mk(async () => { calls++; verifiedDuringProbe = t.store.getStatus(id)!.verified; return fail ? null : { serverId: id }; });
    id = t.server.id;
    expect((await from(t.app, t.key, '203.0.113.7')).json().listed).toBe(true);
    t.clock.t += 30; fail = true;
    const r = await from(t.app, t.key, '203.0.113.8');
    expect(calls).toBe(2);
    expect(verifiedDuringProbe).toBe(false);
    expect(r.json().listed).toBe(false);
    expect(t.store.listOnline(t.clock.t)).toHaveLength(0);
  });

  it('mudança de porta HTTP re-sonda', async () => {
    let id = ''; let calls = 0; let verifiedDuringProbe: boolean | undefined;
    const t = await mk(async () => { calls++; verifiedDuringProbe = t.store.getStatus(id)!.verified; return { serverId: id }; });
    id = t.server.id;
    await from(t.app, t.key, '203.0.113.7');
    t.clock.t += 30; await from(t.app, t.key, '203.0.113.7');
    expect(calls).toBe(1);
    t.clock.t += 30; await from(t.app, t.key, '203.0.113.7', { ...hb, httpPort: 30501 });
    expect(calls).toBe(2);
    expect(verifiedDuringProbe).toBe(false);
  });

  it('back-off: após probe falho, não re-sonda por 120 s no mesmo endpoint', async () => {
    let calls = 0;
    const t = await mk(async () => { calls++; return null; });
    await from(t.app, t.key, '203.0.113.7');
    t.clock.t += 30;
    const r = await from(t.app, t.key, '203.0.113.7');
    t.clock.t += 60;
    await from(t.app, t.key, '203.0.113.7');
    expect(calls).toBe(1);
    expect(r.json()).toEqual({ ok: true, listed: false, nextHeartbeatSeconds: 30, reason: 'probe_failed' });
    t.clock.t += 30; // 120 s desde a falha
    await from(t.app, t.key, '203.0.113.7');
    expect(calls).toBe(2);
  });

  it("probe de A em voo não verifica o endpoint B (IPs diferentes, mesmo servidor)", async () => {
    let id = "";
    const t = await mk(async (h) => {
      if (h === "203.0.113.7") { await new Promise((r) => setTimeout(r, 60)); return { serverId: id }; }
      return null;
    });
    id = t.server.id;
    const a = from(t.app, t.key, "203.0.113.7");
    await new Promise((r) => setTimeout(r, 15));
    const b = from(t.app, t.key, "203.0.113.8");
    const [, rb] = await Promise.all([a, b]);
    expect(rb.json().listed).toBe(false);
    expect(t.store.listOnline(t.clock.t)).toHaveLength(0);
  });

  it("probe lento de A não lista depois um endpoint privado", async () => {
    let id = "";
    const t = await mk(async () => { await new Promise((r) => setTimeout(r, 60)); return { serverId: id }; }, { allowPrivateServers: false });
    id = t.server.id;
    const a = from(t.app, t.key, "8.8.8.8");
    await new Promise((r) => setTimeout(r, 15));
    const priv = await from(t.app, t.key, "10.0.0.5");
    expect(priv.json().reason).toBe("private_ip");
    await a;
    expect(t.store.listOnline(t.clock.t)).toHaveLength(0);
  });
});
