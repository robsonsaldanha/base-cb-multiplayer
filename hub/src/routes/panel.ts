import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify';
import { z, ZodError } from 'zod';
import { HttpError } from '../errors';
import { SESSION_COOKIE, currentUser } from '../auth';
import { CreateServerBody, MAX_SERVERS_PER_OWNER, serverView } from './owner';
import type { UserRow } from '../store';

const esc = (s: unknown) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);

function page(title: string, body: string): string {
  return `<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${esc(title)}</title><style>
body{font-family:system-ui,sans-serif;max-width:860px;margin:2rem auto;padding:0 1rem;background:#14111c;color:#ece6f5}
a,button{color:#d9b45a}button{background:#2a2238;border:1px solid #d9b45a;padding:.35rem .8rem;border-radius:6px;cursor:pointer}
input,textarea{width:100%;padding:.4rem;background:#1e1829;color:inherit;border:1px solid #4a3d63;border-radius:6px}
table{width:100%;border-collapse:collapse}td,th{padding:.4rem;border-bottom:1px solid #2f2642;text-align:left}
.key{font-family:monospace;background:#0c0a12;padding:.6rem;border:1px solid #d9b45a;word-break:break-all}
.btn{display:inline-block;text-decoration:none;background:#2a2238;border:1px solid #d9b45a;padding:.35rem .8rem;border-radius:6px}
.err{color:#ff8a8a}.ok{color:#8affb0}form.inline{display:inline}
</style></head><body>${body}</body></html>`;
}

export async function panelRoutes(app: FastifyInstance) {
  const { config, store, now } = app.deps;
  const origin = new URL(config.publicUrl).origin;

  // Cabeçalhos de segurança em toda resposta /panel (hook encapsulado neste plugin).
  app.addHook('onSend', async (_req, reply) => {
    reply.header('content-security-policy', "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
    reply.header('x-frame-options', 'DENY');
    reply.header('referrer-policy', 'same-origin');
    reply.header('cache-control', 'no-store');
  });

  /** CSRF: Origin igual ao publicUrl, ou Origin ausente/"null" com Sec-Fetch-Site same-origin. */
  function checkOrigin(req: FastifyRequest): void {
    const o = req.headers.origin;
    const ok = o === origin || ((o === undefined || o === 'null') && req.headers['sec-fetch-site'] === 'same-origin');
    if (!ok) throw new HttpError(403, 'bad_origin', 'origem inválida');
  }

  const loginLink = '<a class="btn" href="/v1/auth/discord/login?client=panel">Entrar com Discord</a>';

  app.setErrorHandler((err, req, reply) => {
    const send = (status: number, html: string) => reply.status(status).type('text/html; charset=utf-8').send(html);
    if (err instanceof HttpError) {
      if (err.status === 401) return send(401, page('Painel', `<h1>Painel de servidores</h1><p class="err">Faça login para continuar.</p><p>${loginLink}</p>`));
      if (err.status === 403) return send(403, page('Ação bloqueada', `<h1>Ação bloqueada</h1><p class="err">Ação bloqueada (origem inválida). Recarregue o painel e tente de novo.</p><p><a href="/panel">Voltar ao painel</a></p>`));
      if (err.status === 404) return send(404, page('Não encontrado', `<h1>Servidor não encontrado</h1><p><a href="/panel">Voltar ao painel</a></p>`));
    }
    if (err instanceof ZodError) {
      const u = currentUser(req);
      const msg = err.issues.map((i) => `${i.path.join('.') || '(corpo)'}: ${i.message}`).join('; ');
      if (!u) return send(401, page('Painel', `<h1>Painel de servidores</h1><p class="err">Faça login para continuar.</p><p>${loginLink}</p>`));
      return send(400, dashboard(u, `<p class="err">${esc(msg)}</p>`));
    }
    const status = (err as { statusCode?: number }).statusCode;
    if (status && status >= 400 && status < 500) return send(status, page('Erro', `<h1>Requisição inválida</h1><p class="err">${esc((err as Error).message)}</p><p><a href="/panel">Voltar ao painel</a></p>`));
    req.log.error(err);
    return send(500, page('Erro', '<h1>Erro interno</h1><p><a href="/panel">Voltar ao painel</a></p>'));
  });

  function requirePanelUser(req: FastifyRequest): UserRow {
    checkOrigin(req);
    const u = currentUser(req);
    if (!u) throw new HttpError(401, 'unauthorized', 'faça login');
    return u;
  }

  function dashboard(user: UserRow, extra = ''): string {
    const rows = store.listServersByOwner(user.id).map(({ server, status }) => {
      const v = serverView(server, status, now());
      const estado = v.online ? `<span class="ok">online</span> ${v.players}/${v.maxPlayers}` : v.verified ? 'offline' : 'aguardando 1º heartbeat';
      return `<tr><td>${esc(v.name)}<br><small>${esc(v.id)}</small></td><td>${estado}</td><td>
        <form class="inline" method="post" action="/panel/servers/${esc(v.id)}/rotate"><button>Gerar nova chave</button></form>
        <form class="inline" method="post" action="/panel/servers/${esc(v.id)}/delete"><button>Apagar</button></form>
      </td></tr>`;
    }).join('');
    return page('Painel - servidores', `
      <h1>Seus servidores</h1>
      <div>Logado como <b>${esc(user.name)}</b> <form class="inline" method="post" action="/panel/logout"><button>Sair</button></form></div>
      ${extra}
      <table><tr><th>Servidor</th><th>Estado</th><th></th></tr>${rows || '<tr><td colspan="3">Nenhum servidor ainda.</td></tr>'}</table>
      <h2>Novo servidor (máx. ${MAX_SERVERS_PER_OWNER})</h2>
      <form method="post" action="/panel/servers">
        <p><label>Nome<input name="name" required minlength="3" maxlength="64"></label></p>
        <p><label>Descrição<textarea name="description" maxlength="500"></textarea></label></p>
        <p><button>Criar</button></p>
      </form>`);
  }

  const keyBox = (key: string) =>
    `<p class="ok">Chave do servidor (aparece só agora; cole em <code>server.cfg</code>):</p><p class="key">${esc(key)}</p>`;

  const sendHtml = (reply: FastifyReply, html: string, status = 200) => reply.status(status).type('text/html; charset=utf-8').send(html);

  app.get('/panel', async (req, reply) => {
    const u = currentUser(req);
    if (!u) {
      const dev = config.devLogin
        ? `<p><a href="/v1/auth/dev-login?client=panel&amp;discordId=1&amp;name=Dev">Entrar como Dev (somente local)</a></p>` : '';
      return sendHtml(reply, page('Painel', `<h1>Painel de servidores</h1><p>${loginLink}</p>${dev}`));
    }
    return sendHtml(reply, dashboard(u));
  });

  app.post('/panel/servers', async (req, reply) => {
    const u = requirePanelUser(req);
    const form = z.object({ name: z.string(), description: z.string().optional() }).parse(req.body);
    try {
      const body = CreateServerBody.parse({ name: form.name, description: form.description ?? '' });
      if (store.countServersByOwner(u.id) >= MAX_SERVERS_PER_OWNER) {
        return sendHtml(reply, dashboard(u, `<p class="err">Limite de ${MAX_SERVERS_PER_OWNER} servidores atingido.</p>`), 409);
      }
      const { key } = store.createServer(u.id, body, now());
      return sendHtml(reply, dashboard(u, keyBox(key)));
    } catch (e) {
      if (e instanceof ZodError) {
        const msg = e.issues.map((i) => `${i.path.join('.')}: ${i.message}`).join('; ');
        return sendHtml(reply, dashboard(u, `<p class="err">${esc(msg)}</p>`), 400);
      }
      throw e;
    }
  });

  app.post('/panel/servers/:id/rotate', async (req, reply) => {
    const u = requirePanelUser(req);
    const { id } = z.object({ id: z.string().max(64) }).parse(req.params);
    const key = store.rotateKey(id, u.id);
    if (!key) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return sendHtml(reply, dashboard(u, keyBox(key)));
  });

  app.post('/panel/servers/:id/delete', async (req, reply) => {
    const u = requirePanelUser(req);
    const { id } = z.object({ id: z.string().max(64) }).parse(req.params);
    const srv = store.getServer(id);
    if (!srv || srv.ownerId !== u.id) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    const confirm = (req.body as { confirm?: unknown } | undefined)?.confirm;
    if (confirm !== '1') {
      return sendHtml(reply, page('Apagar servidor', `<h1>Apagar servidor</h1><p>Apagar <b>${esc(srv.name)}</b>? Isso não pode ser desfeito.</p>
        <form method="post" action="/panel/servers/${esc(srv.id)}/delete"><input type="hidden" name="confirm" value="1"><button>Sim, apagar</button></form>
        <p><a href="/panel">Cancelar</a></p>`));
    }
    if (!store.deleteServer(id, u.id)) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return reply.redirect('/panel', 303);
  });

  app.post('/panel/logout', async (req, reply) => {
    checkOrigin(req);
    const token = req.cookies[SESSION_COOKIE];
    if (token) store.deleteSessionByAccess(token);
    reply.clearCookie(SESSION_COOKIE, { path: '/' });
    return reply.redirect('/panel', 303);
  });
}
