import { createHash, createHmac, randomBytes, randomUUID, timingSafeEqual } from 'node:crypto';
import { isIP } from 'node:net';
import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify';
import formbody from '@fastify/formbody';
import { TICKET_TTL_SECONDS, signTicket, type TicketPayload } from '@cbmp/contracts';
import { SESSION_COOKIE } from '../auth';
import type { ServerRow, StatusRow, UserRow } from '../store';
import { serverView } from './owner';
import { normalizeIp } from '../probe';

/**
 * Login no painel admin de um servidor via Hub.
 *
 * GET  /v1/auth/server-admin?serverId=…[&state=…] — só CONFIRMAÇÃO: mostra servidor, destino e a identidade Discord
 *      que será compartilhada, com botão "Entrar como admin" (form POST com token CSRF). Nunca emite ticket.
 * POST /v1/auth/server-admin (form: serverId, csrf, state?) — exige Origin == Hub e token CSRF amarrado à sessão,
 *      ao servidor, ao state e à origem de destino mostrada; emite o ticket e responde 303 para
 *      `<adminUrl>/auth/callback#ticket=…[&state=…]`.
 *
 * O Hub só IDENTIFICA: qualquer usuário logado pode receber o ticket (`aud = "<serverId>#admin"`, mesmo formato dos
 * tickets de jogo, TTL 300 s); quem decide se aquele `did` é admin é o servidor (`admin.admins`). O `state` é opaco
 * para o Hub: o painel do servidor o gera, guarda (cookie SameSite=Strict) e compara no callback (Task 8).
 */

export const SERVER_ID_RE = /^srv_[A-Za-z0-9_-]{12}$/;
export const ADMIN_STATE_RE = /^[A-Za-z0-9_-]{16,64}$/;
export const isServerId = (v: unknown): v is string => typeof v === 'string' && SERVER_ID_RE.test(v);
const isAdminState = (v: unknown): v is string => typeof v === 'string' && ADMIN_STATE_RE.test(v);

export const adminConfirmPath = (serverId: string, state: string | null) =>
  `/v1/auth/server-admin?serverId=${encodeURIComponent(serverId)}${state ? `&state=${encodeURIComponent(state)}` : ''}`;

export const ADMIN_LOGIN_PER_USER = 10;
export const ADMIN_LOGIN_PER_IP = 30;
/** Teto de chaves por limitador (por janela); acima disso, chaves novas são recusadas até a janela virar. */
export const LIMITER_MAX_KEYS = 50_000;
const CSRF_TTL = 600;

const BASE_CSP = "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
// O POST responde 303 para o painel: form-action precisa liberar a origem de destino (navegadores checam o redirect).
const confirmCsp = (adminUrl: string) =>
  `default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self' ${adminUrl}; frame-ancestors 'none'`;

const esc = (s: unknown) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);

function page(title: string, body: string): string {
  return `<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${esc(title)}</title><style>
body{font-family:system-ui,sans-serif;max-width:720px;margin:2rem auto;padding:0 1rem;background:#14111c;color:#ece6f5}
a{color:#d9b45a}code,.dest{background:#0c0a12;padding:0 .3rem;font-family:monospace}.err{color:#ff8a8a}.warn{color:#ffcc66}
.dest{display:block;padding:.6rem;border:1px solid #d9b45a;font-size:1.1rem;word-break:break-all}
button{background:#2a2238;color:#d9b45a;border:1px solid #d9b45a;padding:.5rem 1rem;border-radius:6px;cursor:pointer;font-size:1rem}
dt{opacity:.7}dd{margin:0 0 .6rem 0}
</style></head><body><h1>${esc(title)}</h1>${body}</body></html>`;
}

type Result = 'confirm_shown' | 'ticket_issued' | 'login_required' | 'bad_request' | 'bad_origin' | 'bad_csrf' | 'unauthorized'
  | 'not_found' | 'offline' | 'no_admin_url' | 'rate_limited';

/**
 * Limite por minuto com janela deslizante aproximada (janela atual + fração da anterior: sem rajada de 2× na
 * virada). Memória limitada: no máximo LIMITER_MAX_KEYS chaves por janela; chave nova acima disso é recusada.
 */
class MinuteLimiter {
  private window = -1;
  private cur = new Map<string, number>();
  private prev = new Map<string, number>();
  constructor(private readonly max: number) {}
  hit(key: string, now: number): boolean {
    const w = Math.floor(now / 60);
    if (w !== this.window) {
      this.prev = w === this.window + 1 ? this.cur : new Map();
      this.cur = new Map();
      this.window = w;
    }
    const n = this.cur.get(key);
    if (n === undefined && this.cur.size >= LIMITER_MAX_KEYS) return false;
    const elapsed = (now - w * 60) / 60;
    const estimate = (n ?? 0) + 1 + (this.prev.get(key) ?? 0) * (1 - elapsed);
    if (estimate > this.max) return false;
    this.cur.set(key, (n ?? 0) + 1);
    return true;
  }
}

/** Chave do limite por IP: IPv4 inteiro; IPv6 agrupado por /64 (um cliente costuma ter o /64 inteiro). */
export function ipKey(raw: string): string {
  const ip = normalizeIp(raw);
  if (isIP(ip) !== 6) return ip;
  const [head = '', tail] = ip.toLowerCase().split('::');
  const h = head ? head.split(':') : [];
  const t = tail === undefined ? [] : tail ? tail.split(':') : [];
  const groups = tail === undefined ? h : [...h, ...Array<string>(Math.max(0, 8 - h.length - t.length)).fill('0'), ...t];
  return groups.slice(0, 4).map((g) => (g.includes('.') ? g : parseInt(g, 16).toString(16))).join(':') + '::/64';
}

export async function adminLoginRoutes(app: FastifyInstance) {
  const { config, store, keyring, now } = app.deps;
  const hubOrigin = new URL(config.publicUrl).origin;
  const perUser = new MinuteLimiter(ADMIN_LOGIN_PER_USER);
  const perIp = new MinuteLimiter(ADMIN_LOGIN_PER_IP);
  // Chave do HMAC do token CSRF: só neste processo (reiniciar o Hub invalida confirmações abertas; refaz em 1 clique).
  const csrfKey = randomBytes(32);
  // Auditoria em logger filho com nível próprio: não some com HUB_LOG_LEVEL=warn/error (só com "silent").
  const auditLog = app.log.child({ audit: 'server_admin_login' }, { level: 'info' });

  await app.register(formbody);

  app.addHook('onSend', async (_req, reply) => {
    if (!reply.hasHeader('content-security-policy')) reply.header('content-security-policy', BASE_CSP);
    reply.header('x-frame-options', 'DENY');
    reply.header('x-content-type-options', 'nosniff');
    reply.header('referrer-policy', 'no-referrer');
    reply.header('cache-control', 'no-store');
  });

  const audit = (user: UserRow | null, serverId: string | null, result: Result) =>
    auditLog.info({ did: user?.discordId ?? null, serverId, result }, 'login de admin de servidor');

  const html = (reply: FastifyReply, status: number, title: string, body: string) =>
    reply.status(status).type('text/html; charset=utf-8').send(page(title, body));

  const tooMany = (reply: FastifyReply) =>
    html(reply, 429, 'Muitas tentativas', '<p class="err">Muitas tentativas de login. Aguarde um minuto e tente de novo.</p>');
  const badRequest = (reply: FastifyReply) =>
    html(reply, 400, 'Endereço inválido', '<p class="err">O link de login de admin está incompleto ou inválido. Volte ao painel do servidor e tente de novo.</p>');
  const forbidden = (reply: FastifyReply) =>
    html(reply, 403, 'Ação bloqueada', '<p class="err">Confirmação inválida ou expirada. Volte ao painel do servidor e comece de novo.</p>');

  const sessionOf = (req: FastifyRequest) => {
    const cookie = req.cookies[SESSION_COOKIE];
    const user = cookie ? store.findUserByAccess(cookie, now()) : null;
    return user && cookie ? { user, cookie } : null;
  };

  const csrfMac = (cookie: string, serverId: string, state: string | null, adminUrl: string, exp: number) =>
    createHmac('sha256', csrfKey)
      .update(['admin-login/v1', createHash('sha256').update(cookie).digest('hex'), serverId, state ?? '', adminUrl, String(exp)].join('\n'))
      .digest('base64url');

  function csrfOk(token: string, cookie: string, serverId: string, state: string | null, adminUrl: string): boolean {
    const m = /^([0-9]{1,12})\.([A-Za-z0-9_-]{43})$/.exec(token);
    if (!m) return false;
    const exp = Number(m[1]);
    if (exp < now()) return false;
    const want = Buffer.from(csrfMac(cookie, serverId, state, adminUrl, exp));
    const got = Buffer.from(m[2]!);
    return want.length === got.length && timingSafeEqual(want, got);
  }

  /** Servidor existe, está online e anunciou adminUrl; senão responde a página e devolve null. */
  function target(reply: FastifyReply, user: UserRow, serverId: string): { server: ServerRow; status: StatusRow & { adminUrl: string } } | null {
    const server = store.getServer(serverId);
    if (!server) {
      audit(user, serverId, 'not_found');
      html(reply, 404, 'Servidor não encontrado', '<p class="err">Esse servidor não existe (ou foi apagado) no Hub.</p>');
      return null;
    }
    const status = store.getStatus(serverId);
    if (!status || !serverView(server, status, now()).online) {
      audit(user, serverId, 'offline');
      html(reply, 503, 'Servidor offline', `<p>O servidor <strong>${esc(server.name)}</strong> não está online no Hub agora (sem heartbeat recente ou ainda não verificado).</p><p>Tente de novo quando ele estiver no ar.</p>`);
      return null;
    }
    if (!status.adminUrl) {
      audit(user, serverId, 'no_admin_url');
      html(reply, 409, 'Painel admin não configurado', `<p>O servidor <strong>${esc(server.name)}</strong> não anunciou um painel admin.</p>
<p>Quem administra o servidor precisa definir <code>admin.publicUrl</code> no <code>server.json</code> (origem https do painel, por exemplo <code>https://painel.meuservidor.com</code>) e reiniciar o servidor.</p>`);
      return null;
    }
    return { server, status: status as StatusRow & { adminUrl: string } };
  }

  app.get('/v1/auth/server-admin', async (req, reply) => {
    const q = (req.query ?? {}) as Record<string, unknown>;
    const serverId = isServerId(q.serverId) ? q.serverId : null;
    if (config.rateLimit && !perIp.hit(ipKey(req.ip), now())) {
      audit(null, serverId, 'rate_limited');
      return tooMany(reply);
    }
    const state = q.state === undefined ? null : isAdminState(q.state) ? q.state : undefined;
    if (!serverId || state === undefined) {
      audit(null, serverId, 'bad_request');
      return badRequest(reply);
    }
    // Só o cookie do painel do Hub (navegação de navegador); Bearer não vale aqui.
    const s = sessionOf(req);
    if (!s) {
      audit(null, serverId, 'login_required');
      // O retorno vai no state OAuth (uso único), não em cookie: morre com o login.
      const qs = new URLSearchParams({ client: 'panel', return: serverId });
      if (state) qs.set('return_state', state);
      return reply.redirect(`/v1/auth/discord/login?${qs}`);
    }
    const tg = target(reply, s.user, serverId);
    if (!tg) return reply;
    const { server, status } = tg;
    const exp = now() + CSRF_TTL;
    const csrf = `${exp}.${csrfMac(s.cookie, serverId, state, status.adminUrl, exp)}`;
    const host = new URL(status.adminUrl).hostname;
    const ipWarn = isIP(host.startsWith('[') ? host.slice(1, -1) : host)
      ? '<p class="warn">Atenção: o destino é um endereço IP, não um nome de domínio. Só continue se você reconhece esse endereço.</p>' : '';
    audit(s.user, serverId, 'confirm_shown');
    reply.header('content-security-policy', confirmCsp(status.adminUrl));
    return html(reply, 200, 'Entrar no painel admin', `<p>Você vai entrar no painel admin do servidor abaixo. O Hub vai enviar ao painel desse servidor a
sua identidade Discord; quem decide se você é admin é o próprio servidor.</p>
<dl><dt>Servidor</dt><dd><strong>${esc(server.name)}</strong> <small><code>${esc(server.id)}</code></small></dd>
<dt>Destino (painel admin)</dt><dd><span class="dest">${esc(status.adminUrl)}</span></dd>
<dt>Identidade compartilhada</dt><dd>${esc(s.user.name)} — Discord ID <code>${esc(s.user.discordId)}</code></dd></dl>
${ipWarn}<p>Se você não pediu para entrar nesse painel, feche esta página.</p>
<form method="post" action="/v1/auth/server-admin">
<input type="hidden" name="serverId" value="${esc(server.id)}">
${state ? `<input type="hidden" name="state" value="${esc(state)}">` : ''}
<input type="hidden" name="csrf" value="${esc(csrf)}">
<button type="submit">Entrar como admin</button> <a href="/panel">Cancelar</a>
</form>`);
  });

  app.post('/v1/auth/server-admin', async (req, reply) => {
    const b = (req.body && typeof req.body === 'object' ? req.body : {}) as Record<string, unknown>;
    const serverId = isServerId(b.serverId) ? b.serverId : null;
    if (config.rateLimit && !perIp.hit(ipKey(req.ip), now())) {
      audit(null, serverId, 'rate_limited');
      return tooMany(reply);
    }
    // CSRF: Origin obrigatório e igual ao Hub (ausente ou "null" → recusa).
    if (req.headers.origin !== hubOrigin) {
      audit(null, serverId, 'bad_origin');
      return forbidden(reply);
    }
    const state = b.state === undefined ? null : isAdminState(b.state) ? b.state : undefined;
    if (!serverId || state === undefined) {
      audit(null, serverId, 'bad_request');
      return badRequest(reply);
    }
    const s = sessionOf(req);
    if (!s) {
      audit(null, serverId, 'unauthorized');
      return html(reply, 401, 'Sessão expirada', `<p class="err">Sua sessão no Hub expirou.</p><p><a href="${esc(adminConfirmPath(serverId, state))}">Começar de novo</a></p>`);
    }
    if (typeof b.csrf !== 'string' || b.csrf === '') {
      audit(s.user, serverId, 'bad_csrf');
      return forbidden(reply);
    }
    if (config.rateLimit && !perUser.hit(s.user.id, now())) {
      audit(s.user, serverId, 'rate_limited');
      return tooMany(reply);
    }
    const tg = target(reply, s.user, serverId);
    if (!tg) return reply;
    const { server, status } = tg;
    // O token amarra sessão, servidor, state e a origem mostrada na confirmação (adminUrl mudou → recusa).
    if (!csrfOk(b.csrf, s.cookie, serverId, state, status.adminUrl)) {
      audit(s.user, serverId, 'bad_csrf');
      return forbidden(reply);
    }
    const iat = now();
    const payload: TicketPayload = {
      v: 1, kid: keyring.active.kid, sub: s.user.id, did: s.user.discordId, name: s.user.name,
      aud: `${server.id}#admin`, iat, exp: iat + TICKET_TTL_SECONDS, jti: randomUUID(),
    };
    const ticket = signTicket(payload, keyring.active.privateKey);
    audit(s.user, serverId, 'ticket_issued');
    // adminUrl já é uma origem normalizada; caminho fixo; ticket e state só têm [A-Za-z0-9_.-].
    return reply.code(303).header('location', `${status.adminUrl}/auth/callback#ticket=${ticket}${state ? `&state=${state}` : ''}`).send();
  });
}
