import { chmodSync, existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { openDb } from './db';
import { Store } from './store';
import { ensureSystemServer } from './admin-tool';

/**
 * Ferramenta de linha de comando do Hub, rodada NA MÁQUINA do Hub (lê `HUB_DATA_DIR`, padrão ./data):
 *
 *   ensure-server --name <nome> --state <arquivo> [--description <t>] [--tag <t>]... [--locale pt-BR] [--featured]
 *
 * Cadastra (ou reaproveita) um servidor do usuário de sistema e grava `{serverId, key}` em `--state` (modo 600). A
 * chave nunca é impressa. Feito para o ambiente de teste online (docs/AMBIENTE-TESTE-ONLINE.md).
 */
function usage(msg?: string): never {
  if (msg) console.error(`erro: ${msg}`);
  console.error('uso: cli ensure-server --name <nome> --state <arquivo> [--description <t>] [--tag <t>]... [--locale pt-BR] [--featured]');
  process.exit(2);
}

const [cmd, ...rest] = process.argv.slice(2);
if (cmd !== 'ensure-server') usage(cmd ? `comando desconhecido: ${cmd}` : undefined);
const o: { name?: string; state?: string; description?: string; tags: string[]; locale?: string; featured: boolean } = { tags: [], featured: false };
for (let i = 0; i < rest.length; i++) {
  const a = rest[i];
  const val = () => rest[++i] ?? usage(`${a} precisa de um valor`);
  switch (a) {
    case '--name': o.name = val(); break;
    case '--state': o.state = val(); break;
    case '--description': o.description = val(); break;
    case '--tag': o.tags.push(val()); break;
    case '--locale': o.locale = val(); break;
    case '--featured': o.featured = true; break;
    default: usage(`opção desconhecida: ${a}`);
  }
}
if (!o.name || !o.state) usage('--name e --state são obrigatórios');

let previous: { serverId?: string; key?: string } | null = null;
if (existsSync(o.state)) {
  try { previous = JSON.parse(readFileSync(o.state, 'utf8')); } catch { previous = null; }
}
const dataDir = process.env.HUB_DATA_DIR ?? './data';
mkdirSync(dataDir, { recursive: true });
const store = new Store(openDb(join(dataDir, 'hub.sqlite')));
try {
  const r = ensureSystemServer(store, { name: o.name, description: o.description, tags: o.tags, locale: o.locale, featured: o.featured, previous },
    Math.floor(Date.now() / 1000));
  mkdirSync(dirname(o.state), { recursive: true });
  const tmp = `${o.state}.tmp`;
  writeFileSync(tmp, JSON.stringify({ serverId: r.server.id, key: r.key }, null, 2) + '\n', { mode: 0o600 });
  chmodSync(tmp, 0o600);
  renameSync(tmp, o.state);
  console.log(JSON.stringify({ serverId: r.server.id, name: r.server.name, featured: r.server.featured, action: r.action }));
} finally {
  store.close();
}
