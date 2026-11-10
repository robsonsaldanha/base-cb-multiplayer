<script lang="ts">
</script>

<!-- Lista densa de servidores. Linhas reais (ServerRow, filtros, ping) na Task 8. -->
<section class="list" aria-label="Lista de servidores">
  <div class="head" aria-hidden="true">
    <span>Servidor</span><span>Jogadores</span><span>Ping</span>
  </div>
  <p class="empty">Nenhum servidor carregado.</p>
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
  .head {
    display: grid;
    grid-template-columns: 1fr 110px 70px;
    padding: 10px 16px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--muted);
    border-bottom: 1px solid var(--border);
  }
  .empty {
    margin: 0;
    padding: 24px 16px;
    color: var(--muted);
  }
</style>
