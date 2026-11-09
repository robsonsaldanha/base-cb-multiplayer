import { describe, expect, it } from 'vitest';
import { HeartbeatBody, ResourceManifest } from '../src/index';

const hb = { name: 'Castelo Bruxo', players: 10, maxPlayers: 512, gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1 };

describe('HeartbeatBody', () => {
  it('aplica defaults', () => {
    expect(HeartbeatBody.parse(hb)).toEqual({ ...hb, description: '', tags: [], locale: 'pt-BR' });
  });
  it('rejeita players > maxPlayers', () => {
    expect(HeartbeatBody.safeParse({ ...hb, players: 600 }).success).toBe(false);
  });
  it('rejeita tag inválida e porta fora da faixa', () => {
    expect(HeartbeatBody.safeParse({ ...hb, tags: ['Com Espaço'] }).success).toBe(false);
    expect(HeartbeatBody.safeParse({ ...hb, gamePort: 70000 }).success).toBe(false);
  });
});

describe('ResourceManifest', () => {
  it('exige sha256 hex de 64 chars', () => {
    const m = { serverId: 'srv_x', protocol: 1, totalSize: 1, resources: [{ name: 'a', version: '1', files: [{ path: 'client/a.lua', sha256: 'z', size: 1 }] }] };
    expect(ResourceManifest.safeParse(m).success).toBe(false);
    m.resources[0]!.files[0]!.sha256 = 'a'.repeat(64);
    expect(ResourceManifest.safeParse(m).success).toBe(true);
  });
});
