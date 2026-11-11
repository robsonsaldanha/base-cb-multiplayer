import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { ExchangeRequest, HeartbeatBody, normalizeAdminOrigin, RefreshRequest, ResourceManifest, ServerDetail, ServerListing, TokenResponse } from '../src/index';

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
  const base = { id: 'srv_x', name: 'Castelo', description: '', tags: ['rp'], locale: 'pt-BR', featured: false };
  it('offline: só campos básicos', () => {
    expect(ServerDetail.parse({ ...base, online: false })).toEqual({ ...base, online: false });
  });
  it('online: exige campos de conexão, incluindo protocol', () => {
    const full = { ...base, online: true, players: 1, maxPlayers: 10, host: '2001:db8::1', gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1 };
    expect(ServerDetail.parse(full)).toEqual(full);
    const { protocol: _p, ...semProtocol } = full;
    expect(ServerDetail.safeParse(semProtocol).success).toBe(false);
  });
  it('featured é obrigatório (offline e online)', () => {
    const { featured: _f, ...semFeatured } = base;
    expect(ServerDetail.safeParse({ ...semFeatured, online: false }).success).toBe(false);
    expect(ServerDetail.parse({ ...base, featured: true, online: false }).featured).toBe(true);
  });
});

describe('ServerListing', () => {
  const listing = { id: 'srv_x', name: 'Castelo', description: '', tags: [], locale: 'pt-BR', players: 1, maxPlayers: 10,
    host: '203.0.113.7', gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1, featured: true };
  it('traz featured (booleano obrigatório)', () => {
    expect(ServerListing.parse(listing)).toEqual(listing);
    const { featured: _f, ...semFeatured } = listing;
    expect(ServerListing.safeParse(semFeatured).success).toBe(false);
    expect(ServerListing.safeParse({ ...listing, featured: 1 }).success).toBe(false);
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

describe('HeartbeatBody.adminUrl', () => {
  const parse = (adminUrl: unknown) => HeartbeatBody.safeParse({ ...hb, adminUrl });
  it('é opcional', () => {
    expect(HeartbeatBody.parse(hb).adminUrl).toBeUndefined();
  });
  it('normaliza para a origem (scheme://host[:porta]), minúsculas e sem porta padrão', () => {
    expect(HeartbeatBody.parse({ ...hb, adminUrl: 'https://Painel.Example.com' }).adminUrl).toBe('https://painel.example.com');
    expect(HeartbeatBody.parse({ ...hb, adminUrl: 'https://painel.example.com/' }).adminUrl).toBe('https://painel.example.com');
    expect(HeartbeatBody.parse({ ...hb, adminUrl: 'https://painel.example.com:443' }).adminUrl).toBe('https://painel.example.com');
    expect(HeartbeatBody.parse({ ...hb, adminUrl: 'https://painel.example.com:8443/' }).adminUrl).toBe('https://painel.example.com:8443');
    expect(HeartbeatBody.parse({ ...hb, adminUrl: 'HTTPS://203.0.113.5:30510' }).adminUrl).toBe('https://203.0.113.5:30510');
  });
  it('http só para loopback (127.0.0.0/8, ::1, localhost)', () => {
    expect(HeartbeatBody.parse({ ...hb, adminUrl: 'http://127.0.0.1:30510' }).adminUrl).toBe('http://127.0.0.1:30510');
    expect(HeartbeatBody.parse({ ...hb, adminUrl: 'http://127.8.9.10:30510' }).adminUrl).toBe('http://127.8.9.10:30510');
    expect(HeartbeatBody.parse({ ...hb, adminUrl: 'http://[::1]:30510' }).adminUrl).toBe('http://[::1]:30510');
    expect(HeartbeatBody.parse({ ...hb, adminUrl: 'http://localhost:30510' }).adminUrl).toBe('http://localhost:30510');
    for (const u of ['http://painel.example.com', 'http://203.0.113.5:30510', 'http://10.0.0.1', 'http://localhost.example.com',
      'http://127.0.0.1.nip.io', 'http://[::2]']) {
      expect(parse(u).success, u).toBe(false);
    }
  });
  it('recusa caminho, query, fragmento, credenciais, espaços e controles', () => {
    for (const u of [
      'https://painel.example.com/admin', 'https://painel.example.com/auth/callback', 'https://painel.example.com?x=1',
      'https://painel.example.com/?', 'https://painel.example.com#frag', 'https://painel.example.com/#', 'https://user:pw@painel.example.com',
      'https://user@painel.example.com', 'https://evil.com@painel.example.com', 'https://painel.example.com\@evil.com',
      ' https://painel.example.com', 'https://painel.example.com ', 'https://painel .example.com', 'https://painel.example.com\t',
      'https://painel.example.com\n', 'https://painel.example.com\u0000', 'https://painel.example.com/.', 'https://painel.example.com//',
      'javascript:alert(1)', 'ftp://painel.example.com', 'https://', 'https:///x', '//painel.example.com', 'painel.example.com',
      'https://painel.example.com:0', 'https://painel.example.com:65536', 'https://painel.exámple.com',
      `https://${'a'.repeat(250)}.com`, 42, '',
    ]) {
      expect(parse(u).success, JSON.stringify(u)).toBe(false);
    }
  });
});

describe('normalizeAdminOrigin: vetores compartilhados com o C++', () => {
  const v = JSON.parse(readFileSync(new URL('../test-vectors/admin-origin.json', import.meta.url), 'utf8')) as { accept: [string, string][]; reject: string[] };
  it('aceita e normaliza igual ao C++', () => {
    for (const [input, out] of v.accept) expect(normalizeAdminOrigin(input), input).toBe(out);
  });
  it('recusa igual ao C++', () => {
    for (const input of v.reject) expect(normalizeAdminOrigin(input), JSON.stringify(input)).toBeNull();
  });
  it('a saída é sempre a origem WHATWG (o que o navegador usa)', () => {
    for (const [, out] of v.accept) expect(new URL(out).origin).toBe(out);
  });
});
