import type { Store, ServerRow } from './store';
import { CreateServerBody } from './routes/owner';

/**
 * Cadastro de servidor pela própria máquina do Hub (sem login Discord), para ambientes de teste: o servidor fica no
 * nome de um usuário de sistema (Discord ID `0`, que o Discord nunca emite) e pode ser marcado como destaque. Usado
 * por `src/cli.ts ensure-server` (ambiente de teste online, docs/AMBIENTE-TESTE-ONLINE.md).
 */
export const SYSTEM_DISCORD_ID = '0';
export const SYSTEM_NAME = 'Plataforma (sistema)';

export interface EnsureServerInput {
  name: string;
  description?: string;
  tags?: string[];
  locale?: string;
  featured: boolean;
  /** Estado de uma execução anterior (id + chave); chave que não confere com o id é trocada. */
  previous?: { serverId?: string; key?: string } | null;
}

export interface EnsureServerResult {
  server: ServerRow;
  /** Chave válida do servidor (a anterior, se ainda confere, ou uma nova). Nunca logar. */
  key: string;
  action: 'created' | 'reused' | 'rotated';
}

/** Idempotente: reaproveita o servidor (pelo id anterior ou pelo nome) do usuário de sistema; cria se não houver. */
export function ensureSystemServer(store: Store, input: EnsureServerInput, now: number): EnsureServerResult {
  const body = CreateServerBody.parse({
    name: input.name, description: input.description ?? '', tags: input.tags ?? [], locale: input.locale ?? 'pt-BR',
  });
  const owner = store.upsertUserFromDiscord({ discordId: SYSTEM_DISCORD_ID, name: SYSTEM_NAME, avatar: null }, now);
  const mine = store.listServersByOwner(owner.id).map((s) => s.server);
  const prevId = input.previous?.serverId;
  const found = mine.find((s) => s.id === prevId) ?? mine.find((s) => s.name === body.name);
  let result: EnsureServerResult;
  if (!found) {
    const { server, key } = store.createServer(owner.id, body, now);
    result = { server, key, action: 'created' };
  } else {
    store.updateServer(found.id, owner.id, { name: body.name, description: body.description, tags: body.tags, locale: body.locale });
    const prevKey = input.previous?.key;
    if (prevKey && found.id === prevId && store.getServerByKey(prevKey)?.id === found.id) {
      result = { server: store.getServer(found.id)!, key: prevKey, action: 'reused' };
    } else {
      const key = store.rotateKey(found.id, owner.id)!;
      result = { server: store.getServer(found.id)!, key, action: 'rotated' };
    }
  }
  const featured = store.setFeatured(result.server.id, input.featured)!;
  return { ...result, server: featured };
}
