<script lang="ts">
  // Resultado do Jogar: "Pronto para abrir" (modo simular, conexão sem o ticket), jogo aberto, ou erro por código.
  // Conflito com outro carregador de mods: explica e oferece usar o do CB Multiplayer (diálogo nativo no Rust) e
  // repetir o Jogar. Sem permissão na pasta do jogo (`mod_folder_protected`): a mensagem do Rust já diz o que fazer.
  import { pickGameDir, resolveModConflict, toApiError } from "../lib/api";
  import GameSuggestion from "./GameSuggestion.svelte";
  import { describeError, MOD_CONFLICT_EXPLANATION } from "../lib/errors";
  import { dismissPlay, playState, startPlay } from "../lib/play";
  import { notify } from "../lib/notice";
  import { settings } from "../lib/settings";

  const err = $derived($playState.status === "failed" ? describeError($playState.error) : null);
  const needsGame = $derived(err?.code === "game_not_found" || err?.code === "game_invalid");
  const conflict = $derived(err?.code === "mod_conflict");
  let picking = $state(false);
  let resolving = $state(false);

  async function resolveConflict() {
    if ($playState.status !== "failed") return;
    const server = $playState.server;
    resolving = true;
    try {
      const st = await resolveModConflict();
      if (st.state === "installed") await startPlay(server);
    } catch (e) {
      playState.set({ status: "failed", server, error: toApiError(e) });
    } finally {
      resolving = false;
    }
  }

  async function pick() {
    picking = true;
    try {
      settings.set(await pickGameDir());
    } catch (e) {
      notify(e);
    } finally {
      picking = false;
    }
  }
</script>

{#if $playState.status === "done"}
  {#if $playState.outcome.kind === "simulated"}
    {@const p = $playState.outcome.preview}
    <section class="panel ok" aria-label="Pronto para abrir">
      <div class="head">
        <strong>Pronto para abrir — {$playState.server.name}</strong>
        <button type="button" onclick={dismissPlay}>Fechar</button>
      </div>
      <p>Modo simular: o conteúdo foi baixado e conferido, mas o jogo não foi aberto. Conexão preparada:</p>
      <dl>
        <dt>Endereço</dt><dd>{p.host}</dd>
        <dt>Porta</dt><dd>{p.port}</dd>
        <dt>Servidor</dt><dd>{p.serverId}</dd>
        <dt>Protocolo</dt><dd>{p.protocol}</dd>
        <dt>Pasta do conteúdo</dt><dd class="path">{p.contentDir}</dd>
      </dl>
    </section>
  {:else}
    <section class="panel ok" aria-label="Jogo aberto">
      <div class="head">
        <strong>{$playState.server.name}</strong>
        <button type="button" onclick={dismissPlay}>Fechar</button>
      </div>
      <p>Jogo aberto. Bom jogo!</p>
    </section>
  {/if}
{:else if err && $playState.status === "failed"}
  <section class="panel fail" aria-label="Erro ao entrar">
    <div class="row">
      <div class="head" role="alert">
        <span>
          <strong>{err.title}</strong>
          {#if err.detail}<span class="detail">{err.detail}</span>{/if}
        </span>
      </div>
      <div class="actions">
        {#if needsGame}
          <button type="button" class="primary" disabled={picking} onclick={pick}>Escolher pasta do jogo</button>
        {/if}
        {#if conflict}
          <button type="button" class="primary" disabled={resolving} onclick={resolveConflict}
            >Usar o carregador do CB Multiplayer</button
          >
        {/if}
        <button type="button" onclick={dismissPlay}>Fechar</button>
      </div>
    </div>
    {#if conflict}<p class="explain">{MOD_CONFLICT_EXPLANATION}</p>{/if}
  </section>
  {#if needsGame}
    <GameSuggestion />
  {/if}
{/if}

<style>
  .panel {
    flex-shrink: 0;
    padding: 12px 16px;
    border-radius: 8px;
    border: 1px solid var(--border);
    background: var(--panel);
    font-size: 13px;
  }
  .ok {
    border-color: var(--accent);
  }
  .fail {
    display: flex;
    flex-direction: column;
    gap: 6px;
    border-color: var(--danger);
  }
  .row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
  }
  .explain {
    margin: 0;
    overflow-wrap: anywhere;
  }
  .head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
  }
  .fail strong {
    color: var(--danger);
  }
  .detail {
    display: block;
    margin-top: 2px;
    color: var(--muted);
    overflow-wrap: anywhere;
  }
  p {
    margin: 6px 0;
    color: var(--muted);
  }
  dl {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: 2px 14px;
    margin: 0;
  }
  dt {
    color: var(--muted);
  }
  dd {
    margin: 0;
    user-select: text;
  }
  .path {
    overflow-wrap: anywhere;
  }
  .actions {
    display: flex;
    gap: 8px;
  }
  button {
    padding: 4px 12px;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: transparent;
    color: var(--text);
    font: inherit;
    cursor: pointer;
    white-space: nowrap;
  }
  .primary {
    border: 0;
    background: var(--accent);
    color: var(--on-accent);
    font-weight: 700;
  }
</style>
