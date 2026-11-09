import { describe, expect, it } from 'vitest';
import { generateKeyPairSync } from 'node:crypto';
import { mkdtempSync, readdirSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { generatePrivateKey, publicKeyToRaw, signTicket, verifyTicket, publicKeyFromRaw, TICKET_TTL_SECONDS } from '@cbmp/contracts';
import { loadOrCreateKeyRing } from '../src/keyring';

describe('keyring', () => {
  it('cria uma chave na primeira vez e reusa depois', () => {
    const dir = mkdtempSync(join(tmpdir(), 'cbmp-keys-'));
    const a = loadOrCreateKeyRing(dir, new Date('2026-11-09T12:00:00Z'));
    expect(a.active.kid).toBe('k20261109120000');
    expect(readdirSync(dir)).toEqual(['k20261109120000.pem']);
    const b = loadOrCreateKeyRing(dir);
    expect(b.publicKeys()).toEqual(a.publicKeys());
  });
  it('a chave mais nova (por nome) é a ativa e as antigas continuam publicadas', () => {
    const dir = mkdtempSync(join(tmpdir(), 'cbmp-keys-'));
    loadOrCreateKeyRing(dir, new Date('2026-01-01T00:00:00Z'));
    const nova = generatePrivateKey();
    writeFileSync(join(dir, 'k20270101000000.pem'), nova.export({ format: 'pem', type: 'pkcs8' }));
    const ring = loadOrCreateKeyRing(dir);
    expect(ring.active.kid).toBe('k20270101000000');
    expect(ring.publicKeys().map((k) => k.kid)).toEqual(['k20260101000000', 'k20270101000000']);
    const t = signTicket({ v: 1, kid: ring.active.kid, sub: 's', did: 'd', name: 'n', aud: 'a', iat: 1, exp: 1 + TICKET_TTL_SECONDS, jti: 'j' }, ring.active.privateKey);
    const keys = new Map(ring.publicKeys().map((k) => [k.kid, publicKeyFromRaw(k.publicKey)] as const));
    expect(verifyTicket(t, { publicKeys: keys, audience: 'a', nowSeconds: 2 }).ok).toBe(true);
    expect(ring.publicKeys()[1]!.publicKey).toBe(publicKeyToRaw(nova));
  });
  it('ignora arquivos fora do padrão e falha citando o arquivo corrompido', () => {
    const dir = mkdtempSync(join(tmpdir(), 'cbmp-keys-'));
    const a = loadOrCreateKeyRing(dir, new Date('2026-01-01T00:00:00Z'));
    writeFileSync(join(dir, 'zz-backup.pem'), 'lixo');
    const b = loadOrCreateKeyRing(dir);
    expect(b.active.kid).toBe(a.active.kid);
    expect(b.publicKeys().map((k) => k.kid)).toEqual([a.active.kid]);
    writeFileSync(join(dir, 'k20990101000000.pem'), 'corrompida');
    expect(() => loadOrCreateKeyRing(dir)).toThrow(/k20990101000000\.pem/);
  });
  it('recusa chave que não é Ed25519, citando o arquivo', () => {
    const dir = mkdtempSync(join(tmpdir(), 'cbmp-keys-'));
    const ec = generateKeyPairSync('ec', { namedCurve: 'prime256v1' }).privateKey;
    writeFileSync(join(dir, 'k20990101000000.pem'), ec.export({ format: 'pem', type: 'pkcs8' }));
    expect(() => loadOrCreateKeyRing(dir)).toThrow(/k20990101000000\.pem.*ed25519/i);
  });
});
