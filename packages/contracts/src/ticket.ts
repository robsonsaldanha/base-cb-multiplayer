import {
  createPrivateKey, createPublicKey, generateKeyPairSync, sign, verify, type KeyObject,
} from 'node:crypto';

export const TICKET_TTL_SECONDS = 300;
export const CLOCK_SKEW_SECONDS = 60;

export interface TicketPayload {
  v: 1;
  kid: string;
  sub: string;
  did: string;
  name: string;
  aud: string;
  iat: number;
  exp: number;
  jti: string;
}

export type TicketError =
  | 'malformed' | 'bad_version' | 'unknown_kid' | 'bad_signature'
  | 'wrong_audience' | 'expired' | 'not_yet_valid';

export type VerifyResult = { ok: true; payload: TicketPayload } | { ok: false; error: TicketError };

// Prefixo DER PKCS#8 de uma chave privada Ed25519; seguido dos 32 bytes da seed.
const PKCS8_ED25519_PREFIX = Buffer.from('302e020100300506032b657004220420', 'hex');
const B64URL = /^[A-Za-z0-9_-]+$/;

export function privateKeyFromSeed(seed: Buffer): KeyObject {
  if (seed.length !== 32) throw new Error('seed Ed25519 precisa ter 32 bytes');
  return createPrivateKey({ key: Buffer.concat([PKCS8_ED25519_PREFIX, seed]), format: 'der', type: 'pkcs8' });
}

export function generatePrivateKey(): KeyObject {
  return generateKeyPairSync('ed25519').privateKey;
}

/** Chave pública Ed25519 como 32 bytes raw em base64url. Aceita chave pública ou privada. */
export function publicKeyToRaw(key: KeyObject): string {
  const pub = key.type === 'private' ? createPublicKey(key) : key;
  const jwk = pub.export({ format: 'jwk' });
  if (typeof jwk.x !== 'string') throw new Error('chave não é Ed25519');
  return jwk.x;
}

export function publicKeyFromRaw(raw: string): KeyObject {
  return createPublicKey({ key: { kty: 'OKP', crv: 'Ed25519', x: raw }, format: 'jwk' });
}

export function signTicket(payload: TicketPayload, privateKey: KeyObject): string {
  const body = Buffer.from(JSON.stringify(payload), 'utf8');
  return `${body.toString('base64url')}.${sign(null, body, privateKey).toString('base64url')}`;
}

function isPayloadShape(p: unknown): p is TicketPayload {
  if (typeof p !== 'object' || p === null) return false;
  const o = p as Record<string, unknown>;
  return typeof o.v === 'number'
    && ['kid', 'sub', 'did', 'name', 'aud', 'jti'].every((k) => typeof o[k] === 'string')
    && Number.isInteger(o.iat) && Number.isInteger(o.exp);
}

/**
 * Ordem das checagens (o validador C++ deve seguir a mesma):
 * formato → JSON/forma → versão → kid → assinatura → audience → iat futuro → expiração.
 */
export function verifyTicket(
  ticket: string,
  opts: { publicKeys: ReadonlyMap<string, KeyObject>; audience: string; nowSeconds: number },
): VerifyResult {
  const parts = ticket.split('.');
  if (parts.length !== 2 || !B64URL.test(parts[0]!) || !B64URL.test(parts[1]!)) return { ok: false, error: 'malformed' };
  const body = Buffer.from(parts[0]!, 'base64url');
  const sig = Buffer.from(parts[1]!, 'base64url');
  let payload: unknown;
  try {
    payload = JSON.parse(body.toString('utf8'));
  } catch {
    return { ok: false, error: 'malformed' };
  }
  if (!isPayloadShape(payload)) return { ok: false, error: 'malformed' };
  if (payload.v !== 1) return { ok: false, error: 'bad_version' };
  const key = opts.publicKeys.get(payload.kid);
  if (!key) return { ok: false, error: 'unknown_kid' };
  if (sig.length !== 64 || !verify(null, body, key, sig)) return { ok: false, error: 'bad_signature' };
  if (payload.aud !== opts.audience) return { ok: false, error: 'wrong_audience' };
  if (payload.iat > opts.nowSeconds + CLOCK_SKEW_SECONDS) return { ok: false, error: 'not_yet_valid' };
  if (payload.exp + CLOCK_SKEW_SECONDS < opts.nowSeconds) return { ok: false, error: 'expired' };
  return { ok: true, payload };
}
