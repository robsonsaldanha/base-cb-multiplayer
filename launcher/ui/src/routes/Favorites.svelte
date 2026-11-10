<script lang="ts">
  import ListStatus from "../components/ListStatus.svelte";
  import ServerList from "../components/ServerList.svelte";
  import { serverList } from "../lib/servers";
  import { settings } from "../lib/settings";
  import { pingVisible } from "../lib/visiblePings.svelte";

  const shown = $derived(
    ($settings?.favorites ?? []).flatMap((id) => $serverList.servers.filter((s) => s.id === id)),
  );

  pingVisible(
    () => shown.map((s) => s.id),
    () => $serverList.servers,
  );
</script>

<ListStatus />
<ServerList
  servers={shown}
  label="Servidores favoritos"
  empty="Nenhum favorito na lista. Use ☆ ao lado de um servidor para favoritar."
/>
