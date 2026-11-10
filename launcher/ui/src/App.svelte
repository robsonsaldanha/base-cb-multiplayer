<script lang="ts">
  import { onMount } from "svelte";
  import Shell from "./components/Shell.svelte";
  import Login from "./routes/Login.svelte";
  import { appVersion } from "./lib/api";
  import { auth, initAuth, refreshProfile } from "./lib/stores";

  let version = $state("");

  onMount(() => {
    appVersion()
      .then((v) => (version = v))
      .catch(() => (version = ""));
    let stop: (() => void) | undefined;
    let gone = false;
    initAuth()
      .then((s) => (gone ? s() : (stop = s)))
      .catch(() => auth.set({ status: "anonymous", error: null }));
    return () => {
      gone = true;
      stop?.();
    };
  });
</script>

{#if $auth.status === "logged_in"}
  <Shell profile={$auth.profile} {version} />
{:else if $auth.status === "checking"}
  <p class="checking" role="status">Carregando…</p>
{:else if $auth.status === "unavailable"}
  <div class="checking">
    <p role="alert">{$auth.error.message}</p>
    <button type="button" onclick={refreshProfile}>Tentar de novo</button>
  </div>
{:else}
  <Login />
{/if}

<style>
  .checking {
    display: grid;
    place-content: center;
    justify-items: center;
    gap: 12px;
    height: 100vh;
    margin: 0;
    color: var(--muted);
  }
  .checking p {
    margin: 0;
  }
  .checking button {
    padding: 8px 16px;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: transparent;
    color: var(--text);
    font: inherit;
    cursor: pointer;
  }
</style>
