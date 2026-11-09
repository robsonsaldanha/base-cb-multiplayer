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
      expect(res.headers['referrer-policy']).toBe('same-origin');
      expect(res.headers['cache-control']).toBe('no-store');
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
    const del = await app.inject({ method: 'POST', url: `/panel/servers/${server.id}/delete`, cookies, headers: FORM, payload: 'confirm=1' });
    expect(del.statusCode).toBe(303);
    expect(store.listServersByOwner(user.id)).toHaveLength(0);
  });

  it('CSRF: Origin null + Sec-Fetch-Site same-origin passa; sem Sec-Fetch-Site ou cross-site é 403', async () => {
    const { app, cookies } = await panelSession();
    const h = (extra: Record<string, string>) => ({ 'content-type': 'application/x-www-form-urlencoded', ...extra });
    const ok = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: h({ origin: 'null', 'sec-fetch-site': 'same-origin' }), payload: 'name=Castelo' });
    expect(ok.statusCode).toBe(200);
    const ok2 = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: h({ 'sec-fetch-site': 'same-origin' }), payload: 'name=Castelo2' });
    expect(ok2.statusCode).toBe(200);
    expect((await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: h({ origin: 'null' }), payload: 'name=Castelo3' })).statusCode).toBe(403);
    expect((await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: h({ origin: 'null', 'sec-fetch-site': 'cross-site' }), payload: 'name=Castelo3' })).statusCode).toBe(403);
  });

  it('erros do painel são HTML', async () => {
    const { app, cookies } = await panelSession();
    const r401 = await app.inject({ method: 'POST', url: '/panel/servers', headers: FORM, payload: 'name=Castelo' });
    expect(r401.statusCode).toBe(401);
    expect(r401.headers['content-type']).toContain('text/html');
    expect(r401.body).toContain('/v1/auth/discord/login?client=panel');
    const r403 = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: { origin: 'https://evil.example', 'content-type': 'application/x-www-form-urlencoded' }, payload: 'name=Castelo' });
    expect(r403.statusCode).toBe(403);
    expect(r403.headers['content-type']).toContain('text/html');
    expect(r403.body).toContain('Ação bloqueada (origem inválida). Recarregue o painel e tente de novo.');
    const r404 = await app.inject({ method: 'POST', url: '/panel/servers/nope/rotate', cookies, headers: ORIGIN });
    expect(r404.statusCode).toBe(404);
    expect(r404.headers['content-type']).toContain('text/html');
    expect(r404.body).toContain('Servidor não encontrado');
    expect(r404.body).toContain('href="/panel"');
    const r400 = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: ORIGIN });
    expect(r400.statusCode).toBe(400);
    expect(r400.headers['content-type']).toContain('text/html');
    expect(r400.body).toContain('Seus servidores');
    const api = await app.inject({ method: 'GET', url: '/v1/me' });
    expect(api.headers['content-type']).toContain('application/json');
  });

  it('HTML válido: link-botão, sem form dentro de p, & escapado', async () => {
    const t = await makeTestApp({ config: { devLogin: true } });
    const anon = await t.app.inject({ method: 'GET', url: '/panel' });
    expect(anon.body).toContain('class="btn"');
    expect(anon.body).not.toContain('<button>Entrar');
    expect(anon.body).toContain('client=panel&amp;discordId=1&amp;name=Dev');
    const { app, cookies } = await panelSession();
    const res = await app.inject({ method: 'GET', url: '/panel', cookies });
    expect(res.body).not.toMatch(/<p>[^]*?<form[^]*?<\/p>/);
  });

  it('apagar pede confirmação sem JS', async () => {
    const { app, cookies, store, user } = await panelSession();
    const { server } = store.createServer(user.id, { name: 'Srv <i>', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
    const url = `/panel/servers/${server.id}/delete`;
    const ask = await app.inject({ method: 'POST', url, cookies, headers: ORIGIN });
    expect(ask.statusCode).toBe(200);
    expect(ask.body).toContain('Srv &lt;i&gt;');
    expect(ask.body).toContain('name="confirm" value="1"');
    expect(ask.body).toContain('href="/panel"');
    expect(store.listServersByOwner(user.id)).toHaveLength(1);
    const del = await app.inject({ method: 'POST', url, cookies, headers: FORM, payload: 'confirm=1' });
    expect(del.statusCode).toBe(303);
    expect(del.headers.location).toBe('/panel');
    expect(store.listServersByOwner(user.id)).toHaveLength(0);
  });

  it('erro de validação aparece na página', async () => {
    const { app, cookies } = await panelSession();
    const res = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: FORM, payload: 'name=x' });
    expect(res.statusCode).toBe(400);
    expect(res.body).toContain('name');
  });
});
