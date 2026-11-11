<script lang="ts">
  // Sugestão da pasta do jogo achada pela Epic, mas que outra conta do PC poderia alterar: o Rust nunca a usa
  // sozinho. "Usar esta pasta" pede ao Rust, que procura de novo, só aceita esta mesma pasta e confirma no diálogo
  // do Windows antes de gravar (a UI não escolhe caminho nenhum).
  import { onMount } from "svelte";
  import { detectGame, useDetectedGame, type GameCandidate } from "../lib/api";
  import { notify } from "../lib/notice";
  import { settings } from "../lib/settings";

  let candidate = $state<GameCandidate | null>(null);
  let busy = $state(false);

  const STORE_NAME: Record<GameCandidate["store"], string> = { epic: "Epic Games", steam: "Steam", manual: "" };

  onMount(() => {
    detectGame()
      .then((r) => {
        settings.set(r.settings);
        candidate = r.candidate;
      })
      // Sem sugestão; o seletor de pasta continua disponível.
      .catch(() => (candidate = null));
  });

  async function use() {
    if (!candidate) return;
    busy = true;
    try {
      const s = await useDetectedGame(candidate.dir);
      settings.set(s);
      if (s.gameDir) candidate = null;
    } catch (e) {
      candidate = null;
      notify(e);
    } finally {
      busy = false;
    }
  }
</script>

{#if candidate && !$settings?.gameDir}
  {@const store = STORE_NAME[candidate.store]}
  <div class="suggest" role="group" aria-label="Pasta do jogo encontrada">
    <p>
      Encontramos o Hogwarts Legacy em <span class="path">{candidate.dir}</span>{store ? ` (${store})` : ""}. Usar
      esta pasta?
    </p>
    <p class="hint">
      Outras contas deste computador podem alterar esta pasta, por isso o launcher pede sua confirmação. Confira se é a
      sua instalação.
    </p>
    <button type="button" class="primary" disabled={busy} onclick={use}>Usar esta pasta</button>
  </div>
{/if}

<style>
  .suggest {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 6px;
    padding: 10px 14px;
    border: 1px solid var(--accent);
    border-radius: 8px;
    background: var(--panel);
    font-size: 13px;
  }
  p {
    margin: 0;
  }
  .path {
    font-family: Consolas, "Cascadia Mono", monospace;
    font-size: 12px;
    overflow-wrap: anywhere;
    user-select: text;
  }
  .hint {
    color: var(--muted);
  }
  button {
    padding: 4px 12px;
    border: 0;
    border-radius: 6px;
    font: inherit;
    cursor: pointer;
  }
  .primary {
    background: var(--accent);
    color: var(--on-accent);
    font-weight: 700;
  }
  .primary:hover:not(:disabled) {
    background: var(--accent-strong);
  }
  button:disabled {
    opacity: 0.5;
    cursor: default;
  }
</style>
