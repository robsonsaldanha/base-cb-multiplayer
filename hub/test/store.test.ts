import { beforeEach, describe, expect, it } from 'vitest';
import { openDb } from '../src/db';
import { LAUNCHER_ACCESS_TTL, LAUNCHER_REFRESH_TTL, LOGIN_CODE_TTL, OAUTH_STATE_TTL, PANEL_ACCESS_TTL, Store } from '../src/store';

let store: Store;
const T = 1_800_000_000;
beforeEach(() => { store = new Store(openDb(':memory:')); });

const hb = { name: 'Srv', description: '', players: 3, maxPlayers: 64, gamePort: 30500, httpPort: 30500, version: '0.1', protocol: 1, tags: ['rp'], locale: 'pt-BR' };

describe('Store', () => {
  it('upsert de usuário por discord id', () => {
    const a = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const b = store.upsertUserFromDiscord({ discordId: '1', name: 'A2', avatar: 'x' }, T + 5);
    expect(b.id).toBe(a.id);
    expect(store.getUserById(a.id)).toEqual({ id: a.id, discordId: '1', name: 'A2', avatar: 'x' });
  });

  it('sessão launcher: access expira, refresh gira tokens', () => {
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const s = store.createSession(u.id, 'launcher', T);
    expect(s.access.startsWith('at_')).toBe(true);
    expect(s.refresh?.startsWith('rt_')).toBe(true);
    expect(store.findUserByAccess(s.access, T + 3599)?.id).toBe(u.id);
    expect(store.findUserByAccess(s.access, T + 3601)).toBeNull();
    const r = store.refreshSession(s.refresh!, T + 3601)!;
    expect(r.access).not.toBe(s.access);
    expect(store.refreshSession(s.refresh!, T + 3602)).toBeNull(); // refresh antigo não vale mais
    expect(store.findUserByAccess(r.access, T + 3602)?.id).toBe(u.id);
  });

  it('sessão painel não tem refresh e dura 12 h', () => {
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const s = store.createSession(u.id, 'panel', T);
    expect(s.refresh).toBeNull();
    expect(store.findUserByAccess(s.access, T + 12 * 3600 - 1)).not.toBeNull();
    expect(store.findUserByAccess(s.access, T + 12 * 3600 + 1)).toBeNull();
  });

  it('oauth state e login code são de uso único e expiram', () => {
    const st = store.createOauthState('launcher', T, 'CH');
    expect(store.consumeOauthState(st, T + 1)).toEqual({ client: 'launcher', codeChallenge: 'CH', returnTo: null, clientState: null });
    const ret = store.createOauthState('panel', T, undefined, { serverId: 'srv_AAAAAAAAAAAA', state: null });
    expect(store.consumeOauthState(ret, T + 1)).toEqual({ client: 'panel', codeChallenge: null, returnTo: { serverId: 'srv_AAAAAAAAAAAA', state: null }, clientState: null });
    expect(store.consumeOauthState(st, T + 2)).toBeNull();
    expect(store.consumeOauthState(store.createOauthState('panel', T), T + 601)).toBeNull();
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const code = store.createLoginCode(u.id, T, 'CH');
    expect(store.consumeLoginCode(code, T + 59)).toEqual({ userId: u.id, codeChallenge: 'CH' });
    expect(store.consumeLoginCode(code, T + 59)).toBeNull();
    expect(store.consumeLoginCode(store.createLoginCode(u.id, T, 'CH'), T + 61)).toBeNull();
  });

  it('servidor: cria com chave, acha por chave, gira chave, apaga só do dono', () => {
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const other = store.upsertUserFromDiscord({ discordId: '2', name: 'B', avatar: null }, T);
    const { server, key } = store.createServer(u.id, { name: 'Meu', description: 'd', tags: ['rp'], locale: 'pt-BR', useGlobalBans: true }, T);
    expect(server.id).toMatch(/^srv_[A-Za-z0-9_-]{12}$/);
    expect(key).toMatch(/^sk_[A-Za-z0-9_-]{43}$/);
    expect(store.getServerByKey(key)?.id).toBe(server.id);
    const k2 = store.rotateKey(server.id, u.id)!;
    expect(store.getServerByKey(key)).toBeNull();
    expect(store.getServerByKey(k2)?.id).toBe(server.id);
    expect(store.rotateKey(server.id, other.id)).toBeNull();
    expect(store.deleteServer(server.id, other.id)).toBe(false);
    expect(store.deleteServer(server.id, u.id)).toBe(true);
    expect(store.getServer(server.id)).toBeNull();
  });

  it('heartbeat + lista online respeita verificação e janela de 90 s', () => {
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const { server } = store.createServer(u.id, { name: 'Meu', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T);
    store.recordHeartbeat(server.id, hb, '1.2.3.4', T);
    expect(store.listOnline(T)).toEqual([]); // não verificado
    store.setVerified(server.id, true, T);
    expect(store.listOnline(T + 90).map((s) => s.server.id)).toEqual([server.id]);
    expect(store.getServer(server.id)?.name).toBe('Srv'); // heartbeat atualiza nome
    expect(store.listOnline(T + 91)).toEqual([]);
  });

  it('bans globais', () => {
    store.addGlobalBan('9', 'cheat', 'admin1', T);
    expect(store.isGloballyBanned('9')).toBe(true);
    expect(store.listGlobalBans()).toEqual([{ discordId: '9', reason: 'cheat', createdBy: 'admin1', createdAt: T }]);
    expect(store.removeGlobalBan('9')).toBe(true);
    expect(store.isGloballyBanned('9')).toBe(false);
  });

  it('updateServer ignora undefined no patch', () => {
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const { server } = store.createServer(u.id, { name: 'Meu', description: 'd', tags: ['rp'], locale: 'pt-BR', useGlobalBans: true }, T);
    const up = store.updateServer(server.id, u.id, { name: undefined, description: 'novo', useGlobalBans: undefined })!;
    expect(up).toMatchObject({ name: 'Meu', description: 'novo', useGlobalBans: true, tags: ['rp'] });
  });

  it('purgeExpired remove states, códigos e sessões vencidas e mantém o resto', () => {
    const db = openDb(':memory:');
    const st = new Store(db);
    const count = (t: string) => (db.prepare(`SELECT COUNT(*) AS n FROM ${t}`).get() as { n: number }).n;
    const u = st.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const now = T + 100_000;
    st.createOauthState('panel', now - OAUTH_STATE_TTL - 1);      // vencido
    const keepState = st.createOauthState('panel', now - OAUTH_STATE_TTL); // no limite: fica
    st.createLoginCode(u.id, now - LOGIN_CODE_TTL - 1, 'x');        // vencido
    const keepCode = st.createLoginCode(u.id, now - 10, 'x');
    const panelOld = st.createSession(u.id, 'panel', now - PANEL_ACCESS_TTL - 1);   // access vencido, sem refresh → sai
    const panelNew = st.createSession(u.id, 'panel', now - 10);
    const launcherRefreshOk = st.createSession(u.id, 'launcher', now - LAUNCHER_ACCESS_TTL - 1); // access vencido, refresh válido → fica
    st.createSession(u.id, 'launcher', now - LAUNCHER_REFRESH_TTL - 1); // ambos vencidos → sai
    st.purgeExpired(now);
    expect(count('oauth_states')).toBe(1);
    expect(st.consumeOauthState(keepState, now)).not.toBeNull();
    expect(count('login_codes')).toBe(1);
    expect(st.consumeLoginCode(keepCode, now)).not.toBeNull();
    expect(count('sessions')).toBe(2);
    expect(st.findUserByAccess(panelOld.access, now - PANEL_ACCESS_TTL - 1)).toBeNull();
    expect(st.findUserByAccess(panelNew.access, now)).not.toBeNull();
    expect(st.refreshSession(launcherRefreshOk.refresh!, now)).not.toBeNull();
  });
});
