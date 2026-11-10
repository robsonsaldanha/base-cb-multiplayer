<script lang="ts">
  // Lista densa. Textos do servidor só como texto. Teclado: setas/Home/End entre os botões da mesma coluna.
  import type { ServerListing } from "../lib/api";
  import { formatPing } from "../lib/format";
  import { navKey } from "../lib/keynav";
  import { pings } from "../lib/servers";
  import { settings, toggleFavorite } from "../lib/settings";
  import Chips from "./Chips.svelte";
  import PlayButton from "./PlayButton.svelte";

  let {
    servers,
    label = "Lista de servidores",
    empty = "Nenhum servidor carregado.",
  }: { servers: readonly ServerListing[]; label?: string; empty?: string } = $props();

  const favorites = $derived(new Set($settings?.favorites ?? []));
</script>

<section class="list" aria-label={label}>
  <div class="head" aria-hidden="true">
    <span></span><span>Servidor</span><span>Tags</span><span>Jogadores</span><span>Ping</span><span></span>
  </div>
  {#if servers.length}
    <ul>
      {#each servers as s (s.id)}
        {@const fav = favorites.has(s.id)}
        <li class="row">
          <button
            type="button"
            class="star"
            class:on={fav}
            data-nav="fav"
            aria-pressed={fav}
            aria-label={fav ? `Remover ${s.name} dos favoritos` : `Favoritar ${s.name}`}
            onclick={() => toggleFavorite(s.id)}
            onkeydown={navKey}>{fav ? "★" : "☆"}</button
          >
          <div class="info">
            <span class="name">{s.name}</span>
            <span class="desc">{s.description}</span>
          </div>
          <Chips tags={s.tags} locale={s.locale} />
          <span class="players" class:full={s.players >= s.maxPlayers}>{s.players}/{s.maxPlayers}</span>
          <span class="ping">{formatPing($pings[s.id])}</span>
          <PlayButton server={s} nav="play" />
        </li>
      {/each}
    </ul>
  {:else}
    <p class="empty">{empty}</p>
  {/if}
</section>

<style>
  .list {
    flex: 1;
    min-height: 0;
    overflow: auto;
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--panel);
  }
  .head,
  .row {
    display: grid;
    grid-template-columns: 32px minmax(0, 1fr) minmax(0, 200px) 80px 96px 92px;
    align-items: center;
    gap: 10px;
    padding: 8px 14px;
  }
  .head {
    position: sticky;
    top: 0;
    z-index: 1;
    padding-block: 9px;
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--muted);
    background: var(--panel);
    border-bottom: 1px solid var(--border);
  }
  ul {
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .row {
    border-bottom: 1px solid var(--row-border);
  }
  .row:hover,
  .row:focus-within {
    background: var(--panel-hover);
  }
  .info {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .name,
  .desc {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .name {
    font-weight: 600;
  }
  .desc {
    font-size: 12px;
    color: var(--muted);
  }
  .players,
  .ping {
    font-size: 13px;
    font-variant-numeric: tabular-nums;
    color: var(--muted);
  }
  .players.full {
    color: var(--danger);
  }
  .star {
    width: 28px;
    height: 28px;
    border: 0;
    border-radius: 6px;
    background: transparent;
    color: var(--muted);
    font-size: 16px;
    cursor: pointer;
  }
  .star:hover {
    background: var(--panel-hover);
    color: var(--text);
  }
  .star.on {
    color: var(--accent);
  }
  .empty {
    margin: 0;
    padding: 24px 16px;
    color: var(--muted);
  }
</style>
