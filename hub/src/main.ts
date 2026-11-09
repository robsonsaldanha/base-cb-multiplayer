import { mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { loadConfig } from './config';
import { openDb } from './db';
import { Store } from './store';
import { loadOrCreateKeyRing } from './keyring';
import { createDiscordClient } from './discord';
import { httpProbe } from './probe';
import { buildApp } from './app';

const config = loadConfig();
mkdirSync(config.dataDir, { recursive: true });
const store = new Store(openDb(join(config.dataDir, 'hub.sqlite')));
const keyring = loadOrCreateKeyRing(join(config.dataDir, 'keys'));
const discord = createDiscordClient(config.discordClientId, config.discordClientSecret);
const app = await buildApp({ config, store, keyring, discord, probe: httpProbe, now: () => Math.floor(Date.now() / 1000) });
await app.listen({ port: config.port, host: config.host });
app.log.info(`Hub em ${config.publicUrl} (chave ativa ${keyring.active.kid})`);
