import { BlockList, isIP } from 'node:net';
import { ServerInfo } from '@cbmp/contracts';

export type ProbeFn = (host: string, httpPort: number) => Promise<{ serverId: string } | null>;

export const MAX_PROBE_BODY = 16 * 1024;

export function normalizeIp(ip: string): string {
  return ip.startsWith('::ffff:') ? ip.slice(7) : ip;
}

const nonPublic = new BlockList();
for (const [net, bits] of [
  ['0.0.0.0', 8], ['10.0.0.0', 8], ['100.64.0.0', 10], ['127.0.0.0', 8], ['169.254.0.0', 16], ['172.16.0.0', 12],
  ['192.0.0.0', 24], ['192.0.2.0', 24], ['192.168.0.0', 16], ['198.18.0.0', 15], ['198.51.100.0', 24],
  ['203.0.113.0', 24], ['224.0.0.0', 4], ['240.0.0.0', 4],
] as const) nonPublic.addSubnet(net, bits, 'ipv4');
for (const [net, bits] of [['fc00::', 7], ['fe80::', 10], ['ff00::', 8], ['64:ff9b::', 96], ['2001:db8::', 32], ['::', 96], ['2002::', 16], ['2001::', 32], ['100::', 64], ['fec0::', 10]] as const) {
  nonPublic.addSubnet(net, bits, 'ipv6');
}
nonPublic.addAddress('::', 'ipv6');
nonPublic.addAddress('::1', 'ipv6');

/** true só para endereços roteáveis na internet pública (IPv4 mapeado em IPv6 é normalizado antes). */
export function isPublicIp(ip: string): boolean {
  const n = normalizeIp(ip);
  const fam = isIP(n);
  if (fam === 0) return false;
  return !nonPublic.check(n, fam === 6 ? 'ipv6' : 'ipv4');
}

async function readCapped(res: Response): Promise<string | null> {
  const len = res.headers.get('content-length');
  if (len !== null && Number(len) > MAX_PROBE_BODY) return null;
  if (!res.body) return null;
  const reader = res.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > MAX_PROBE_BODY) { await reader.cancel().catch(() => undefined); return null; }
    chunks.push(value);
  }
  return Buffer.concat(chunks).toString('utf8');
}

export const httpProbe: ProbeFn = async (host, httpPort) => {
  const h = host.includes(':') ? `[${host}]` : host;
  try {
    const res = await fetch(`http://${h}:${httpPort}/info`, { signal: AbortSignal.timeout(3000), redirect: 'error' });
    if (!res.ok) return null;
    if (!(res.headers.get('content-type') ?? '').includes('application/json')) return null;
    const text = await readCapped(res);
    if (text === null) return null;
    const parsed = ServerInfo.safeParse(JSON.parse(text));
    return parsed.success ? { serverId: parsed.data.serverId } : null;
  } catch {
    return null;
  }
};
