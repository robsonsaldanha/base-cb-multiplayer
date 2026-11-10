// Ping só do que está na tela: quando a lista chega/muda e a cada 30 s.
import { onMount, untrack } from "svelte";
import { PING_INTERVAL_MS, refreshPings } from "./servers";

/** Chame no `<script>` do componente. `ids` lê os servidores visíveis; `listKey` muda quando a lista recarrega. */
export function pingVisible(ids: () => readonly string[], listKey: () => unknown): void {
  $effect(() => {
    listKey();
    untrack(() => void refreshPings(ids()));
  });
  onMount(() => {
    const t = setInterval(() => void refreshPings(ids()), PING_INTERVAL_MS);
    return () => clearInterval(t);
  });
}
