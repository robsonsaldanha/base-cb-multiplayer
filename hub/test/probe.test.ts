import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { createServer, type Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { httpProbe, normalizeIp } from '../src/probe';

let srv: Server;
let port: number;
beforeAll(async () => {
  srv = createServer((req, res) => {
    if (req.url === '/info') {
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ serverId: 'srv_ok', name: 'X', players: 0, maxPlayers: 10, protocol: 1, version: '1' }));
    } else { res.statusCode = 404; res.end(); }
  });
  await new Promise<void>((r) => srv.listen(0, '127.0.0.1', r));
  port = (srv.address() as AddressInfo).port;
});
afterAll(() => srv.close());

describe('probe', () => {
  it('lê serverId de /info', async () => {
    expect(await httpProbe('127.0.0.1', port)).toEqual({ serverId: 'srv_ok' });
  });
  it('porta fechada → null', async () => {
    expect(await httpProbe('127.0.0.1', 1)).toBeNull();
  });
  it('normaliza IPv4 mapeado em IPv6', () => {
    expect(normalizeIp('::ffff:203.0.113.7')).toBe('203.0.113.7');
    expect(normalizeIp('2001:db8::1')).toBe('2001:db8::1');
  });
});
