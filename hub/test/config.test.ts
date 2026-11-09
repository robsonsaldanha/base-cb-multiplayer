import { describe, expect, it } from 'vitest';
import { loadConfig } from '../src/config';

const base = { HUB_DEV_LOGIN: '1' };

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
});
