import { isIP } from 'node:net';

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

export interface HubConfig {
  port: number;
  host: string;
  publicUrl: string;
  dataDir: string;
  discordClientId: string;
  discordClientSecret: string;
  platformAdmins: string[];
  devLogin: boolean;
  trustProxy: string | false;
  allowPrivateServers: boolean;
  rateLimit: boolean;
  logLevel: string;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): HubConfig {
  const port = Number(env.HUB_PORT ?? 8787);
  const host = env.HUB_HOST ?? '127.0.0.1';
  const devLogin = env.HUB_DEV_LOGIN === '1';
  const cfg: HubConfig = {
    port,
    host,
    publicUrl: (env.HUB_PUBLIC_URL ?? `http://localhost:${port}`).replace(/\/$/, ''),
    dataDir: env.HUB_DATA_DIR ?? './data',
    discordClientId: env.DISCORD_CLIENT_ID ?? '',
    discordClientSecret: env.DISCORD_CLIENT_SECRET ?? '',
    platformAdmins: (env.HUB_PLATFORM_ADMINS ?? '').split(',').map((s) => s.trim()).filter(Boolean),
    devLogin,
    trustProxy: parseTrustProxy(env.HUB_TRUST_PROXY),
    allowPrivateServers: env.HUB_ALLOW_PRIVATE_SERVERS === '1',
    rateLimit: env.HUB_RATE_LIMIT !== '0',
    logLevel: env.HUB_LOG_LEVEL ?? 'info',
  };
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('HUB_PORT inválida');
  if (!devLogin && (!cfg.discordClientId || !cfg.discordClientSecret)) {
    throw new Error('DISCORD_CLIENT_ID e DISCORD_CLIENT_SECRET são obrigatórios (ou HUB_DEV_LOGIN=1 para testes locais)');
  }
  if (devLogin && host !== '127.0.0.1' && host !== 'localhost') {
    throw new Error('HUB_DEV_LOGIN=1 só é permitido com HUB_HOST=127.0.0.1');
  }
  return cfg;
}
