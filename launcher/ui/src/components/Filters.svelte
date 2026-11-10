<script lang="ts">
  import type { ServerListing } from "../lib/api";
  import { localesOf, tagsOf, type Filters } from "../lib/filters";

  let { servers, filters = $bindable() }: { servers: readonly ServerListing[]; filters: Filters } = $props();

  const tags = $derived(tagsOf(servers));
  const locales = $derived(localesOf(servers));

  function toggleTag(t: string) {
    filters.tags = filters.tags.includes(t) ? filters.tags.filter((x) => x !== t) : [...filters.tags, t];
  }

  function setLocale(e: Event) {
    const v = (e.currentTarget as HTMLSelectElement).value;
    filters.locale = v || null;
  }
</script>

<div class="filters" role="group" aria-label="Filtros">
  <input
    type="search"
    class="search"
    placeholder="Buscar servidor"
    aria-label="Buscar servidor"
    bind:value={filters.query}
  />
  {#each tags as t (t)}
    <button type="button" class="chip" aria-pressed={filters.tags.includes(t)} onclick={() => toggleTag(t)}
      >{t}</button
    >
  {/each}
  <select aria-label="Idioma" value={filters.locale ?? ""} onchange={setLocale}>
    <option value="">Todos os idiomas</option>
    {#each locales as l (l)}<option value={l}>{l}</option>{/each}
  </select>
  <label class="check"><input type="checkbox" bind:checked={filters.freeSlots} /> Com vagas</label>
</div>

<style>
  .filters {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
    flex-shrink: 0;
  }
  .search,
  select {
    padding: 6px 10px;
    border: 1px solid var(--border);
    border-radius: 16px;
    background: var(--chip);
    color: var(--text);
    font: inherit;
    font-size: 13px;
  }
  .search {
    width: 220px;
  }
  .chip {
    max-width: 140px;
    padding: 4px 10px;
    border: 1px solid transparent;
    border-radius: 14px;
    background: var(--chip);
    color: var(--chip-text);
    font: inherit;
    font-size: 12px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    cursor: pointer;
  }
  .chip[aria-pressed="true"] {
    border-color: var(--accent);
    background: var(--accent-soft);
    color: var(--accent);
  }
  .check {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 4px 8px;
    font-size: 13px;
    color: var(--chip-text);
    cursor: pointer;
  }
</style>
