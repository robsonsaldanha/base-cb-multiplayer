import { describe, expect, it } from 'vitest';
import { loadConfig } from '../src/config';

const base = { DISCORD_CLIENT_ID: 'cid', DISCORD_CLIENT_SECRET: 'secret' };
const dev = { HUB_DEV_LOGIN: '1' };

describe('config', () => {
  it('trustProxy: vazio/0 → false; lista de IPs/CIDRs → string', () => {
    expect(loadConfig({ ...base }).trustProxy).toBe(false);
    expect(loadConfig({ ...base, HUB_TRUST_PROXY: '0' }).trustProxy).toBe(false);
    expect(loadConfig({ ...base, HUB_TRUST_PROXY: '127.0.0.1, ::1' }).trustProxy).toBe('127.0.0.1,::1');
    expect(loadConfig({ ...base, HUB_TRUST_PROXY: '10.0.0.0/8' }).trustProxy).toBe('10.0.0.0/8');
  });
  it('HUB_TRUST_PROXY=1 (forma booleana antiga) é rejeitado', () => {
    expect(() => loadConfig({ ...base, HUB_TRUST_PROXY: '1' })).toThrow(/HUB_TRUST_PROXY/);
    expect(() => loadConfig({ ...base, HUB_TRUST_PROXY: 'true' })).toThrow(/HUB_TRUST_PROXY/);
  });
  it('allowPrivateServers: padrão false', () => {
    expect(loadConfig({ ...base }).allowPrivateServers).toBe(false);
    expect(loadConfig({ ...base, HUB_ALLOW_PRIVATE_SERVERS: '1' }).allowPrivateServers).toBe(true);
  });
  describe('HUB_DEV_LOGIN=1', () => {
    it('permitido em host local, publicUrl local e sem proxy', () => {
      expect(loadConfig({ ...dev }).devLogin).toBe(true);
      expect(loadConfig({ ...dev, HUB_HOST: 'localhost', HUB_PUBLIC_URL: 'http://127.0.0.1:8787' }).devLogin).toBe(true);
      expect(loadConfig({ ...dev, HUB_HOST: '::1', HUB_PUBLIC_URL: 'http://[::1]:8787/' }).devLogin).toBe(true);
    });
    it('recusado com HUB_HOST não local', () => {
      expect(() => loadConfig({ ...dev, HUB_HOST: '0.0.0.0' })).toThrow(/HUB_DEV_LOGIN.*HUB_HOST/);
    });
    it('recusado com HUB_PUBLIC_URL não local', () => {
      expect(() => loadConfig({ ...dev, HUB_PUBLIC_URL: 'https://hub.castelobruxo.example' })).toThrow(/HUB_DEV_LOGIN.*HUB_PUBLIC_URL/);
      expect(() => loadConfig({ ...dev, HUB_PUBLIC_URL: 'http://localhost.evil.example' })).toThrow(/HUB_DEV_LOGIN.*HUB_PUBLIC_URL/);
    });
    it('recusado com HUB_TRUST_PROXY ligado', () => {
      expect(() => loadConfig({ ...dev, HUB_TRUST_PROXY: '127.0.0.1' })).toThrow(/HUB_DEV_LOGIN.*HUB_TRUST_PROXY/);
    });
  });
});
