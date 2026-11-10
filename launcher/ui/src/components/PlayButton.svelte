<script lang="ts">
  import type { ServerListing } from "../lib/api";
  import { ERROR_TEXT } from "../lib/errors";
  import { navKey } from "../lib/keynav";
  import { blocked, blockReason, playState, startPlay } from "../lib/play";

  let { server, big = false, nav }: { server: ServerListing; big?: boolean; nav?: string } = $props();

  const reason = $derived(blockReason($blocked, server.id));
  const running = $derived($playState.status === "running");
  const mine = $derived($playState.status === "running" && $playState.server.id === server.id);
  const hint = $derived(reason ? (ERROR_TEXT[reason] ?? undefined) : running ? "Aguarde a entrada em andamento." : undefined);
</script>

<button
  type="button"
  class="play"
  class:big
  data-nav={nav}
  aria-label={`Jogar em ${server.name}`}
  title={hint}
  disabled={running || !!reason}
  onclick={() => startPlay({ id: server.id, name: server.name })}
  onkeydown={nav ? navKey : undefined}>{mine ? "Entrando…" : "Jogar"}</button
>

<style>
  .play {
    padding: 6px 16px;
    border: 0;
    border-radius: 6px;
    background: var(--accent);
    color: var(--on-accent);
    font: inherit;
    font-weight: 700;
    cursor: pointer;
    white-space: nowrap;
  }
  .play:hover:not(:disabled) {
    background: var(--accent-strong);
  }
  .play:disabled {
    opacity: 0.45;
    cursor: not-allowed;
  }
  .big {
    padding: 9px 26px;
    font-size: 15px;
  }
</style>
