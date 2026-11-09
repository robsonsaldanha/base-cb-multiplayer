import { ServerInfo } from '@cbmp/contracts';

export type ProbeFn = (host: string, httpPort: number) => Promise<{ serverId: string } | null>;

export function normalizeIp(ip: string): string {
  return ip.startsWith('::ffff:') ? ip.slice(7) : ip;
}

export const httpProbe: ProbeFn = async (host, httpPort) => {
  const h = host.includes(':') ? `[${host}]` : host;
  try {
    const res = await fetch(`http://${h}:${httpPort}/info`, { signal: AbortSignal.timeout(3000), redirect: 'error' });
    if (!res.ok) return null;
    const parsed = ServerInfo.safeParse(await res.json());
    return parsed.success ? { serverId: parsed.data.serverId } : null;
  } catch {
    return null;
  }
};
