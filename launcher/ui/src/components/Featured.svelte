<script lang="ts">
</script>

<!-- Espaço do destaque (servidores `featured` do Hub). Conteúdo real na Task 8. -->
<section class="featured" aria-label="Destaque">
  <div class="tag">Destaque</div>
  <h2>Nenhum servidor em destaque</h2>
  <p>Os servidores destacados pelo Hub aparecem aqui.</p>
  <button type="button" disabled>Jogar</button>
</section>

<style>
  .featured {
    padding: 22px 24px;
    border-radius: 10px;
    border: 1px solid var(--border);
    background: linear-gradient(135deg, #2a2140 0%, var(--panel) 70%);
  }
  .tag {
    display: inline-block;
    padding: 2px 8px;
    border-radius: 4px;
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--bg);
    background: var(--accent);
  }
  h2 {
    margin: 12px 0 6px;
    font-size: 22px;
  }
  p {
    margin: 0 0 16px;
    color: var(--muted);
  }
  button {
    padding: 9px 22px;
    border: 0;
    border-radius: 6px;
    background: var(--accent);
    color: var(--bg);
    font: inherit;
    font-weight: 700;
  }
  button:disabled {
    opacity: 0.5;
  }
</style>
