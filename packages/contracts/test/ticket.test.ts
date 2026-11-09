import { describe, expect, it } from 'vitest';
import { createPublicKey, sign } from 'node:crypto';
import {
  CLOCK_SKEW_SECONDS, TICKET_TTL_SECONDS, privateKeyFromSeed, publicKeyFromRaw, publicKeyToRaw,
  signTicket, verifyTicket, type TicketPayload,
} from '../src/index';

const seed = Buffer.alloc(32, 7);
const priv = privateKeyFromSeed(seed);
const pubRaw = publicKeyToRaw(createPublicKey(priv));
const keys = new Map([['k1', publicKeyFromRaw(pubRaw)]]);
const base: TicketPayload = {
  v: 1, kid: 'k1', sub: 'u_1', did: '123456789012345678', name: 'Robson', aud: 'srv_abc',
  iat: 1_800_000_000, exp: 1_800_000_000 + TICKET_TTL_SECONDS, jti: '00000000-0000-4000-8000-000000000001',
};

describe('ticket', () => {
  it('chave pública raw tem 32 bytes', () => {
    expect(Buffer.from(pubRaw, 'base64url').length).toBe(32);
  });
  it('assina e valida', () => {
    const t = signTicket(base, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat + 1 }))
      .toEqual({ ok: true, payload: base });
  });
  it('assinatura é determinística', () => {
    expect(signTicket(base, priv)).toBe(signTicket(base, priv));
  });
  it('rejeita audience errada', () => {
    const t = signTicket(base, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_outro', nowSeconds: base.iat })).toEqual({ ok: false, error: 'wrong_audience' });
  });
  it('rejeita expirado além da tolerância e aceita dentro', () => {
    const t = signTicket(base, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.exp + CLOCK_SKEW_SECONDS }).ok).toBe(true);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.exp + CLOCK_SKEW_SECONDS + 1 })).toEqual({ ok: false, error: 'expired' });
  });
  it('rejeita emitido no futuro além da tolerância', () => {
    const t = signTicket(base, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat - CLOCK_SKEW_SECONDS - 1 })).toEqual({ ok: false, error: 'not_yet_valid' });
  });
  it('rejeita kid desconhecido', () => {
    const t = signTicket({ ...base, kid: 'k2' }, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat })).toEqual({ ok: false, error: 'unknown_kid' });
  });
  it('rejeita payload adulterado', () => {
    const [, sig] = signTicket(base, priv).split('.');
    const forged = Buffer.from(JSON.stringify({ ...base, name: 'Outro' })).toString('base64url') + '.' + sig;
    expect(verifyTicket(forged, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat })).toEqual({ ok: false, error: 'bad_signature' });
  });
  it('rejeita versão diferente', () => {
    const t = signTicket({ ...base, v: 2 as 1 }, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat })).toEqual({ ok: false, error: 'bad_version' });
  });
  it('rejeita base64url não canônico e tamanho inválido', () => {
    const t = signTicket(base, priv);
    const [b, sg] = t.split('.') as [string, string];
    const alt = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
    const nc = b + '.' + sg.slice(0, -1) + alt[alt.indexOf(sg.at(-1)!) ^ 1];
    expect(Buffer.from(nc.split('.')[1]!, 'base64url').equals(Buffer.from(sg, 'base64url'))).toBe(true);
    const o = { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat };
    expect(verifyTicket(nc, o)).toEqual({ ok: false, error: 'malformed' });
    let bad = b;
    while (bad.length % 4 !== 1) bad += 'A';
    expect(bad.length % 4).toBe(1);
    expect(verifyTicket(bad + '.' + sg, o)).toEqual({ ok: false, error: 'malformed' });
  });
  it('rejeita iat/exp não inteiros ou inseguros', () => {
    const o = { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat };
    expect(verifyTicket(signTicket({ ...base, iat: 1.5 }, priv), o)).toEqual({ ok: false, error: 'malformed' });
    expect(verifyTicket(signTicket({ ...base, exp: 2 ** 60 }, priv), o)).toEqual({ ok: false, error: 'malformed' });
  });
  it('rejeita literal float/expoente em iat', () => {
    const o = { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat };
    for (const lit of ['1800000000.0', '18e8']) {
      const body = Buffer.from(JSON.stringify(base).replace('"iat":1800000000', '"iat":' + lit), 'utf8');
      const t = body.toString('base64url') + '.' + sign(null, body, priv).toString('base64url');
      expect(verifyTicket(t, o)).toEqual({ ok: false, error: 'malformed' });
    }
  });
  it.each(['', 'abc', 'a.b.c', '!!.??', `${Buffer.from('não é json').toString('base64url')}.AAAA`])('rejeita malformado %j', (t) => {
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat })).toEqual({ ok: false, error: 'malformed' });
  });
});
