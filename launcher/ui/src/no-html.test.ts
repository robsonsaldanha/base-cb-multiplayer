// @vitest-environment node
// Textos de servidores e do Rust são sempre texto: nenhum caminho do código-fonte injeta HTML.
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const SRC = fileURLToPath(new URL(".", import.meta.url));

function sources(dir: string): string[] {
  return readdirSync(dir).flatMap((n) => {
    const p = join(dir, n);
    if (statSync(p).isDirectory()) return sources(p);
    return /\.(svelte|ts)$/.test(n) && !/\.test\.ts$/.test(n) ? [p] : [];
  });
}

describe("sem HTML dinâmico", () => {
  it("nenhum {@html}, innerHTML, outerHTML, insertAdjacentHTML ou atributo style= no código", () => {
    const files = sources(SRC);
    expect(files.length).toBeGreaterThan(10);
    for (const f of files) {
      const text = readFileSync(f, "utf8");
      expect(text, f).not.toMatch(/\{@html|innerHTML|outerHTML|insertAdjacentHTML|document\.write/);
      if (f.endsWith(".svelte")) expect(text, f).not.toMatch(/<[a-zA-Z][^>]*\sstyle\s*=/);
    }
  });
});
