import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { buildVectors, publicKeyFromRaw, verifyTicket } from '../src/index';

const file = new URL('../test-vectors/tickets.json', import.meta.url);

describe('vetores de ticket', () => {
  it('arquivo commitado é igual ao gerado (rode npm run vectors se mudar)', () => {
    expect(readFileSync(file, 'utf8')).toBe(JSON.stringify(buildVectors(), null, 2) + '\n');
  });
  it('cada caso dá o resultado esperado', () => {
    const v = buildVectors();
    const keys = new Map(v.publicKeys.map((k) => [k.kid, publicKeyFromRaw(k.publicKey)] as const));
    for (const c of v.cases) {
      const r = verifyTicket(c.ticket, { publicKeys: keys, audience: c.audience, nowSeconds: c.nowSeconds });
      expect(r.ok ? 'ok' : r.error, c.name).toBe(c.expected);
    }
  });
});
