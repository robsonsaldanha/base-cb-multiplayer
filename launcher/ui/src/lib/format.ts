const MB = 1024 * 1024;
const GB = 1024 * MB;

const num = (n: number, digits: number) =>
  n.toLocaleString("pt-BR", { minimumFractionDigits: digits, maximumFractionDigits: digits });

export function formatBytes(bytes: number): string {
  return bytes >= GB ? `${num(bytes / GB, 2)} GB` : `${num(bytes / MB, 1)} MB`;
}

export function formatSpeed(bytesPerSec: number): string {
  return `${num(bytesPerSec / MB, 1)} MB/s`;
}

export function formatPing(ms: number | null | undefined): string {
  if (ms === undefined) return "…";
  return ms === null ? "sem resposta" : `${ms} ms`;
}

export function formatDate(unixSecs: number): string {
  return new Date(unixSecs * 1000).toLocaleString("pt-BR", { dateStyle: "short", timeStyle: "short" });
}
