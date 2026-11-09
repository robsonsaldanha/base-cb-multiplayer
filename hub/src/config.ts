export interface HubConfig {
  port: number;
  host: string;
  publicUrl: string;
  dataDir: string;
  discordClientId: string;
  discordClientSecret: string;
  platformAdmins: string[];
  devLogin: boolean;
  trustProxy: boolean;
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
    trustProxy: env.HUB_TRUST_PROXY === '1',
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
