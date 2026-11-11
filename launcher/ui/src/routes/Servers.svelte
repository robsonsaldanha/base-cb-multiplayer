<script lang="ts">
  import Featured from "../components/Featured.svelte";
  import Filters from "../components/Filters.svelte";
  import ListStatus from "../components/ListStatus.svelte";
  import ServerList from "../components/ServerList.svelte";
  import { applyFilters, EMPTY_FILTERS, type Filters as F } from "../lib/filters";
  import { serverList } from "../lib/servers";
  import { pingVisible } from "../lib/visiblePings.svelte";

  let filters = $state<F>({ ...EMPTY_FILTERS, tags: [] });
  const all = $derived($serverList.servers);
  const featured = $derived(all.filter((s) => s.featured));
  const shown = $derived(applyFilters(all, filters));
  const empty = $derived(
    $serverList.status === "loading"
      ? "Carregando servidores…"
      : all.length
        ? "Nenhum servidor com esses filtros."
        : "Nenhum servidor disponível.",
  );

  pingVisible(
    () => [...featured, ...shown].map((s) => s.id),
    () => $serverList.servers,
  );
</script>

<Featured servers={featured} loading={$serverList.status === "loading"} />
<Filters servers={all} bind:filters />
<ListStatus />
<ServerList servers={shown} {empty} />
