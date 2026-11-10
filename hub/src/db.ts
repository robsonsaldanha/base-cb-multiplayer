import Database from 'better-sqlite3';

export type DB = Database.Database;

const MIGRATIONS: string[] = [
  `
  CREATE TABLE users (
    id TEXT PRIMARY KEY, discord_id TEXT NOT NULL UNIQUE, name TEXT NOT NULL, avatar TEXT, created_at INTEGER NOT NULL
  );
  CREATE TABLE sessions (
    id TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, client TEXT NOT NULL,
    access_hash TEXT NOT NULL UNIQUE, access_exp INTEGER NOT NULL,
    refresh_hash TEXT UNIQUE, refresh_exp INTEGER, created_at INTEGER NOT NULL
  );
  CREATE TABLE oauth_states (state TEXT PRIMARY KEY, client TEXT NOT NULL, created_at INTEGER NOT NULL);
  CREATE TABLE login_codes (
    code_hash TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, created_at INTEGER NOT NULL
  );
  CREATE TABLE servers (
    id TEXT PRIMARY KEY, owner_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, key_hash TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '[]',
    locale TEXT NOT NULL DEFAULT 'pt-BR', use_global_bans INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL
  );
  CREATE TABLE server_status (
    server_id TEXT PRIMARY KEY REFERENCES servers(id) ON DELETE CASCADE, ip TEXT NOT NULL,
    game_port INTEGER NOT NULL, http_port INTEGER NOT NULL, players INTEGER NOT NULL, max_players INTEGER NOT NULL,
    version TEXT NOT NULL, protocol INTEGER NOT NULL, last_heartbeat INTEGER NOT NULL,
    verified INTEGER NOT NULL DEFAULT 0, verified_at INTEGER NOT NULL DEFAULT 0
  );
  CREATE TABLE global_bans (
    discord_id TEXT PRIMARY KEY, reason TEXT NOT NULL, created_by TEXT NOT NULL, created_at INTEGER NOT NULL
  );
  `,
  `
  ALTER TABLE oauth_states ADD COLUMN code_challenge TEXT;
  ALTER TABLE login_codes ADD COLUMN code_challenge TEXT;
  `,
  `
  ALTER TABLE server_status ADD COLUMN admin_url TEXT;
  `,
];

export function openDb(file: string): DB {
  const db = new Database(file);
  db.pragma('journal_mode = WAL');
  db.pragma('foreign_keys = ON');
  const version = db.pragma('user_version', { simple: true }) as number;
  for (let i = version; i < MIGRATIONS.length; i++) {
    db.transaction(() => {
      db.exec(MIGRATIONS[i]!);
      db.pragma(`user_version = ${i + 1}`);
    })();
  }
  return db;
}
