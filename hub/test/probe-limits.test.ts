import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { createServer, type Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { httpProbe } from '../src/probe';

const info = JSON.stringify({ serverId: 'srv_ok', name: 'X', players: 0, maxPlayers: 10, protocol: 1, version: '1' });
let bad: Server;
let port: number;
beforeAll(async () => {
  bad = createServer((req, res) => {
    if (req.url === '/info' && req.headers['x-case'] === 'big') {
      res.setHeader('content-type', 'application/json'); res.end(JSON.stringify({ serverId: 'srv_ok', name: 'X', players: 0, maxPlayers: 10, protocol: 1, version: '1', pad: 'x'.repeat(20 * 1024) }));
    } else if (req.headers['x-case'] === 'chunked') {
      res.setHeader('content-type', 'application/json'); res.write('{"pad":"');
      let sent = 0;
      const t = setInterval(() => { if (sent < 20 * 1024) { res.write('x'.repeat(4096)); sent += 4096; } }, 1);
      res.on('close', () => clearInterval(t));
    } else { res.setHeader('content-type', 'text/plain'); res.end(info); }
  });
  await new Promise<void>((r) => bad.listen(0, '127.0.0.1', r));
  port = (bad.address() as AddressInfo).port;
});
afterAll(() => bad.close());

/** O probe não aceita headers; usamos o caminho via fetch global com cabeçalho extra. */
async function withCase(c: string) {
  const orig = globalThis.fetch;
  globalThis.fetch = ((u: string, o: RequestInit = {}) => orig(u, { ...o, headers: { 'x-case': c } })) as typeof fetch;
  try { return await httpProbe('127.0.0.1', port); } finally { globalThis.fetch = orig; }
}

describe('probe: limites do corpo', () => {
  it('corpo de 20 KiB (content-length) → null', async () => { expect(await withCase('big')).toBeNull(); });
  it('corpo em chunks acima de 16 KiB → null em < 1 s (cap, não timeout)', async () => {
    const t0 = Date.now();
    expect(await withCase('chunked')).toBeNull();
    expect(Date.now() - t0).toBeLessThan(1000);
  });
  it('content-type errado → null', async () => { expect(await withCase('plain')).toBeNull(); });
});
