<script lang="ts">
  // Tags e idioma do servidor: sempre texto, em chips.
  let { tags, locale }: { tags: readonly string[]; locale: string } = $props();
</script>

<span class="chips">
  {#if locale}<span class="chip locale" title="Idioma">{locale}</span>{/if}
  {#each tags.slice(0, 6) as t, i (i)}<span class="chip">{t}</span>{/each}
</span>

<style>
  .chips {
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
    min-width: 0;
  }
  .chip {
    max-width: 120px;
    padding: 1px 8px;
    border-radius: 10px;
    background: var(--chip);
    color: var(--chip-text);
    font-size: 11px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .locale {
    color: var(--accent);
  }
</style>
