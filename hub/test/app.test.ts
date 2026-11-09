import { describe, expect, it } from 'vitest';
import { makeTestApp } from './helpers';

describe('app', () => {
  it('GET /health', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/health' });
    expect(res.statusCode).toBe(200);
    expect(res.json()).toEqual({ ok: true });
  });
  it('404 no formato de erro padrão', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/nao-existe' });
    expect(res.statusCode).toBe(404);
    expect(res.json()).toEqual({ error: 'not_found', message: 'rota não encontrada' });
  });
});

describe('trustProxy', () => {
  const hb = { name: 'Servidor', players: 1, maxPlayers: 10, gamePort: 30500, httpPort: 30500, version: '1', protocol: 1, tags: [] };
  async function run(remoteAddress: string) {
    const { makeTestApp, loginAs, T0 } = await import('./helpers');
    const t = await makeTestApp({ config: { trustProxy: '127.0.0.1' } });
    const o = loginAs(t.store, '1');
    const { server, key } = t.store.createServer(o.user.id, { name: 'S', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
    const res = await t.app.inject({ method: 'POST', url: '/v1/servers/heartbeat', headers: { authorization: `Bearer ${key}`, 'x-forwarded-for': '203.0.113.9' }, payload: hb, remoteAddress });
    expect(res.statusCode).toBe(200);
    return t.store.getStatus(server.id)!.ip;
  }
  it('proxy confiável: usa X-Forwarded-For', async () => { expect(await run('127.0.0.1')).toBe('203.0.113.9'); });
  it('origem não confiável: ignora X-Forwarded-For', async () => { expect(await run('198.51.100.1')).toBe('198.51.100.1'); });
});
