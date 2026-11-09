import type { KeyObject } from 'node:crypto';
export interface KeyRing {
  active: { kid: string; privateKey: KeyObject };
  publicKeys(): { kid: string; publicKey: string }[];
}
