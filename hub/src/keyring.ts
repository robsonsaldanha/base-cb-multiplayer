import { createPrivateKey, type KeyObject } from 'node:crypto';
import { mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { generatePrivateKey, publicKeyToRaw } from '@cbmp/contracts';

export interface KeyRing {
  active: { kid: string; privateKey: KeyObject };
  publicKeys(): { kid: string; publicKey: string }[];
}

/**
 * Chaves em `<dir>/<kid>.pem` (PKCS#8). A ativa é a de maior nome (kid = k + AAAAMMDDhhmmss).
 * Rotação: gerar novo .pem com kid maior; as antigas continuam publicadas até serem removidas.
 */
export function loadOrCreateKeyRing(dir: string, nowDate: Date = new Date()): KeyRing {
  mkdirSync(dir, { recursive: true });
  let files = readdirSync(dir).filter((f) => f.endsWith('.pem')).sort();
  if (files.length === 0) {
    const kid = 'k' + nowDate.toISOString().replace(/\D/g, '').slice(0, 14);
    writeFileSync(join(dir, `${kid}.pem`), generatePrivateKey().export({ format: 'pem', type: 'pkcs8' }), { mode: 0o600 });
    files = [`${kid}.pem`];
  }
  const entries = files.map((f) => ({ kid: f.slice(0, -4), privateKey: createPrivateKey(readFileSync(join(dir, f))) }));
  const published = entries.map((e) => ({ kid: e.kid, publicKey: publicKeyToRaw(e.privateKey) }));
  return { active: entries[entries.length - 1]!, publicKeys: () => published };
}
