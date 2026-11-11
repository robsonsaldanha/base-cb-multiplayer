<script lang="ts">
  // Barra fixa do rodapé: etapa do Jogar, progresso (bytes, velocidade, arquivo) e cancelar.
  import { formatBytes, formatSpeed } from "../lib/format";
  import { cancel, playState, STAGES } from "../lib/play";

  const run = $derived($playState.status === "running" ? $playState : null);
  const current = $derived(run?.stage ? STAGES.findIndex((s) => s.id === run.stage) : -1);
  const doing = $derived(current >= 0 ? STAGES[current]!.doing : "Preparando…");
</script>

<footer class="bar" aria-label="Barra de download">
  {#if run}
    <div class="line">
      <span class="what"><strong>{run.server.name}</strong> — <span>{doing}</span></span>
      <ol class="steps" aria-label="Etapas">
        {#each STAGES as s, i (s.id)}
          <li class:done={i < current} class:now={i === current} aria-current={i === current ? "step" : undefined}>
            {s.label}
          </li>
        {/each}
      </ol>
      <button type="button" class="cancel" disabled={run.cancelling} onclick={cancel} aria-label="Cancelar download"
        >{run.cancelling ? "Cancelando…" : "Cancelar"}</button
      >
    </div>
    {#if run.progress}
      <div class="line">
        <progress
          aria-label="Progresso do download"
          max={Math.max(run.progress.totalBytes, 1)}
          value={run.progress.doneBytes}
        ></progress>
        <span class="num"
          >{formatBytes(run.progress.doneBytes)} de {formatBytes(run.progress.totalBytes)} · {formatSpeed(
            run.progress.bytesPerSec,
          )}</span
        >
        <span class="file">{run.progress.current}</span>
      </div>
    {/if}
  {:else}
    <div class="line">
      <span class="idle">Nenhum download em andamento</span>
      <progress aria-hidden="true" max="1" value="0"></progress>
    </div>
  {/if}
</footer>

<style>
  .bar {
    grid-column: 1 / -1;
    display: flex;
    flex-direction: column;
    justify-content: center;
    gap: 6px;
    min-height: 52px;
    padding: 8px 20px;
    background: var(--panel);
    border-top: 1px solid var(--border);
    font-size: 13px;
  }
  .line {
    display: flex;
    align-items: center;
    gap: 14px;
    min-width: 0;
  }
  .what,
  .idle {
    white-space: nowrap;
  }
  .idle,
  .num,
  .file {
    color: var(--muted);
  }
  .num {
    white-space: nowrap;
    font-variant-numeric: tabular-nums;
  }
  .file {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .steps {
    display: flex;
    gap: 4px;
    flex: 1;
    margin: 0;
    padding: 0;
    list-style: none;
    font-size: 11px;
    color: var(--muted);
  }
  .steps li {
    padding: 2px 8px;
    border-radius: 10px;
    background: var(--chip);
  }
  .steps li.done {
    color: var(--text);
  }
  .steps li.now {
    background: var(--accent-soft);
    color: var(--accent);
    font-weight: 700;
  }
  progress {
    flex: 1;
    height: 6px;
    border: 0;
    border-radius: 3px;
    background: var(--chip);
    appearance: none;
    overflow: hidden;
  }
  progress::-webkit-progress-bar {
    background: var(--chip);
  }
  progress::-webkit-progress-value {
    background: var(--accent);
    border-radius: 3px;
  }
  progress::-moz-progress-bar {
    background: var(--accent);
  }
  .cancel {
    margin-left: auto;
    padding: 4px 12px;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: transparent;
    color: var(--text);
    font: inherit;
    cursor: pointer;
  }
  .cancel:hover:not(:disabled) {
    background: var(--panel-hover);
  }
</style>
