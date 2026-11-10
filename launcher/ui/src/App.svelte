<script lang="ts">
  import { onMount } from "svelte";
  import Sidebar from "./components/Sidebar.svelte";
  import DownloadBar from "./components/DownloadBar.svelte";
  import Servers from "./routes/Servers.svelte";
  import Favorites from "./routes/Favorites.svelte";
  import Recent from "./routes/Recent.svelte";
  import Settings from "./routes/Settings.svelte";
  import { appVersion } from "./lib/api";
  import { VIEWS, type View } from "./lib/views";

  let view = $state<View>("servers");
  let version = $state("");
  const title = $derived(VIEWS.find((v) => v.id === view)?.label ?? "");

  onMount(() => {
    appVersion()
      .then((v) => (version = v))
      .catch(() => (version = ""));
  });
</script>

<div class="shell">
  <Sidebar current={view} {version} onselect={(v) => (view = v)} />
  <main class="content">
    <h1>{title}</h1>
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
  h1 {
    margin: 0;
    font-size: 20px;
    font-weight: 600;
  }
</style>
