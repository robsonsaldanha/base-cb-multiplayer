import { randomUUID } from 'node:crypto';
import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify';
import { TICKET_TTL_SECONDS, signTicket, type TicketPayload } from '@cbmp/contracts';
import { SESSION_COOKIE } from '../auth';
import type { UserRow } from '../store';
import { serverView } from './owner';
import { normalizeIp } from '../probe';

/**
 * GET /v1/auth/server-admin?serverId=… — login no painel admin de um servidor via Hub.
 *
 * O Hub só IDENTIFICA o usuário: emite um ticket com `aud = "<serverId>#admin"` (mesmo formato/assinatura dos
 * tickets de jogo, TTL 300 s) para qualquer usuário logado, inclusive quem não é dono do servidor. Quem decide se
 * aquele `did` é admin é o próprio servidor (`admin.admins` no server.json). O redirecionamento vai só para a origem
 * anunciada no heartbeat (já normalizada) + caminho fixo `/auth/callback#ticket=…` (fragmento: não vai em logs nem
 * em Referer).
 */

export const RETURN_COOKIE = 'cbmp_return';
/** Cobre esta rota e o callback do Discord (e o dev-login), nada mais. */
export const RETURN_COOKIE_PATH = '/v1/auth';
const RETURN_TTL = 600;
export const ADMIN_LOGIN_PER_USER = 10;
export const ADMIN_LOGIN_PER_IP = 30;

const SERVER_ID_RE = /^srv_[A-Za-z0-9_-]{12}$/;
export const isServerId = (v: unknown): v is string => typeof v === 'string' && SERVER_ID_RE.test(v);

const CSP = "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
const esc = (s: unknown) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);

function page(title: string, body: string): string {
  return `<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${esc(title)}</title><style>
body{font-family:system-ui,sans-serif;max-width:720px;margin:2rem auto;padding:0 1rem;background:#14111c;color:#ece6f5}
a{color:#d9b45a}code{background:#0c0a12;padding:0 .3rem}.err{color:#ff8a8a}
</style></head><body><h1>${esc(title)}</h1>${body}</body></html>`;
}

type Result = 'ticket_issued' | 'login_required' | 'bad_server_id' | 'not_found' | 'offline' | 'no_admin_url' | 'rate_limited';

/** Janela fixa de 60 s por chave; o mapa é zerado a cada janela (memória limitada ao tráfego de 1 min). */
class MinuteLimiter {
  private window = -1;
  private counts = new Map<string, number>();
  constructor(private readonly max: number) {}
  hit(key: string, now: number): boolean {
    const w = Math.floor(now / 60);
    if (w !== this.window) { this.window = w; this.counts.clear(); }
    const n = (this.counts.get(key) ?? 0) + 1;
    this.counts.set(key, n);
    return n <= this.max;
  }
}

export async function adminLoginRoutes(app: FastifyInstance) {
  const { config, store, keyring, now } = app.deps;
  const secure = config.publicUrl.startsWith('https://');
  const perUser = new MinuteLimiter(ADMIN_LOGIN_PER_USER);
  const perIp = new MinuteLimiter(ADMIN_LOGIN_PER_IP);

  // Cabeçalhos em toda resposta desta rota (hook encapsulado neste plugin).
  app.addHook('onSend', async (_req, reply) => {
    reply.header('content-security-policy', CSP);
    reply.header('x-frame-options', 'DENY');
    reply.header('x-content-type-options', 'nosniff');
    reply.header('referrer-policy', 'no-referrer');
    reply.header('cache-control', 'no-store');
  });

  function audit(req: FastifyRequest, user: UserRow | null, serverId: string | null, result: Result) {
    req.log.info({ audit: 'server_admin_login', did: user?.discordId ?? null, serverId, result }, 'login de admin de servidor');
  }

  const html = (reply: FastifyReply, status: number, title: string, body: string) =>
    reply.status(status).type('text/html; charset=utf-8').send(page(title, body));

  app.get('/v1/auth/server-admin', async (req, reply) => {
    const t = now();
    const raw = (req.query as Record<string, unknown> | undefined)?.serverId;
    const serverId = isServerId(raw) ? raw : null;

    if (config.rateLimit && !perIp.hit(normalizeIp(req.ip), t)) {
      audit(req, null, serverId, 'rate_limited');
      return html(reply, 429, 'Muitas tentativas', '<p class="err">Muitas tentativas de login. Aguarde um minuto e tente de novo.</p>');
    }
    if (!serverId) {
      audit(req, null, null, 'bad_server_id');
      return html(reply, 400, 'Endereço inválido', '<p class="err">O link de login de admin está incompleto ou inválido. Volte ao painel do servidor e tente de novo.</p>');
    }

    // Só o cookie do painel do Hub (navegação de navegador); Bearer não vale aqui.
    const cookie = req.cookies[SESSION_COOKIE];
    const user = cookie ? store.findUserByAccess(cookie, t) : null;
    if (!user) {
      reply.setCookie(RETURN_COOKIE, serverId, { path: RETURN_COOKIE_PATH, httpOnly: true, sameSite: 'lax', secure, maxAge: RETURN_TTL });
      audit(req, null, serverId, 'login_required');
      return reply.redirect('/v1/auth/discord/login?client=panel');
    }
    if (config.rateLimit && !perUser.hit(user.id, t)) {
      audit(req, user, serverId, 'rate_limited');
      return html(reply, 429, 'Muitas tentativas', '<p class="err">Muitas tentativas de login. Aguarde um minuto e tente de novo.</p>');
    }

    const server = store.getServer(serverId);
    if (!server) {
      audit(req, user, serverId, 'not_found');
      return html(reply, 404, 'Servidor não encontrado', '<p class="err">Esse servidor não existe (ou foi apagado) no Hub.</p>');
    }
    const status = store.getStatus(serverId);
    if (!serverView(server, status, t).online) {
      audit(req, user, serverId, 'offline');
      return html(reply, 503, 'Servidor offline', `<p>O servidor <strong>${esc(server.name)}</strong> não está online no Hub agora (sem heartbeat recente ou ainda não verificado).</p><p>Tente de novo quando ele estiver no ar.</p>`);
    }
    if (!status?.adminUrl) {
      audit(req, user, serverId, 'no_admin_url');
      return html(reply, 409, 'Painel admin não configurado', `<p>O servidor <strong>${esc(server.name)}</strong> não anunciou um painel admin.</p>
<p>Quem administra o servidor precisa definir <code>admin.publicUrl</code> no <code>server.json</code> (origem https do painel, por exemplo <code>https://painel.meuservidor.com</code>) e reiniciar o servidor.</p>`);
    }

    const iat = t;
    const payload: TicketPayload = {
      v: 1, kid: keyring.active.kid, sub: user.id, did: user.discordId, name: user.name,
      aud: `${server.id}#admin`, iat, exp: iat + TICKET_TTL_SECONDS, jti: randomUUID(),
    };
    const ticket = signTicket(payload, keyring.active.privateKey);
    audit(req, user, serverId, 'ticket_issued');
    // adminUrl já é uma origem normalizada (schema); o caminho é fixo e o ticket só tem [A-Za-z0-9_.-].
    return reply.code(303).header('location', `${status.adminUrl}/auth/callback#ticket=${ticket}`).send();
  });
}
