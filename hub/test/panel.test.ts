import { describe, expect, it } from 'vitest';
import { makeTestApp, T0 } from './helpers';

const ORIGIN = { origin: 'http://localhost:8787' };
const FORM = { ...ORIGIN, 'content-type': 'application/x-www-form-urlencoded' };

async function panelSession() {
  const t = await makeTestApp();
  const user = t.store.upsertUserFromDiscord({ discordId: '1', name: 'Dono <b>', avatar: null }, T0);
  const s = t.store.createSession(user.id, 'panel', T0);
  return { ...t, user, cookies: { cbmp_session: s.access } };
}

describe('painel', () => {
  it('sem sessão mostra botão de login Discord', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/panel' });
    expect(res.statusCode).toBe(200);
    expect(res.headers['content-type']).toContain('text/html');
    expect(res.body).toContain('/v1/auth/discord/login?client=panel');
  });

  it('envia cabeçalhos de segurança (CSP, X-Frame-Options, Referrer-Policy)', async () => {
    const { app, cookies } = await panelSession();
    for (const res of [
      await app.inject({ method: 'GET', url: '/panel' }),
      await app.inject({ method: 'GET', url: '/panel', cookies }),
      await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: FORM, payload: 'name=x' }),
    ]) {
      expect(res.headers['content-security-policy']).toBe("default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
      expect(res.headers['x-frame-options']).toBe('DENY');
      expect(res.headers['referrer-policy']).toBe('no-referrer');
    }
  });

  it('logado: cria servidor e mostra a chave uma única vez, escapando HTML', async () => {
    const { app, cookies, store, user } = await panelSession();
    const res = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: FORM, payload: 'name=Castelo+%3Cscript%3E&description=RP' });
    expect(res.statusCode).toBe(200);
    const key = /sk_[A-Za-z0-9_-]{43}/.exec(res.body)?.[0];
    expect(key).toBeDefined();
    expect(res.body).toContain('Castelo &lt;script&gt;');
    expect(res.body).not.toContain('<script>');
    expect(res.body).toContain('Dono &lt;b&gt;');
    const again = await app.inject({ method: 'GET', url: '/panel', cookies });
    expect(again.body).not.toContain(key!);
    expect(store.listServersByOwner(user.id)).toHaveLength(1);
  });

  it('POST sem Origin igual ao publicUrl é recusado (CSRF)', async () => {
    const { app, cookies } = await panelSession();
    const res = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: { origin: 'https://evil.example', 'content-type': 'application/x-www-form-urlencoded' }, payload: 'name=Castelo' });
    expect(res.statusCode).toBe(403);
  });

  it('gira chave e apaga', async () => {
    const { app, cookies, store, user } = await panelSession();
    const { server } = store.createServer(user.id, { name: 'Srv', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
    const rot = await app.inject({ method: 'POST', url: `/panel/servers/${server.id}/rotate`, cookies, headers: ORIGIN });
    expect(rot.body).toMatch(/sk_[A-Za-z0-9_-]{43}/);
    const del = await app.inject({ method: 'POST', url: `/panel/servers/${server.id}/delete`, cookies, headers: ORIGIN });
    expect(del.statusCode).toBe(303);
    expect(store.listServersByOwner(user.id)).toHaveLength(0);
  });

  it('erro de validação aparece na página', async () => {
    const { app, cookies } = await panelSession();
    const res = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: FORM, payload: 'name=x' });
    expect(res.statusCode).toBe(400);
    expect(res.body).toContain('name');
  });
});
