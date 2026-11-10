/** Setas/Home/End movem o foco entre os botões `[data-nav=<grupo>]` da mesma lista (`ul`/`section`). */
export function navKey(e: KeyboardEvent): void {
  const el = e.currentTarget as HTMLElement | null;
  const group = el?.dataset.nav;
  const root = el?.closest("ul, section");
  if (!el || !group || !root) return;
  const items = [...root.querySelectorAll<HTMLElement>(`[data-nav="${group}"]`)];
  const i = items.indexOf(el);
  const target =
    e.key === "ArrowDown" ? items[i + 1] :
    e.key === "ArrowUp" ? items[i - 1] :
    e.key === "Home" ? items[0] :
    e.key === "End" ? items.at(-1) :
    undefined;
  if (!["ArrowDown", "ArrowUp", "Home", "End"].includes(e.key)) return;
  e.preventDefault();
  target?.focus();
}
