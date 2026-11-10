import type { ServerListing } from "./api";

export interface Filters {
  query: string;
  /** Todas as escolhidas precisam estar no servidor. */
  tags: string[];
  locale: string | null;
  freeSlots: boolean;
}

export const EMPTY_FILTERS: Filters = { query: "", tags: [], locale: null, freeSlots: false };

/** Minúsculas e sem acentos, para a busca. */
export function fold(s: string): string {
  return s.normalize("NFD").replace(/\p{M}/gu, "").toLowerCase();
}

export function applyFilters(servers: readonly ServerListing[], f: Filters): ServerListing[] {
  const q = fold(f.query.trim());
  return servers.filter(
    (s) =>
      (!q || fold(s.name).includes(q)) &&
      f.tags.every((t) => s.tags.includes(t)) &&
      (!f.locale || s.locale === f.locale) &&
      (!f.freeSlots || s.players < s.maxPlayers),
  );
}

/** Tags da lista, mais comuns primeiro (no máximo `max`). */
export function tagsOf(servers: readonly ServerListing[], max = 12): string[] {
  const count = new Map<string, number>();
  for (const s of servers) for (const t of new Set(s.tags)) count.set(t, (count.get(t) ?? 0) + 1);
  return [...count.entries()]
    .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
    .slice(0, max)
    .map(([t]) => t);
}

export function localesOf(servers: readonly ServerListing[]): string[] {
  return [...new Set(servers.map((s) => s.locale).filter(Boolean))].sort();
}
