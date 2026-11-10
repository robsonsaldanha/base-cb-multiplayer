import { privateKeyFromSeed, publicKeyToRaw } from '@cbmp/contracts';
import type { HubConfig } from '../src/config';
import { openDb } from '../src/db';
import { Store } from '../src/store';
import { buildApp } from '../src/app';
import type { KeyRing } from '../src/keyring';
import type { DiscordClient, DiscordUser } from '../src/discord';
import type { ProbeFn } from '../src/probe';
import type { GitHubReleases } from '../src/github';

export const T0 = 1_800_000_000;

export function testConfig(over: Partial<HubConfig> = {}): HubConfig {
  return {
    port: 8787, host: '127.0.0.1', publicUrl: 'http://localhost:8787', dataDir: ':memory:',
    discordClientId: 'cid', discordClientSecret: 'secret', platformAdmins: ['999'],
    devLogin: false, trustProxy: false, allowPrivateServers: true, rateLimit: false, logLevel: 'silent',
    githubToken: '', releasesRepo: '', ...over,
  };
}

export function testKeyRing(): KeyRing {
  const privateKey = privateKeyFromSeed(Buffer.alloc(32, 3));
  return { active: { kid: 'kt', privateKey }, publicKeys: () => [{ kid: 'kt', publicKey: publicKeyToRaw(privateKey) }] };
}

export class FakeDiscord implements DiscordClient {
  users = new Map<string, DiscordUser>(); // code -> usuário
  async exchangeCode(code: string): Promise<string> {
    if (!this.users.has(code)) throw new Error('código inválido');
    return `tok_${code}`;
  }
  async getUser(accessToken: string): Promise<DiscordUser> {
    return this.users.get(accessToken.slice(4))!;
  }
}

export async function makeTestApp(opts: { config?: Partial<HubConfig>; probe?: ProbeFn; logStream?: NodeJS.WritableStream; releases?: GitHubReleases } = {}) {
  const clock = { t: T0 };
  const store = new Store(openDb(':memory:'));
  const discord = new FakeDiscord();
  const probeAnswers = new Map<string, string>(); // "host:port" -> serverId
  const probe: ProbeFn = opts.probe ?? (async (host, port) => {
    const id = probeAnswers.get(`${host}:${port}`);
    return id ? { serverId: id } : null;
  });
  const config = testConfig(opts.config);
  const keyring = testKeyRing();
  const app = await buildApp({ config, store, keyring, discord, probe, now: () => clock.t, logStream: opts.logStream, releases: opts.releases });
  return { app, store, clock, discord, probeAnswers, keyring, config };
}

/** Cria usuário + sessão de launcher direto no store. */
export function loginAs(store: Store, discordId: string, name = `user${discordId}`, now = T0) {
  const user = store.upsertUserFromDiscord({ discordId, name, avatar: null }, now);
  const tokens = store.createSession(user.id, 'launcher', now);
  return { user, tokens, auth: { authorization: `Bearer ${tokens.access}` } };
}
