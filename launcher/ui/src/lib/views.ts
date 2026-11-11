export type View = "servers" | "favorites" | "recent" | "settings";

export const VIEWS: readonly { id: View; label: string }[] = [
  { id: "servers", label: "Servidores" },
  { id: "favorites", label: "Favoritos" },
  { id: "recent", label: "Recentes" },
  { id: "settings", label: "Configurações" },
];
