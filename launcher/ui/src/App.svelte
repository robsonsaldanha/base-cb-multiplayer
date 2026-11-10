<script lang="ts">
  import { onMount } from "svelte";
  import Sidebar from "./components/Sidebar.svelte";
  import DownloadBar from "./components/DownloadBar.svelte";
  import Profile from "./components/Profile.svelte";
  import Servers from "./routes/Servers.svelte";
  import Favorites from "./routes/Favorites.svelte";
  import Recent from "./routes/Recent.svelte";
  import Settings from "./routes/Settings.svelte";
  import Login from "./routes/Login.svelte";
  import { appVersion } from "./lib/api";
  import { auth, initAuth, refreshProfile, signOut } from "./lib/stores";
  import { VIEWS, type View } from "./lib/views";

  let view = $state<View>("servers");
  let version = $state("");
  const title = $derived(VIEWS.find((v) => v.id === view)?.label ?? "");

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
  <div class="shell">
    <Sidebar current={view} {version} onselect={(v) => (view = v)} />
    <main class="content">
      <header class="top">
        <h1>{title}</h1>
        <Profile profile={$auth.profile} onlogout={signOut} />
      </header>
      {#if view === "servers"}
        <Servers />
      {:else if view === "favorites"}
        <Favorites />
      {:else if view === "recent"}
        <Recent />
      {:else}
        <Settings />
      {/if}
    </main>
    <DownloadBar />
  </div>
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
  .shell {
    display: grid;
    grid-template-columns: 210px 1fr;
    grid-template-rows: 1fr auto;
    height: 100vh;
  }
  .content {
    display: flex;
    flex-direction: column;
    gap: 16px;
    min-height: 0;
    padding: 20px 24px;
    overflow: hidden;
  }
  .top {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
  }
  h1 {
    margin: 0;
    font-size: 20px;
    font-weight: 600;
  }
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
