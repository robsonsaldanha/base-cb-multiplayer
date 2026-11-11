<script lang="ts">
  // Sugestão de pasta do jogo que o Rust achou, mas não conseguiu verificar (outra conta do PC pode ter criado ou
  // alterado). O Rust nunca a usa sozinho e só devolve uma quando é a ÚNICA: com várias, nenhuma é sugerida e o
  // usuário usa o seletor. "Usar esta pasta" pede ao Rust, que procura de novo, só aceita esta mesma pasta e confirma
  // no diálogo do Windows antes de gravar (a UI não escolhe caminho nenhum). O texto não diz de onde a pasta veio.
  import { onMount } from "svelte";
  import { detectGame, useDetectedGame, type GameCandidate } from "../lib/api";
  import { notify } from "../lib/notice";
  import { settings } from "../lib/settings";

  let candidate = $state<GameCandidate | null>(null);
  let ambiguous = $state(false);
  let busy = $state(false);

  onMount(() => {
    detectGame()
      .then((r) => {
        settings.set(r.settings);
        candidate = r.candidate;
        ambiguous = r.ambiguous;
      })
      .catch((e: unknown) => {
        // Sem sugestão; o seletor de pasta continua disponível.
        console.warn("procura da pasta do jogo falhou", e);
        candidate = null;
      });
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

{#if candidate}
  <div class="suggest" role="group" aria-label="Pasta do jogo encontrada">
    <p>O launcher encontrou uma pasta que parece ser do Hogwarts Legacy:</p>
    <p class="path">{candidate.dir}</p>
    <p class="hint">
      O launcher não conseguiu verificar quem controla esta pasta: outra conta deste computador pode tê-la criado ou
      alterado. Use esta pasta só se foi nela que você instalou o jogo; se não tiver certeza, use "Escolher pasta do
      jogo".
    </p>
    <button type="button" class="primary" disabled={busy} onclick={use}>Usar esta pasta</button>
  </div>
{:else if ambiguous}
  <p class="ambiguous" role="status">
    Mais de uma pasta parece ser do Hogwarts Legacy e o launcher não consegue saber qual é a sua. Use "Escolher pasta
    do jogo".
  </p>
{/if}

<style>
  .suggest {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 6px;
    padding: 10px 14px;
    border: 1px solid var(--warn);
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
  .ambiguous {
    color: var(--warn);
    font-size: 13px;
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
