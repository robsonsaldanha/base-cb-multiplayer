import { describe, expect, it } from 'vitest';
import { ExchangeRequest, HeartbeatBody, RefreshRequest, ResourceManifest, ServerDetail, TokenResponse } from '../src/index';

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

describe('HeartbeatBody.name', () => {
  it('apara espaços antes de validar tamanho', () => {
    expect(HeartbeatBody.parse({ ...hb, name: '  Castelo  ' }).name).toBe('Castelo');
    expect(HeartbeatBody.safeParse({ ...hb, name: '   ab   ' }).success).toBe(false);
  });
});

describe('ServerDetail', () => {
  const base = { id: 'srv_x', name: 'Castelo', description: '', tags: ['rp'], locale: 'pt-BR' };
  it('offline: só campos básicos', () => {
    expect(ServerDetail.parse({ ...base, online: false })).toEqual({ ...base, online: false });
  });
  it('online: exige campos de conexão, incluindo protocol', () => {
    const full = { ...base, online: true, players: 1, maxPlayers: 10, host: '2001:db8::1', gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1 };
    expect(ServerDetail.parse(full)).toEqual(full);
    const { protocol: _p, ...semProtocol } = full;
    expect(ServerDetail.safeParse(semProtocol).success).toBe(false);
  });
});

describe('auth do launcher', () => {
  it('ExchangeRequest exige code e codeVerifier PKCE', () => {
    expect(ExchangeRequest.safeParse({ code: 'c', codeVerifier: 'a'.repeat(43) }).success).toBe(true);
    expect(ExchangeRequest.safeParse({ code: 'c', codeVerifier: 'curto' }).success).toBe(false);
    expect(ExchangeRequest.safeParse({ code: '', codeVerifier: 'a'.repeat(43) }).success).toBe(false);
  });
  it('TokenResponse e RefreshRequest', () => {
    const t = { accessToken: 'at_x', accessExpiresAt: 1, refreshToken: 'rt_x', refreshExpiresAt: 2 };
    expect(TokenResponse.parse(t)).toEqual(t);
    expect(TokenResponse.safeParse({ ...t, accessExpiresAt: 'x' }).success).toBe(false);
    expect(RefreshRequest.safeParse({ refreshToken: 'rt_x' }).success).toBe(true);
    expect(RefreshRequest.safeParse({}).success).toBe(false);
  });
});
