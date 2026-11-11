<script lang="ts">
  import { safeAvatar, type Profile } from "../lib/api";

  let { profile, onlogout }: { profile: Profile; onlogout: () => void } = $props();
  const avatar = $derived(safeAvatar(profile.avatar));
  const initial = $derived(profile.name.trim().charAt(0).toUpperCase() || "?");
</script>

<div class="profile" role="group" aria-label="Perfil">
  {#if avatar}
    <img src={avatar} alt={`Avatar de ${profile.name}`} width="28" height="28" />
  {:else}
    <span class="initial" aria-hidden="true">{initial}</span>
  {/if}
  <span class="name">{profile.name}</span>
  <button type="button" onclick={onlogout}>Sair</button>
</div>

<style>
  .profile {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  img,
  .initial {
    width: 28px;
    height: 28px;
    border-radius: 50%;
  }
  .initial {
    display: grid;
    place-items: center;
    background: var(--accent-soft);
    color: var(--accent);
    font-weight: 600;
  }
  .name {
    max-width: 180px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  button {
    padding: 5px 10px;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: transparent;
    color: var(--muted);
    font: inherit;
    cursor: pointer;
  }
  button:hover {
    color: var(--text);
    background: var(--panel-hover);
  }
</style>
