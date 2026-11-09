import { describe, expect, it } from 'vitest';
import { makeTestApp } from './helpers';

describe('app', () => {
  it('GET /health', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/health' });
    expect(res.statusCode).toBe(200);
    expect(res.json()).toEqual({ ok: true });
  });
  it('404 no formato de erro padrão', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/nao-existe' });
    expect(res.statusCode).toBe(404);
    expect(res.json()).toEqual({ error: 'not_found', message: 'rota não encontrada' });
  });
});
