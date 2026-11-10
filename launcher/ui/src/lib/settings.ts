// Configurações locais (`settings.json` pelo Rust). Favoritos e recentes também moram aqui.
import { get, writable } from "svelte/store";
import { getSettings, setSettings, toUpdate, type Settings, type SettingsUpdate } from "./api";
import { notify } from "./notice";

export const settings = writable<Settings | null>(null);

export const MAX_RECENT = 20;

export async function loadSettings(): Promise<void> {
  try {
    settings.set(await getSettings());
  } catch (e) {
    notify(e);
  }
}

/** Grava a mudança; o estado só muda com a resposta do Rust (valor recusado não fica na tela). */
export async function updateSettings(patch: Partial<SettingsUpdate>): Promise<boolean> {
  const cur = get(settings);
  if (!cur) return false;
  try {
    settings.set(await setSettings({ ...toUpdate(cur), ...patch }));
    return true;
  } catch (e) {
    notify(e);
    return false;
  }
}

export function toggleFavorite(id: string): Promise<boolean> {
  const favs = get(settings)?.favorites ?? [];
  return updateSettings({ favorites: favs.includes(id) ? favs.filter((f) => f !== id) : [...favs, id] });
}

/** Servidor entra no topo dos recentes (sem repetir, no máx. 20). */
export function pushRecent(id: string): Promise<boolean> {
  const rec = get(settings)?.recent ?? [];
  return updateSettings({ recent: [id, ...rec.filter((r) => r !== id)].slice(0, MAX_RECENT) });
}
