#!/usr/bin/env node
// Confere o zip do mod (layout 2) antes de assinar/publicar.
//   node tools/check-mod-zip.mjs <zip> [--version X.Y.Z] [--protocol N]
// Sai 0 e imprime `ok <sha256>`; senão lista os problemas e sai 1.
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { checkModZip } from './lib/mod-zip-check.mjs';

export { checkModZip };

function main(argv) {
  let zip; const opts = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--version' || a === '--protocol') {
      const v = argv[++i];
      if (!v || v.startsWith('--')) { console.error(`${a} exige um valor`); return 2; }
      if (a === '--version') opts.version = v;
      else {
        opts.protocol = Number(v);
        if (!Number.isInteger(opts.protocol)) { console.error(`--protocol inválido: ${v}`); return 2; }
      }
    }
    else if (a.startsWith('--')) { console.error(`opção desconhecida: ${a}`); return 2; }
    else zip = a;
  }
  if (!zip) { console.error('uso: node tools/check-mod-zip.mjs <zip> [--version X.Y.Z] [--protocol N]'); return 2; }
  const buf = readFileSync(zip);
  const problems = checkModZip(buf, opts);
  if (problems.length) {
    console.error(`${zip}: ${problems.length} problema(s)`);
    for (const p of problems) console.error(`  - ${p}`);
    return 1;
  }
  console.log(`ok ${createHash('sha256').update(buf).digest('hex')}`);
  return 0;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) process.exitCode = main(process.argv.slice(2));
