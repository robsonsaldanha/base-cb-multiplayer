<script lang="ts">
  import { VIEWS, type View } from "../lib/views";

  let { current, version, onselect }: { current: View; version: string; onselect: (v: View) => void } = $props();
</script>

<aside class="sidebar">
  <div class="brand">CB <span>Multiplayer</span></div>
  <nav aria-label="Menu principal">
    {#each VIEWS as v (v.id)}
      <button
        type="button"
        class:active={current === v.id}
        aria-current={current === v.id ? "page" : undefined}
        onclick={() => onselect(v.id)}>{v.label}</button
      >
    {/each}
  </nav>
  <div class="version">{version ? `v${version}` : ""}</div>
</aside>

<style>
  .sidebar {
    display: flex;
    flex-direction: column;
    gap: 24px;
    padding: 20px 14px;
    background: var(--panel);
    border-right: 1px solid var(--border);
  }
  .brand {
    font-size: 18px;
    font-weight: 700;
    color: var(--accent);
    letter-spacing: 0.04em;
  }
  .brand span {
    color: var(--text);
    font-weight: 500;
  }
  nav {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  button {
    text-align: left;
    padding: 9px 12px;
    border: 0;
    border-radius: 6px;
    background: transparent;
    color: var(--muted);
    font: inherit;
    cursor: pointer;
  }
  button:hover {
    background: var(--panel-hover);
    color: var(--text);
  }
  button.active {
    background: var(--accent-soft);
    color: var(--accent);
  }
  .version {
    margin-top: auto;
    font-size: 12px;
    color: var(--muted);
  }
</style>
