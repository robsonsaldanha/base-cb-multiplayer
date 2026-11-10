<script lang="ts">
  // Destaque: todos os servidores `featured` do Hub, no topo, cada um com Jogar. Nada fixo no código.
  import type { ServerListing } from "../lib/api";
  import { formatPing } from "../lib/format";
  import { pings } from "../lib/servers";
  import Chips from "./Chips.svelte";
  import PlayButton from "./PlayButton.svelte";

  let { servers, loading = false }: { servers: readonly ServerListing[]; loading?: boolean } = $props();
</script>

<section class="featured" aria-label="Destaque">
  {#each servers as s (s.id)}
    <article class="hero" class:single={servers.length === 1}>
      <div class="tag">★ Destaque</div>
      <h2 class="name">{s.name}</h2>
      <p class="desc">{s.description}</p>
      <div class="meta">
        <span class="players" title="Jogadores">{s.players}/{s.maxPlayers}</span>
        <span class="ping" title="Ping">{formatPing($pings[s.id])}</span>
        <Chips tags={s.tags} locale={s.locale} />
      </div>
      <PlayButton server={s} big />
    </article>
  {:else}
    <article class="hero empty single">
      <h2>{loading ? "Carregando…" : "Nenhum servidor em destaque"}</h2>
      <p class="desc">Os servidores destacados pelo Hub aparecem aqui.</p>
    </article>
  {/each}
</section>

<style>
  .featured {
    display: flex;
    gap: 12px;
    overflow-x: auto;
    flex-shrink: 0;
    padding-bottom: 2px;
  }
  .hero {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 8px;
    flex: 0 0 360px;
    min-width: 0;
    padding: 18px 22px;
    border-radius: 10px;
    border: 1px solid var(--border);
    background: linear-gradient(120deg, var(--hero-a), var(--hero-b));
  }
  .hero.single {
    flex: 1 1 auto;
  }
  .empty {
    background: var(--panel);
  }
  .tag {
    padding: 2px 8px;
    border-radius: 4px;
    font-size: 11px;
    font-weight: 700;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--on-accent);
    background: var(--accent);
  }
  h2 {
    margin: 0;
    font-size: 22px;
    color: var(--title);
    max-width: 100%;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .desc {
    margin: 0;
    color: var(--muted);
    max-width: 100%;
    display: -webkit-box;
    -webkit-line-clamp: 2;
    line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
    overflow-wrap: anywhere;
  }
  .meta {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 10px;
    font-size: 13px;
  }
  .players {
    font-variant-numeric: tabular-nums;
  }
  .ping {
    color: var(--muted);
  }
</style>
