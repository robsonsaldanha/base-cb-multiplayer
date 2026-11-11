<script lang="ts">
  import { describeError } from "../lib/errors";
  import { notice } from "../lib/notice";

  const err = $derived($notice ? describeError($notice) : null);
</script>

{#if err}
  <div class="notice">
    <span role="alert">
      <strong>{err.title}</strong>
      {#if err.detail}<span class="detail">{err.detail}</span>{/if}
    </span>
    <button type="button" aria-label="Fechar aviso" onclick={() => notice.set(null)}>×</button>
  </div>
{/if}

<style>
  .notice {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    flex-shrink: 0;
    padding: 8px 14px;
    border: 1px solid var(--danger);
    border-radius: 8px;
    background: var(--panel);
    font-size: 13px;
  }
  strong {
    color: var(--danger);
  }
  .detail {
    display: block;
    color: var(--muted);
    overflow-wrap: anywhere;
  }
  button {
    border: 0;
    background: transparent;
    color: var(--muted);
    font-size: 18px;
    cursor: pointer;
  }
</style>
