// Conferência de assinaturas minisign (formato do Tauri: o `.sig` e a chave pública são base64 do arquivo de texto do
// minisign). Só verifica; não assina nem lê chave privada. Usado pelo tools/make-test-kit.mjs (antes de empacotar) e
// pelo tools/kit-smoke.mjs (no kit, contra o Hub rodando). Mesma regra do launcher (`modpkg::verify_minisign`):
//   - algoritmo "Ed" (Ed25519 sobre o arquivo) ou "ED" (Ed25519 sobre o BLAKE2b-512 do arquivo);
//   - key id da assinatura = key id da chave pública;
//   - assinatura global (Ed25519 sobre assinatura || comentário confiável) confere.

import { createHash, createPublicKey, verify } from 'node:crypto';

/** Texto do minisign dentro do base64 do Tauri (aceita também o texto puro). */
export function minisignText(s) {
  const t = String(s).trim();
  if (t.startsWith('untrusted comment:')) return t;
  const dec = Buffer.from(t, 'base64').toString('utf8');
  if (!dec.startsWith('untrusted comment:')) throw new Error('não é um arquivo minisign (nem base64 de um)');
  return dec;
}

function lines(text) {
  return text.split(/\r?\n/).map((l) => l.trim()).filter((l) => l !== '');
}

/** Chave pública: { keyId (hex, maiúsculas, como o minisign mostra), key (KeyObject Ed25519) }. */
export function parsePublicKey(pub) {
  const l = lines(minisignText(pub));
  const raw = Buffer.from(l[1] ?? '', 'base64');
  if (raw.length !== 42 || raw.subarray(0, 2).toString('latin1') !== 'Ed') throw new Error('chave pública minisign inválida');
  const keyIdRaw = raw.subarray(2, 10);
  const key = createPublicKey({ key: { kty: 'OKP', crv: 'Ed25519', x: raw.subarray(10).toString('base64url') }, format: 'jwk' });
  return { keyId: Buffer.from(keyIdRaw).reverse().toString('hex').toUpperCase(), keyIdRaw, key };
}

/** Assinatura: { alg, keyIdRaw, sig, trustedComment, globalSig }. */
export function parseSignature(sigText) {
  const l = lines(minisignText(sigText));
  const raw = Buffer.from(l[1] ?? '', 'base64');
  const alg = raw.subarray(0, 2).toString('latin1');
  if (raw.length !== 74 || (alg !== 'Ed' && alg !== 'ED')) throw new Error('assinatura minisign inválida');
  const tc = l[2] ?? '';
  if (!tc.startsWith('trusted comment: ')) throw new Error('assinatura sem comentário confiável');
  const globalSig = Buffer.from(l[3] ?? '', 'base64');
  if (globalSig.length !== 64) throw new Error('assinatura global ausente ou inválida');
  return { alg, keyIdRaw: raw.subarray(2, 10), sig: raw.subarray(10), trustedComment: tc.slice('trusted comment: '.length), globalSig };
}

/**
 * Confere `data` (Buffer) com a assinatura (`.sig` do Tauri ou texto minisign) e a chave pública. Devolve o
 * comentário confiável; lança Error com o motivo se não conferir.
 */
export function verifyMinisign(data, sigText, pubText) {
  const pk = parsePublicKey(pubText);
  const s = parseSignature(sigText);
  if (!s.keyIdRaw.equals(pk.keyIdRaw)) throw new Error(`assinatura feita com outra chave (esperada ${pk.keyId})`);
  const msg = s.alg === 'ED' ? createHash('blake2b512').update(data).digest() : data;
  if (!verify(null, msg, pk.key, s.sig)) throw new Error('assinatura não confere com o arquivo');
  const global = Buffer.concat([s.sig, Buffer.from(s.trustedComment, 'utf8')]);
  if (!verify(null, global, pk.key, s.globalSig)) throw new Error('comentário confiável não confere com a assinatura');
  return s.trustedComment;
}

/** Campos `chave:valor` separados por tab do comentário confiável do updater do Tauri. */
export function tauriCommentFields(comment) {
  const out = {};
  for (const part of comment.split('\t')) {
    const i = part.indexOf(':');
    if (i > 0) out[part.slice(0, i)] = part.slice(i + 1);
  }
  return out;
}

/** Comentário do mod: `cbmp-mod protocol=<N> version=<X.Y.Z> sha256=<hex>` (exato) → { protocol, version, sha256 }. */
export function parseModComment(comment) {
  const m = /^cbmp-mod protocol=(\d{1,9}) version=(\d+\.\d+\.\d+) sha256=([0-9a-f]{64})$/.exec(comment);
  return m ? { protocol: Number(m[1]), version: m[2], sha256: m[3] } : null;
}
