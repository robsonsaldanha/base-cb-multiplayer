// node --test tools/test/  (também roda no `npm test` da raiz)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash, generateKeyPairSync, randomBytes, sign } from 'node:crypto';
import { parseModComment, parsePublicKey, tauriCommentFields, verifyMinisign } from '../lib/minisign.mjs';

/** Par minisign gerado no teste (nenhuma chave real): devolve { pub (formato Tauri), signFile(data, comment, alg) }. */
function testKey(keyId = randomBytes(8)) {
  const { publicKey, privateKey } = generateKeyPairSync('ed25519');
  const raw = Buffer.from(publicKey.export({ format: 'jwk' }).x, 'base64url');
  const pubText = `untrusted comment: minisign public key: TESTE\n${Buffer.concat([Buffer.from('Ed'), keyId, raw]).toString('base64')}\n`;
  const signFile = (data, comment, alg = 'ED', kid = keyId) => {
    const msg = alg === 'ED' ? createHash('blake2b512').update(data).digest() : data;
    const sig = sign(null, msg, privateKey);
    const global = sign(null, Buffer.concat([sig, Buffer.from(comment)]), privateKey);
    const text = `untrusted comment: assinatura de teste\n${Buffer.concat([Buffer.from(alg), kid, sig]).toString('base64')}\ntrusted comment: ${comment}\n${global.toString('base64')}\n`;
    return Buffer.from(text).toString('base64');
  };
  return { pub: Buffer.from(pubText).toString('base64'), pubText, signFile, keyId };
}

test('confere assinatura com pré-hash (ED) e legada (Ed), no base64 do Tauri e em texto', () => {
  const k = testKey();
  const data = Buffer.from('instalador de mentira');
  const c = 'timestamp:1\tfile:x_0.1.1_x64-setup.exe\tversion:0.1.1';
  assert.equal(verifyMinisign(data, k.signFile(data, c), k.pub), c);
  assert.equal(verifyMinisign(data, k.signFile(data, c, 'Ed'), k.pubText), c);
  assert.deepEqual(tauriCommentFields(c), { timestamp: '1', file: 'x_0.1.1_x64-setup.exe', version: '0.1.1' });
});

test('recusa arquivo alterado, outra chave e comentário trocado', () => {
  const k = testKey();
  const other = testKey();
  const data = Buffer.from('conteúdo');
  const sig = k.signFile(data, 'version:0.1.0');
  assert.throws(() => verifyMinisign(Buffer.from('conteúdO'), sig, k.pub), /não confere com o arquivo/);
  assert.throws(() => verifyMinisign(data, sig, other.pub), /outra chave/);
  // Mesmo key id, chave diferente: a assinatura não confere.
  const twin = testKey(k.keyId);
  assert.throws(() => verifyMinisign(data, sig, twin.pub), /não confere/);
  // Comentário confiável trocado depois de assinar.
  const text = Buffer.from(sig, 'base64').toString('utf8').replace('version:0.1.0', 'version:9.9.9');
  assert.throws(() => verifyMinisign(data, Buffer.from(text).toString('base64'), k.pub), /comentário confiável não confere/);
  assert.throws(() => verifyMinisign(data, 'lixo', k.pub), /minisign/);
});

test('chave pública: key id como o minisign mostra', () => {
  const k = testKey(Buffer.from('2916E9918047CBBD', 'hex'));
  assert.equal(parsePublicKey(k.pub).keyId, 'BDCB478091E91629');
});

test('comentário do mod: formato exato', () => {
  const sha = 'a'.repeat(64);
  assert.deepEqual(parseModComment(`cbmp-mod protocol=1 version=0.1.0 sha256=${sha}`), { protocol: 1, version: '0.1.0', sha256: sha });
  assert.equal(parseModComment(`cbmp-mod protocol=1 version=0.1.0 sha256=${sha} x`), null);
  assert.equal(parseModComment(`cbmp-mod protocol=1 version=0.1 sha256=${sha}`), null);
  assert.equal(parseModComment(`cbmp-mod protocol=1 version=0.1.0 sha256=${sha.toUpperCase()}`), null);
});
