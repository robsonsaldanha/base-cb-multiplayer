import { open, readFile, readdir, realpath, stat } from 'node:fs/promises';
import { statSync } from 'node:fs';
import { isAbsolute, join, relative, resolve } from 'node:path';
import { Readable } from 'node:stream';
import { HttpError } from './errors';
import { isServedTag, type AssetStream, type GitHubReleases, type Release, type ReleaseAsset, type ReleasesLogger } from './github';

/**
 * Fonte de releases numa pasta local — SÓ para desenvolvimento e testes (`HUB_RELEASES_DIR`). Mesma interface do
 * cliente do GitHub: `<pasta>/<tag>/<asset>`, com a tag no padrão servido (`launcher-vX.Y.Z`, `mod-pN-vX.Y.Z`).
 *
 * - `notes.txt` dentro da pasta da tag vira o corpo (notas) do release e não é asset.
 * - `publishedAt` = data de modificação da pasta da tag.
 * - Só entram pastas e arquivos comuns (links simbólicos e junções são ignorados) com nomes simples; o download
 *   reabre o arquivo pelo caminho real e confere que ele continua dentro da pasta (troca por junção depois da
 *   listagem é recusada).
 * - Sem cache: cada `list()` relê a pasta (dá para publicar uma versão com o Hub rodando).
 */
export interface LocalReleasesOptions {
  dir: string;
  logger?: ReleasesLogger;
}

const NOTES = 'notes.txt';
const TEXT_MAX = 8 * 1024;
const NOTES_MAX = 64 * 1024;
/** Nome de asset: começa com letra/dígito; depois letras, dígitos, espaço e `._()+-` (ex. `CB Multiplayer (Teste local)_0.1.1_x64-setup.exe`). */
const ASSET_NAME = /^[A-Za-z0-9][A-Za-z0-9 ._()+-]{0,199}$/;

const notFound = () => new HttpError(404, 'not_found', 'arquivo não encontrado');

/** `child` está dentro de `parent` (ambos caminhos reais e absolutos). */
function inside(parent: string, child: string): boolean {
  const rel = relative(parent, child);
  return rel !== '' && !rel.startsWith('..') && !isAbsolute(rel);
}

export function createLocalReleases(opts: LocalReleasesOptions): GitHubReleases {
  const dir = resolve(opts.dir);
  let ok = false;
  try { ok = statSync(dir).isDirectory(); } catch { /* recusada abaixo */ }
  if (!ok) throw new Error(`HUB_RELEASES_DIR não é uma pasta existente: ${dir}`);

  /** Ids estáveis por (tag, nome, tamanho, mtime): conteúdo novo → id novo, como no GitHub. */
  const ids = new Map<string, number>();
  const byId = new Map<number, { tag: string; name: string; size: number }>();
  let nextId = 1;

  async function realRoot(): Promise<string> {
    return realpath(dir);
  }

  async function readRelease(root: string, tag: string): Promise<Release | null> {
    const tagDir = join(dir, tag);
    // Pasta da tag tem de ser ela mesma (sem junção/link para fora).
    if ((await realpath(tagDir)) !== join(root, tag)) return null;
    const st = await stat(tagDir);
    const assets: ReleaseAsset[] = [];
    let body = '';
    for (const e of await readdir(tagDir, { withFileTypes: true })) {
      if (!e.isFile()) continue; // links, junções e subpastas ficam de fora
      const path = join(tagDir, e.name);
      if (e.name === NOTES) {
        body = (await readFile(path, 'utf8')).slice(0, NOTES_MAX).trim();
        continue;
      }
      if (!ASSET_NAME.test(e.name)) continue;
      const fst = await stat(path);
      const key = `${tag}\u0000${e.name}\u0000${fst.size}\u0000${fst.mtimeMs}`;
      let id = ids.get(key);
      if (id === undefined) {
        id = nextId++;
        ids.set(key, id);
      }
      byId.set(id, { tag, name: e.name, size: fst.size });
      const asset: ReleaseAsset = { id, name: e.name, size: fst.size };
      if ((e.name.endsWith('.sig') || e.name.endsWith('.sha256')) && fst.size <= TEXT_MAX) {
        asset.text = await readFile(path, 'utf8');
      }
      assets.push(asset);
    }
    return { tag, draft: false, prerelease: false, publishedAt: st.mtime.toISOString(), body, assets };
  }

  return {
    async list() {
      const root = await realRoot();
      const out: Release[] = [];
      for (const e of await readdir(dir, { withFileTypes: true })) {
        if (!e.isDirectory() || !isServedTag(e.name)) continue;
        try {
          const r = await readRelease(root, e.name);
          if (r) out.push(r);
        } catch (err) {
          opts.logger?.warn({ service: 'local-releases', tag: e.name, error: err instanceof Error ? err.name : 'unknown' },
            'release local ilegível; ignorado');
        }
      }
      // Ids que não existem mais deixam de valer para download.
      const live = new Set(out.flatMap((r) => r.assets.map((a) => a.id)));
      for (const id of byId.keys()) if (!live.has(id)) byId.delete(id);
      for (const [k, id] of ids) if (!live.has(id)) ids.delete(k);
      return out;
    },
    async download(assetId) {
      const entry = byId.get(assetId);
      if (!entry) throw notFound();
      const root = await realRoot();
      const path = join(dir, entry.tag, entry.name);
      let real: string;
      try { real = await realpath(path); } catch { throw notFound(); }
      if (real !== join(root, entry.tag, entry.name) || !inside(root, real)) throw notFound();
      const fh = await open(real, 'r').catch(() => { throw notFound(); });
      try {
        const st = await fh.stat();
        if (!st.isFile() || st.size !== entry.size) throw notFound();
      } catch (e) {
        await fh.close();
        throw e;
      }
      const web = Readable.toWeb(fh.createReadStream({ autoClose: true })) as unknown as ReadableStream<Uint8Array>;
      return Object.assign(web, { size: entry.size }) as AssetStream;
    },
  };
}
