// Confere a UI compilada (`dist/`) contra a CSP do app: nada inline que `script-src 'self'`/`style-src 'self'`
// bloquearia no app instalado. Em `tauri dev` a CSP não é aplicada, então só este passo pega isso antes do release.
// Uso: node scripts/check-dist.mjs [pasta-dist]
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, extname } from "node:path";
import { fileURLToPath } from "node:url";

const dist = process.argv[2] ?? fileURLToPath(new URL("../dist", import.meta.url));

function files(dir) {
  return readdirSync(dir).flatMap((n) => {
    const p = join(dir, n);
    return statSync(p).isDirectory() ? files(p) : [p];
  });
}

const problems = [];
for (const file of files(dist)) {
  const ext = extname(file);
  const text = readFileSync(file, "utf8");
  if (ext === ".html") {
    for (const m of text.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/gi)) {
      if (!/\bsrc=/i.test(m[1]) || m[2].trim()) problems.push(`${file}: <script> inline`);
    }
    if (/<style\b/i.test(text)) problems.push(`${file}: <style> inline`);
    if (/\sstyle\s*=/i.test(text)) problems.push(`${file}: atributo style=`);
    if (/\son[a-z]+\s*=/i.test(text)) problems.push(`${file}: handler on*= inline`);
    for (const m of text.matchAll(/\s(?:src|href)\s*=\s*["']?(https?:)?\/\//gi)) {
      problems.push(`${file}: recurso remoto (${m[0].trim()})`);
    }
  }
  if (ext === ".css" && /@import\s+url\(\s*["']?https?:/i.test(text)) {
    problems.push(`${file}: @import remoto`);
  }
  if (ext === ".css" && /url\(\s*["']?https?:/i.test(text)) problems.push(`${file}: url() remoto`);
}

if (problems.length) {
  console.error(`check-dist: ${problems.length} problema(s) com a CSP:\n- ${problems.join("\n- ")}`);
  process.exit(1);
}
console.log(`check-dist: ok (${dist})`);
