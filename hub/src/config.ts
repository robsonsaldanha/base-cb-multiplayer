import { isIP } from 'node:net';
import { resolve } from 'node:path';

/** Lista de proxies confiáveis (IPs/CIDRs); vazio ou 0 → desligado. Nunca aceita booleano. */
function parseTrustProxy(raw: string | undefined): string | false {
  const v = (raw ?? '').trim();
  if (v === '' || v === '0') return false;
  const items = v.split(',').map((s) => s.trim()).filter(Boolean);
  for (const it of items) {
    const [addr, bits, extra] = it.split('/');
    const max = addr && isIP(addr) === 6 ? 128 : 32;
    const okBits = bits === undefined || (/^[0-9]{1,3}$/.test(bits) && Number(bits) <= max);
    if (!addr || !isIP(addr) || !okBits || extra !== undefined) {
      throw new Error(`HUB_TRUST_PROXY deve ser uma lista de IPs/CIDRs dos proxies confiáveis (ex.: 127.0.0.1,::1), não um booleano: "${it}"`);
    }
  }
  return items.join(',');
}

/** Inteiro decimal em [min, max]; vazio → padrão. */
function parseIntEnv(raw: string | undefined, name: string, def: number, min: number, max: number): number {
  const v = (raw ?? '').trim();
  if (v === '') return def;
  const n = /^\d{1,9}$/.test(v) ? Number(v) : NaN;
  if (!(n >= min && n <= max)) throw new Error(`${name} deve ser um inteiro entre ${min} e ${max}: "${v}"`);
  return n;
}

function parseDlMax(raw: string | undefined): number {
  const v = (raw ?? '').trim();
  if (v === '') return 32;
  if (!/^[1-9]\d{0,4}$/.test(v)) throw new Error(`HUB_DL_MAX_CONCURRENT deve ser um inteiro entre 1 e 99999: "${v}"`);
  return Number(v);
}

export interface HubConfig {
  port: number;
  host: string;
  publicUrl: string;
  dataDir: string;
  discordClientId: string;
  discordClientSecret: string;
  platformAdmins: string[];
  devLogin: boolean;
  /**
   * Hub de teste PÚBLICO (`HUB_STAGING=1`, ex. o ambiente de teste online): permite `HUB_RELEASES_DIR` sem dev-login e
   * sobe sem o app Discord configurado (o login responde 503 `login_unavailable` até `DISCORD_CLIENT_ID`/`_SECRET`
   * existirem). Exige `HUB_PUBLIC_URL` https; recusado com `HUB_DEV_LOGIN=1` e com `NODE_ENV=production`.
   */
  staging: boolean;
  trustProxy: string | false;
  allowPrivateServers: boolean;
  rateLimit: boolean;
  logLevel: string;
  /** Token do GitHub (só leitura de Releases do repo privado). Vazio → rotas de update/mod/dl respondem 503. Nunca logar. */
  githubToken: string;
  /** `dono/repo` dos Releases (`launcher-vX.Y.Z`, `mod-pN-vX.Y.Z`). */
  releasesRepo: string;
  /**
   * Só desenvolvimento: pasta local de releases (`<pasta>/<tag>/<asset>`) no lugar do GitHub (`HUB_RELEASES_DIR`,
   * caminho absoluto; '' = desligado). Mutuamente exclusivo com `githubToken`; recusado com `NODE_ENV=production`.
   */
  releasesDir: string;
  /** Downloads repassados ao mesmo tempo, todos os clientes (`HUB_DL_MAX_CONCURRENT`, padrão 32). */
  dlMaxConcurrent: number;
  /** Download cujo cliente não drena o socket por este tempo é derrubado (`HUB_DL_STALL_SECONDS`, padrão 30). */
  dlStallTimeoutMs: number;
  /** Taxa mínima de entrega em KiB/s, média da última janela, depois de uma janela de carência (`HUB_DL_MIN_KBPS`, padrão 32; 0 desliga). */
  dlMinKBps: number;
  /** Janela (e carência) da taxa mínima, em ms (30 s; injetável nos testes). */
  dlRateWindowMs: number;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): HubConfig {
  const port = Number(env.HUB_PORT ?? 8787);
  const host = env.HUB_HOST ?? '127.0.0.1';
  const devLogin = env.HUB_DEV_LOGIN === '1';
  const staging = env.HUB_STAGING === '1';
  const cfg: HubConfig = {
    port,
    host,
    publicUrl: (env.HUB_PUBLIC_URL ?? `http://localhost:${port}`).replace(/\/$/, ''),
    dataDir: env.HUB_DATA_DIR ?? './data',
    discordClientId: env.DISCORD_CLIENT_ID ?? '',
    discordClientSecret: env.DISCORD_CLIENT_SECRET ?? '',
    platformAdmins: (env.HUB_PLATFORM_ADMINS ?? '').split(',').map((s) => s.trim()).filter(Boolean),
    devLogin,
    staging,
    trustProxy: parseTrustProxy(env.HUB_TRUST_PROXY),
    allowPrivateServers: env.HUB_ALLOW_PRIVATE_SERVERS === '1',
    rateLimit: env.HUB_RATE_LIMIT !== '0',
    logLevel: env.HUB_LOG_LEVEL ?? 'info',
    githubToken: (env.HUB_GITHUB_TOKEN ?? '').trim(),
    releasesRepo: (env.HUB_RELEASES_REPO ?? '').trim(),
    releasesDir: (env.HUB_RELEASES_DIR ?? '').trim() === '' ? '' : resolve((env.HUB_RELEASES_DIR ?? '').trim()),
    dlMaxConcurrent: parseDlMax(env.HUB_DL_MAX_CONCURRENT),
    dlStallTimeoutMs: parseIntEnv(env.HUB_DL_STALL_SECONDS, 'HUB_DL_STALL_SECONDS', 30, 1, 3600) * 1000,
    dlMinKBps: parseIntEnv(env.HUB_DL_MIN_KBPS, 'HUB_DL_MIN_KBPS', 32, 0, 1_000_000),
    dlRateWindowMs: 30_000,
  };
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('HUB_PORT inválida');
  if (staging) assertStagingSafe(cfg, env);
  if (!devLogin && !staging && (!cfg.discordClientId || !cfg.discordClientSecret)) {
    throw new Error('DISCORD_CLIENT_ID e DISCORD_CLIENT_SECRET são obrigatórios (ou HUB_DEV_LOGIN=1 para testes locais)');
  }
  const repoOk = /^[A-Za-z0-9_.-]{1,100}\/[A-Za-z0-9_.-]{1,100}$/.test(cfg.releasesRepo)
    && cfg.releasesRepo.split('/').every((p) => p !== '.' && p !== '..');
  if (cfg.githubToken && !repoOk) {
    throw new Error('HUB_RELEASES_REPO (dono/repo) é obrigatório quando HUB_GITHUB_TOKEN está definido');
  }
  if (cfg.releasesDir) {
    // NODE_ENV não é definido pelo deploy deste projeto: a trava que vale é o dev-login, que já prende o Hub a
    // HUB_HOST/HUB_PUBLIC_URL locais e sem proxy (assertDevLoginSafe).
    if (!devLogin && !staging) {
      throw new Error('HUB_RELEASES_DIR (releases locais, só para testes) exige HUB_DEV_LOGIN=1 (Hub preso a esta máquina) ou HUB_STAGING=1 (Hub de teste público)');
    }
    if (cfg.githubToken) throw new Error('HUB_RELEASES_DIR e HUB_GITHUB_TOKEN são mutuamente exclusivos (pasta local de releases é só para testes)');
    if ((env.NODE_ENV ?? '').trim() === 'production') throw new Error('HUB_RELEASES_DIR (releases locais, só para testes) é recusado com NODE_ENV=production');
  }
  if (devLogin) assertDevLoginSafe(cfg);
  return cfg;
}

/** Hub de teste público: https, sem dev-login e nunca marcado como produção. */
function assertStagingSafe(cfg: HubConfig, env: NodeJS.ProcessEnv): void {
  if (cfg.devLogin) throw new Error('HUB_STAGING=1 (Hub de teste público) não pode ser combinado com HUB_DEV_LOGIN=1');
  if ((env.NODE_ENV ?? '').trim() === 'production') throw new Error('HUB_STAGING=1 (Hub de teste) é recusado com NODE_ENV=production');
  if (!cfg.publicUrl.startsWith('https://')) throw new Error(`HUB_STAGING=1 exige HUB_PUBLIC_URL https; recebido "${cfg.publicUrl}"`);
}

/** Login de teste só em máquina local: escuta local, URL pública local e sem proxy na frente. */
function assertDevLoginSafe(cfg: HubConfig): void {
  if (!['127.0.0.1', 'localhost', '::1'].includes(cfg.host)) {
    throw new Error(`HUB_DEV_LOGIN=1 só é permitido com HUB_HOST local (127.0.0.1, localhost ou ::1); recebido "${cfg.host}"`);
  }
  let hostname = '';
  try { hostname = new URL(cfg.publicUrl).hostname; } catch { /* inválida → recusada abaixo */ }
  if (!['localhost', '127.0.0.1', '[::1]'].includes(hostname)) {
    throw new Error(`HUB_DEV_LOGIN=1 só é permitido com HUB_PUBLIC_URL local (localhost, 127.0.0.1 ou [::1]); recebido "${cfg.publicUrl}"`);
  }
  if (cfg.trustProxy !== false) {
    throw new Error('HUB_DEV_LOGIN=1 não pode ser usado com HUB_TRUST_PROXY (o hub estaria atrás de um proxy, logo exposto)');
  }
}
