<script lang="ts">
  // Pasta do jogo (só pelo seletor do Rust), limites, limpar cache, modo simular, versão e atualização.
  import { onMount } from "svelte";
  import { appVersion, checkUpdate, clearCache, installUpdate, pickGameDir, toApiError } from "../lib/api";
  import { describeError, type ErrorView } from "../lib/errors";
  import { settings, updateSettings } from "../lib/settings";
  import { updateInfo } from "../lib/update";

  let version = $state("");
  let cacheMB = $state(0);
  let contentMB = $state(0);
  let limitsMsg = $state("");
  let confirmClear = $state(false);
  let clearMsg = $state("");
  let busy = $state<"" | "pick" | "limits" | "clear" | "check" | "install">("");
  let updateMsg = $state("");
  let error = $state<ErrorView | null>(null);

  onMount(() => {
    appVersion()
      .then((v) => (version = v))
      .catch(() => (version = ""));
  });

  // Campos começam com o valor salvo (e voltam a ele quando o Rust devolve as configurações).
  $effect(() => {
    if ($settings) {
      cacheMB = $settings.cacheLimitMB;
      contentMB = $settings.maxServerContentMB;
    }
  });

  async function run<T>(what: typeof busy, f: () => Promise<T>): Promise<T | undefined> {
    busy = what;
    error = null;
    try {
      return await f();
    } catch (e) {
      error = describeError(toApiError(e));
      return undefined;
    } finally {
      busy = "";
    }
  }

  async function pick() {
    const s = await run("pick", pickGameDir);
    if (s) settings.set(s);
  }

  async function saveLimits() {
    limitsMsg = "";
    busy = "limits";
    const ok = await updateSettings({ cacheLimitMB: Number(cacheMB), maxServerContentMB: Number(contentMB) });
    busy = "";
    if (ok) limitsMsg = "Limites salvos.";
  }

  async function doClear() {
    clearMsg = "";
    confirmClear = false;
    await run("clear", clearCache);
    if (!error) clearMsg = "Cache limpo.";
  }

  async function check() {
    updateMsg = "";
    const s = await run("check", checkUpdate);
    if (!s) return;
    updateInfo.set(s.available ? s : null);
    if (!s.available) updateMsg = "Você já está na versão mais recente.";
  }

  async function install() {
    updateMsg = "Baixando e conferindo a atualização… o launcher será reiniciado.";
    await run("install", installUpdate);
    if (error) updateMsg = "";
  }
</script>

<div class="settings">
  {#if error}
    <div class="error" role="alert">
      <strong>{error.title}</strong>
      {#if error.detail}<span>{error.detail}</span>{/if}
      <button type="button" aria-label="Fechar aviso" onclick={() => (error = null)}>×</button>
    </div>
  {/if}
  <section aria-labelledby="s-game">
    <h2 id="s-game">Pasta do jogo</h2>
    <p class="path">{$settings?.gameDir ?? "Não encontrada"}</p>
    <p class="hint">O launcher procura o Hogwarts Legacy na Steam e na Epic. Se não achar, escolha a pasta.</p>
    <button type="button" disabled={busy === "pick"} onclick={pick}>Escolher pasta do jogo</button>
  </section>

  <section aria-labelledby="s-limits">
    <h2 id="s-limits">Limites</h2>
    <div class="fields">
      <label
        >Limite do cache (MB)
        <input type="number" min="256" max="1048576" step="256" bind:value={cacheMB} /></label
      >
      <label
        >Conteúdo máximo por servidor (MB)
        <input type="number" min="1" max="65536" bind:value={contentMB} /></label
      >
    </div>
    <button type="button" disabled={busy === "limits" || !$settings} onclick={saveLimits}>Salvar limites</button>
    {#if limitsMsg}<span class="ok" role="status">{limitsMsg}</span>{/if}
  </section>

  <section aria-labelledby="s-cache">
    <h2 id="s-cache">Cache</h2>
    <p class="hint">Apaga o conteúdo baixado de todos os servidores; ele é baixado de novo ao entrar.</p>
    {#if confirmClear}
      <p class="warn">
        Feche o jogo antes de limpar: arquivos em uso pelo Hogwarts Legacy não podem ser apagados.
      </p>
      <div class="row">
        <button type="button" class="danger" onclick={doClear}>Limpar agora</button>
        <button type="button" onclick={() => (confirmClear = false)}>Cancelar</button>
      </div>
    {:else}
      <button type="button" disabled={busy === "clear"} onclick={() => (confirmClear = true)}>Limpar cache</button>
    {/if}
    {#if clearMsg}<span class="ok" role="status">{clearMsg}</span>{/if}
  </section>

  <section aria-labelledby="s-sim">
    <h2 id="s-sim">Modo simular</h2>
    <label class="check">
      <input
        type="checkbox"
        checked={$settings?.simulateLaunch ?? true}
        disabled={!$settings}
        onchange={(e) => updateSettings({ simulateLaunch: e.currentTarget.checked })}
      />
      Modo simular (baixa e confere tudo, mas não abre o jogo)
    </label>
  </section>

  <section aria-labelledby="s-ver">
    <h2 id="s-ver">Versão</h2>
    <p>{version ? `v${version}` : ""}</p>
    <div class="row">
      <button type="button" disabled={busy === "check" || busy === "install"} onclick={check}
        >Procurar atualização</button
      >
      {#if $updateInfo?.available}
        <button type="button" class="primary" disabled={busy === "install"} onclick={install}
          >Instalar atualização</button
        >
      {/if}
    </div>
    {#if $updateInfo?.available}
      <p class="ok" role="status">Versão {$updateInfo.version} disponível.</p>
      {#if $updateInfo.notes}<p class="notes">{$updateInfo.notes}</p>{/if}
    {/if}
    {#if updateMsg}<p class="ok" role="status">{updateMsg}</p>{/if}
  </section>
</div>

<style>
  .settings {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(380px, 1fr));
    gap: 14px;
    overflow: auto;
    min-height: 0;
    align-content: start;
  }
  section {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 8px;
    padding: 16px 18px;
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--panel);
  }
  h2 {
    margin: 0;
    font-size: 15px;
    color: var(--title);
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
  .hint,
  .notes {
    color: var(--muted);
    font-size: 13px;
  }
  .notes {
    white-space: pre-line;
  }
  .error {
    grid-column: 1 / -1;
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 14px;
    border: 1px solid var(--danger);
    border-radius: 8px;
    font-size: 13px;
  }
  .error strong {
    color: var(--danger);
  }
  .error span {
    color: var(--muted);
    overflow-wrap: anywhere;
  }
  .error button {
    margin-left: auto;
    border: 0;
    font-size: 18px;
  }
  .warn {
    color: var(--warn);
    font-size: 13px;
  }
  .ok {
    color: var(--accent);
    font-size: 13px;
  }
  .fields {
    display: flex;
    flex-wrap: wrap;
    gap: 12px;
  }
  label {
    display: flex;
    flex-direction: column;
    gap: 4px;
    font-size: 13px;
    color: var(--muted);
  }
  .check {
    flex-direction: row;
    align-items: center;
    gap: 8px;
    color: var(--text);
    cursor: pointer;
  }
  input[type="number"] {
    width: 150px;
    padding: 6px 8px;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--bg);
    color: var(--text);
    font: inherit;
  }
  .row {
    display: flex;
    gap: 8px;
  }
  button {
    padding: 6px 14px;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: transparent;
    color: var(--text);
    font: inherit;
    cursor: pointer;
  }
  button:hover:not(:disabled) {
    background: var(--panel-hover);
  }
  button:disabled {
    opacity: 0.5;
    cursor: default;
  }
  .primary {
    border: 0;
    background: var(--accent);
    color: var(--on-accent);
    font-weight: 700;
  }
  .primary:hover:not(:disabled) {
    background: var(--accent-strong);
  }
  .danger {
    border-color: var(--danger);
    color: var(--danger);
  }
</style>
