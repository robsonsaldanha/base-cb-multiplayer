<script lang="ts">
</script>

<!-- Barra fixa de download (progresso, velocidade, cancelar). Estados reais na Task 8. -->
<footer class="bar" aria-label="Barra de download">
  <span class="status">Nenhum download em andamento</span>
  <div class="track" aria-hidden="true"><div class="fill"></div></div>
</footer>

<style>
  .bar {
    grid-column: 1 / -1;
    display: flex;
    align-items: center;
    gap: 16px;
    padding: 0 20px;
    height: 48px;
    background: var(--panel);
    border-top: 1px solid var(--border);
  }
  .status {
    font-size: 13px;
    color: var(--muted);
    white-space: nowrap;
  }
  .track {
    flex: 1;
    height: 6px;
    border-radius: 3px;
    background: var(--bg);
    overflow: hidden;
  }
  .fill {
    width: 0;
    height: 100%;
    background: var(--accent);
  }
</style>
