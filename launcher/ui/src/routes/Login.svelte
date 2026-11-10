<script lang="ts">
  import { auth, cancelLogin, startLogin } from "../lib/stores";

  const waiting = $derived($auth.status === "waiting");
  const error = $derived($auth.status === "anonymous" ? $auth.error : null);
</script>

<section class="login" aria-label="Entrar">
  <div class="card">
    <div class="brand">CB <span>Multiplayer</span></div>
    {#if waiting}
      <p class="status" role="status">Aguardando o navegador… Conclua o login do Discord e volte para cá.</p>
      <button type="button" class="secondary" onclick={cancelLogin}>Cancelar</button>
    {:else}
      <p class="hint">Entre com sua conta do Discord para ver os servidores e jogar.</p>
      <button type="button" class="primary" onclick={startLogin}>Entrar com Discord</button>
    {/if}
    {#if error}
      <p class="error" role="alert">{error.message}</p>
    {/if}
  </div>
</section>

<style>
  .login {
    display: grid;
    place-items: center;
    height: 100vh;
  }
  .card {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 16px;
    width: 360px;
    padding: 32px 28px;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 10px;
    text-align: center;
  }
  .brand {
    font-size: 22px;
    font-weight: 700;
    color: var(--accent);
    letter-spacing: 0.04em;
  }
  .brand span {
    color: var(--text);
    font-weight: 500;
  }
  .hint,
  .status {
    margin: 0;
    color: var(--muted);
  }
  button {
    padding: 10px 18px;
    border-radius: 6px;
    font: inherit;
    cursor: pointer;
  }
  .primary {
    border: 0;
    background: var(--accent);
    color: var(--bg);
    font-weight: 600;
  }
  .secondary {
    border: 1px solid var(--border);
    background: transparent;
    color: var(--text);
  }
  .error {
    margin: 0;
    color: #e07a7a;
  }
</style>
