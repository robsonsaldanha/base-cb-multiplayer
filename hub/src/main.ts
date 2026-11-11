import { mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { loadConfig } from './config';
import { openDb } from './db';
import { Store } from './store';
import { loadOrCreateKeyRing } from './keyring';
import { createDiscordClient } from './discord';
import { httpProbe } from './probe';
import { buildApp } from './app';
import { createGitHubReleases } from './github';
import { createLocalReleases } from './local-releases';

const config = loadConfig();
mkdirSync(config.dataDir, { recursive: true });
const store = new Store(openDb(join(config.dataDir, 'hub.sqlite')));
const keyring = loadOrCreateKeyRing(join(config.dataDir, 'keys'));
const discord = createDiscordClient(config.discordClientId, config.discordClientSecret);
const now = () => Math.floor(Date.now() / 1000);
// O logger do app só existe depois do buildApp; os avisos do cliente do GitHub (nunca com token) vão para ele.
let releasesLog: { warn(obj: object, msg: string): void } | undefined;
const releasesLogger = { warn: (o: object, m: string) => releasesLog?.warn(o, m) };
const releases = config.githubToken
  ? createGitHubReleases({ token: config.githubToken, repo: config.releasesRepo, logger: releasesLogger })
  : config.releasesDir
    ? createLocalReleases({ dir: config.releasesDir, logger: releasesLogger })
    : undefined;
const app = await buildApp({ config, store, keyring, discord, probe: httpProbe, now, releases });
releasesLog = app.log;
const PURGE_INTERVAL_MS = 10 * 60 * 1000;
function purge(): void {
  try { store.purgeExpired(now()); } catch (err) { app.log.error(err); }
}
purge();
const purgeTimer = setInterval(purge, PURGE_INTERVAL_MS);
purgeTimer.unref();
try {
  await app.listen({ port: config.port, host: config.host });
} catch (err) {
  app.log.error(err);
  clearInterval(purgeTimer);
  store.close();
  process.exit(1);
}
app.log.info(`Hub em ${config.publicUrl} (chave ativa ${keyring.active.kid})`);
if (config.releasesDir) app.log.warn(`releases LOCAIS de ${config.releasesDir} (HUB_RELEASES_DIR, só para testes)`);
if (config.staging) app.log.warn('HUB_STAGING=1: Hub de TESTE público (releases locais permitidas; não é produção)');
if (!config.devLogin && (!config.discordClientId || !config.discordClientSecret)) {
  app.log.warn('login com Discord desligado até DISCORD_CLIENT_ID e DISCORD_CLIENT_SECRET serem definidos (rotas de login respondem 503 login_unavailable)');
}

let closing = false;
async function shutdown(signal: string): Promise<void> {
  if (closing) return;
  closing = true;
  app.log.info(`${signal} recebido, encerrando`);
  clearInterval(purgeTimer);
  try {
    await app.close();
    store.close();
    process.exit(0);
  } catch (err) {
    app.log.error(err);
    process.exit(1);
  }
}
process.on('SIGINT', () => void shutdown('SIGINT'));
process.on('SIGTERM', () => void shutdown('SIGTERM'));
