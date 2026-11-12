<script lang="ts">
  // Pasta do jogo (só pelo seletor do Rust), mod do jogo, limites, limpar cache, avançado (modo simular), versão e
  // atualização.
  import { onMount } from "svelte";
  import {
    appVersion,
    checkUpdate,
    clearCache,
    forgetModFolder,
    installUpdate,
    modStatus,
    openModLogs,
    pickGameDir,
    removeMod,
    toApiError,
    type FolderRemoval,
    type ModInfo,
  } from "../lib/api";
  import { describeError, type ErrorView } from "../lib/errors";
  import GameSuggestion from "../components/GameSuggestion.svelte";
  import { settings, updateSettings } from "../lib/settings";
  import { updateInfo } from "../lib/update";

  let version = $state("");
  let cacheMB = $state(0);
  let contentMB = $state(0);
  let limitsMsg = $state("");
  let confirmClear = $state(false);
  let clearMsg = $state("");
  let busy = $state<"" | "pick" | "limits" | "clear" | "check" | "install" | "remove" | "logs" | "forget">("");
  let updateMsg = $state("");
  let error = $state<ErrorView | null>(null);
  let mod = $state<ModInfo | null>(null);
  let confirmRemove = $state(false);
  let removeMsg = $state("");
  let removeFailures = $state<FolderRemoval[]>([]);
  // Pasta que não existe mais à espera da confirmação de "Esquecer pasta".
  let confirmForget = $state<string | null>(null);

  const gone = $derived(new Set(mod?.goneFolders.map((g) => g.gameDir) ?? []));
  function goneBackups(dir: string): string[] {
    return mod?.goneFolders.find((g) => g.gameDir === dir)?.backups ?? [];
  }

  const modText = $derived(
    !mod
      ? ""
      : mod.state === "installed"
        ? `Instalado ${mod.version}`
        : mod.state === "conflict"
          ? "Conflito com outro carregador de mods"
          : "Não instalado",
  );
  const canRemove = $derived(!!mod && (mod.state !== "notInstalled" || mod.otherFolders.length > 0));

  function refreshMod() {
    modStatus()
      .then((m) => (mod = m))
      .catch(() => (mod = null));
  }

  onMount(() => {
    appVersion()
      .then((v) => (version = v))
      .catch(() => (version = ""));
    refreshMod();
  });

  // Campos começam com o valor salvo e só são trocados quando os limites salvos mudam (mudar outra configuração,
  // como o modo simular, não apaga o que está sendo digitado).
  const savedCache = $derived($settings?.cacheLimitMB);
  const savedContent = $derived($settings?.maxServerContentMB);
  $effect(() => {
    if (savedCache !== undefined) cacheMB = savedCache;
  });
  $effect(() => {
    if (savedContent !== undefined) contentMB = savedContent;
  });

  async function toggleSimulate(e: Event & { currentTarget: HTMLInputElement }) {
    const box = e.currentTarget;
    const ok = await updateSettings({ simulateLaunch: box.checked });
    // Gravação recusada: a caixa volta ao valor salvo (o store não mudou, então o Svelte não redesenha sozinho).
    if (!ok) box.checked = $settings?.simulateLaunch ?? false;
  }

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
    if (s) {
      settings.set(s);
      refreshMod();
    }
  }

  async function doRemove() {
    removeMsg = "";
    removeFailures = [];
    confirmRemove = false;
    const out = await run("remove", removeMod);
    if (!out) return;
    mod = out.status;
    removeFailures = out.folders.filter((f) => f.error);
    if (removeFailures.length === 0) removeMsg = "Mod removido do jogo.";
  }

  async function doForget(dir: string) {
    confirmForget = null;
    const out = await run("forget", () => forgetModFolder(dir));
    if (out) mod = out;
  }

  async function openLogs() {
    await run("logs", openModLogs);
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
    <GameSuggestion />
    <button type="button" disabled={busy === "pick"} onclick={pick}>Escolher pasta do jogo</button>
  </section>

  <section aria-labelledby="s-mod">
    <h2 id="s-mod">Mod do jogo</h2>
    {#if modText}<p>{modText}</p>{/if}
    {#if mod && mod.otherFolders.length > 0}
      <p class="warn">O mod também continua em pasta anterior do jogo:</p>
      <ul class="paths">
        {#each mod.otherFolders as dir (dir)}
          <li>
            <span class="path">{dir}</span>
            {#if gone.has(dir)}
              <span class="hint">Esta pasta não existe mais.</span>
              {#if confirmForget === dir}
                {#if goneBackups(dir).length > 0}
                  <p class="warn">
                    Os seus arquivos guardados pelo launcher ({goneBackups(dir).join(", ")}) estavam nesta pasta e o
                    launcher não consegue devolvê-los. Esquecer a pasta assim mesmo?
                  </p>
                {:else}
                  <p class="warn">O launcher não guardou arquivos seus nesta pasta. Esquecer a pasta?</p>
                {/if}
                <div class="row">
                  <button type="button" class="danger" disabled={busy === "forget"} onclick={() => doForget(dir)}
                    >Esquecer pasta</button
                  >
                  <button type="button" onclick={() => (confirmForget = null)}>Cancelar</button>
                </div>
              {:else}
                <button type="button" onclick={() => (confirmForget = dir)}>Esquecer pasta</button>
              {/if}
            {/if}
          </li>
        {/each}
      </ul>
    {/if}
    {#if confirmRemove}
      <p class="warn">
        Feche o jogo antes de remover. Os arquivos do CB Multiplayer saem da pasta do jogo e os seus, guardados em
        cbmp-backup, voltam para o lugar.
      </p>
      <div class="row">
        <button type="button" class="danger" onclick={doRemove}>Remover agora</button>
        <button type="button" onclick={() => (confirmRemove = false)}>Cancelar</button>
      </div>
    {:else}
      <div class="row">
        <button type="button" disabled={!canRemove || busy === "remove"} onclick={() => (confirmRemove = true)}
          >Remover mod do jogo</button
        >
        <button type="button" disabled={busy === "logs"} onclick={openLogs}>Abrir pasta de logs do mod</button>
      </div>
    {/if}
    {#if removeMsg}<span class="ok" role="status">{removeMsg}</span>{/if}
    {#if removeFailures.length > 0}
      <p class="warn">Não foi possível remover o mod de:</p>
      <ul class="paths" aria-label="Pastas onde o mod não saiu">
        {#each removeFailures as f (f.gameDir)}
          <li>
            <span class="path">{f.gameDir}</span>
            {#if f.error}<span class="hint">{describeError(f.error).detail ?? describeError(f.error).title}</span>{/if}
          </li>
        {/each}
      </ul>
    {/if}
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

  <section aria-labelledby="s-adv">
    <h2 id="s-adv">Avançado</h2>
    <label class="check">
      <input
        type="checkbox"
        checked={$settings?.simulateLaunch ?? false}
        disabled={!$settings}
        onchange={toggleSimulate}
      />
      Modo simular: prepara tudo e não abre o jogo
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
  .paths {
    margin: 0;
    padding-left: 18px;
    font-size: 12px;
  }
  .paths li {
    display: flex;
    flex-direction: column;
    gap: 2px;
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
