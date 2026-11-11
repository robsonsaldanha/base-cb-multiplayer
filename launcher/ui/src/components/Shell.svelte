<script lang="ts">
  // Launcher logado (layout A): menu lateral, topo com perfil, tela ativa e barra de download fixa.
  import { onMount } from "svelte";
  import type { Profile as P } from "../lib/api";
  import { initPlayEvents } from "../lib/play";
  import { loadServers, stopServers } from "../lib/servers";
  import { loadSettings } from "../lib/settings";
  import { signOut } from "../lib/stores";
  import { initUpdateEvents, updateInfo } from "../lib/update";
  import { VIEWS, type View } from "../lib/views";
  import Favorites from "../routes/Favorites.svelte";
  import Recent from "../routes/Recent.svelte";
  import Servers from "../routes/Servers.svelte";
  import Settings from "../routes/Settings.svelte";
  import DownloadBar from "./DownloadBar.svelte";
  import Notice from "./Notice.svelte";
  import PlayStatus from "./PlayStatus.svelte";
  import Profile from "./Profile.svelte";
  import Sidebar from "./Sidebar.svelte";

  let { profile, version }: { profile: P; version: string } = $props();

  let view = $state<View>("servers");
  const title = $derived(VIEWS.find((v) => v.id === view)?.label ?? "");

  onMount(() => {
    const offs: (() => void)[] = [];
    let gone = false;
    const keep = (off: () => void) => (gone ? off() : offs.push(off));
    // Eventos antes do primeiro play; configurações e lista em paralelo.
    initPlayEvents().then(keep, () => {});
    initUpdateEvents().then(keep, () => {});
    void loadSettings();
    void loadServers();
    return () => {
      gone = true;
      stopServers();
      offs.forEach((off) => off());
    };
  });
</script>

<div class="shell">
  <Sidebar current={view} {version} update={$updateInfo} onselect={(v) => (view = v)} />
  <main class="content">
    <header class="top">
      <h1>{title}</h1>
      <Profile {profile} onlogout={signOut} />
    </header>
    <Notice />
    <PlayStatus />
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
    grid-template-columns: 210px minmax(0, 1fr);
    grid-template-rows: minmax(0, 1fr) auto;
    height: 100vh;
  }
  .content {
    display: flex;
    flex-direction: column;
    gap: 14px;
    min-height: 0;
    padding: 18px 24px;
    overflow: hidden;
  }
  .top {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
    flex-shrink: 0;
  }
  h1 {
    margin: 0;
    font-size: 20px;
    font-weight: 600;
  }
</style>
