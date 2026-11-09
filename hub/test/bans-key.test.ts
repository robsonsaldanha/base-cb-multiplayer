import { describe, expect, it } from 'vitest';
import { makeTestApp } from './helpers';

describe('GET /v1/bans/global: erros de chave', () => {
  it('sem header → no_key; chave errada → bad_key', async () => {
    const { app } = await makeTestApp();
    const a = await app.inject({ method: 'GET', url: '/v1/bans/global' });
    expect(a.statusCode).toBe(401);
    expect(a.json().error).toBe('no_key');
    const b = await app.inject({ method: 'GET', url: '/v1/bans/global', headers: { authorization: 'Bearer sk_errada' } });
    expect(b.statusCode).toBe(401);
    expect(b.json().error).toBe('bad_key');
  });
});
