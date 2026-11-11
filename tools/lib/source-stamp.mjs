// Carimbo do código que entra no binário do launcher (testes e examples ficam de fora). O tools/dev-env.mjs grava o
// carimbo junto de cada release de teste (`releases\launcher-vX.Y.Z\.source-stamp`) e só reaproveita o release se ele
// não mudou; o tools/make-test-kit.mjs recusa empacotar um release com carimbo diferente do código atual.

import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

export const LAUNCHER_INPUTS = ['launcher/src-tauri/src', 'launcher/src-tauri/Cargo.toml', 'launcher/src-tauri/Cargo.lock',
  'launcher/src-tauri/build.rs', 'launcher/src-tauri/tauri.conf.json', 'launcher/src-tauri/tauri.localtest.conf.json',
  'launcher/src-tauri/tauri.onlinetest.conf.json',
  'launcher/src-tauri/capabilities', 'launcher/src-tauri/permissions', 'launcher/src-tauri/keys', 'launcher/src-tauri/icons',
  // A UI inteira (node_modules/dist ficam de fora pelo .gitignore; testes pelo filtro abaixo).
  'launcher/ui', 'package.json', 'package-lock.json'];

/** sha256 do conteúdo dos arquivos que entram no launcher (versionados e novos não ignorados; sem testes). */
export function sourceStamp(repo) {
  const r = spawnSync('git', ['ls-files', '-z', '--cached', '--others', '--exclude-standard', '--', ...LAUNCHER_INPUTS], { cwd: repo, encoding: 'utf8' });
  if (r.status !== 0) throw new Error('git ls-files falhou (precisa rodar dentro do repositório git)');
  const files = [...new Set(r.stdout.split('\0').filter(Boolean))].sort();
  const h = createHash('sha256');
  for (const f of files) {
    if (/(^|\/)(tests|__tests__)\//.test(f) || /\.test\.ts$/.test(f)) continue;
    const full = join(repo, f);
    if (!existsSync(full)) continue; // apagado e ainda não commitado
    h.update(`${f}\0`).update(readFileSync(full)).update('\0');
  }
  return h.digest('hex');
}
