<script lang="ts">
  // Estado da lista: desatualizada (Hub fora, `servers-cache.json`) ou sem lista nenhuma.
  import { describeError } from "../lib/errors";
  import { formatDate } from "../lib/format";
  import { clearServerBlocks } from "../lib/play";
  import { loadServers, serverList } from "../lib/servers";

  const err = $derived($serverList.error ? describeError($serverList.error) : null);

  function reload() {
    clearServerBlocks();
    void loadServers();
  }
</script>

{#if $serverList.status === "stale" || $serverList.status === "error"}
  <div class="status" class:error={$serverList.status === "error"} role="status">
    <span>
      {#if $serverList.status === "stale"}
        Lista desatualizada{$serverList.savedAt ? ` (de ${formatDate($serverList.savedAt)})` : ""}: {err?.title}
        Tentando de novo automaticamente.
      {:else}
        Não foi possível carregar a lista: {err?.title}
      {/if}
    </span>
    <button type="button" onclick={reload}>Atualizar lista</button>
  </div>
{/if}

<style>
  .status {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    flex-shrink: 0;
    padding: 8px 14px;
    border: 1px solid var(--warn-border);
    border-radius: 8px;
    background: var(--warn-bg);
    color: var(--warn);
    font-size: 13px;
  }
  .status.error {
    border-color: var(--danger);
    color: var(--danger);
  }
  button {
    padding: 4px 12px;
    border: 1px solid currentColor;
    border-radius: 6px;
    background: transparent;
    color: inherit;
    font: inherit;
    cursor: pointer;
    white-space: nowrap;
  }
</style>
