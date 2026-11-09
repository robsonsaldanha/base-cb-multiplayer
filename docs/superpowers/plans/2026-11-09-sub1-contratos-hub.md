# Subprojeto 1 — Contratos + Hub — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pacote `@cbmp/contracts` (tickets Ed25519, schemas, vetores de teste) e o Hub (Node/TS/Fastify/SQLite) com login Discord, tickets, publicação por chave + heartbeat verificado, lista pública, bans globais e painel web de donos.

**Architecture:** Monorepo npm workspaces na raiz do repo. `packages/contracts` é a fonte única dos formatos (usada pelo Hub agora e pelo launcher/servidor C++ depois via vetores JSON). O Hub é montado por `buildApp(deps)` com dependências injetáveis (store SQLite, keyring, cliente Discord, probe HTTP, relógio) para testes sem rede. Painel web é HTML renderizado no servidor, sem framework de frontend.

**Tech Stack:** Node ≥ 22 (máquina dev: Node 24), TypeScript (sem build; execução via `tsx`), Fastify 5, @fastify/cookie, @fastify/formbody, @fastify/rate-limit, better-sqlite3, zod 3, Vitest. Criptografia: `node:crypto` (Ed25519 nativo).

## Global Constraints

- Repo: `robsonsaldanha/base-cb-multiplayer` (privado). Trabalhar no branch `sub1-hub`; commit ao fim de cada task; merge em `main` só com OK do Robson. Nunca tocar no repo `castelobruxo/castelobruxo`.
- Ticket: `base64url(payloadJSON) + "." + base64url(assinatura)`; Ed25519 sobre os bytes do payload. Payload `{ "v": 1, "kid", "sub", "did", "name", "aud", "iat", "exp", "jti" }`. `exp - iat = 300`. Tolerância de relógio ±60 s.
- Chave pública publicada como Ed25519 **raw 32 bytes em base64url** (`GET /v1/keys/public` → `[{kid, publicKey}]`).
- Chave do servidor: 32 bytes aleatórios, prefixo `sk_`, mostrada uma única vez; Hub guarda só `sha256`.
- Heartbeat a cada 30 s; sem heartbeat por 90 s → sai da lista. IP vem da conexão, nunca do corpo. Probe `http://IP:httpPort/info` deve devolver o mesmo `serverId` antes de listar; repetir a cada 10 min.
- `GET /v1/servers` público com cache de 5 s.
- Erros: `{ "error": "<codigo_estavel>", "message": "<texto pt-BR>" }`.
- Banco interno do Hub: SQLite. Chave privada de assinatura só no disco do Hub (`<dataDir>/keys/*.pem`, modo 600), com `kid` para rotação.
- Login Discord, escopo `identify` apenas. Launcher recebe `cbmp://auth?code=<código de uso único, 60 s>` e troca por tokens em `POST /v1/auth/exchange` (tokens nunca na URL).
- Textos ao usuário em pt-BR. Nome "Hogwarts" nunca no produto.

## Estrutura de arquivos

```
package.json                     workspaces + scripts raiz
tsconfig.base.json
packages/contracts/
  package.json  tsconfig.json
  src/index.ts                   reexporta tudo
  src/ticket.ts                  sign/verify/chaves
  src/schemas.ts                 zod: heartbeat, listing, info, manifest, erros, tickets
  src/vectors.ts                 buildVectors() determinístico
  scripts/gen-vectors.ts         grava test-vectors/tickets.json
  test-vectors/tickets.json      gerado (commitado)
  test/ticket.test.ts  test/schemas.test.ts  test/vectors.test.ts
hub/
  package.json  tsconfig.json  .env.example  README.md
  src/config.ts                  env → HubConfig
  src/db.ts                      openDb + migrações
  src/store.ts                   acesso a dados (users, sessions, servers, status, bans)
  src/errors.ts                  HttpError
  src/keyring.ts                 chaves de assinatura
  src/discord.ts                 cliente OAuth Discord
  src/probe.ts                   probe HTTP /info
  src/auth.ts                    sessão atual / requireUser / admin
  src/app.ts                     buildApp(deps)
  src/routes/auth.ts             OAuth, dev-login, exchange, refresh, logout, me
  src/routes/tickets.ts          tickets + chaves públicas
  src/routes/owner.ts            CRUD de servidores do dono
  src/routes/servers.ts          heartbeat + lista pública
  src/routes/bans.ts             bans globais
  src/routes/panel.ts            painel HTML
  src/main.ts                    processo real
  test/helpers.ts  test/*.test.ts
```

---

### Task 1: Workspace + `@cbmp/contracts`

**Files:**
- Create: `package.json`, `tsconfig.base.json`, `packages/contracts/package.json`, `packages/contracts/tsconfig.json`, `packages/contracts/src/{index,ticket,schemas,vectors}.ts`, `packages/contracts/scripts/gen-vectors.ts`, `packages/contracts/test-vectors/tickets.json` (gerado), `packages/contracts/test/{ticket,schemas,vectors}.test.ts`
- Modify: `.gitignore` (adicionar `node_modules/`)

**Interfaces:**
- Produces (import de `@cbmp/contracts`):
  - `TICKET_TTL_SECONDS = 300`, `CLOCK_SKEW_SECONDS = 60`, `PROTOCOL_VERSION = 1`
  - `interface TicketPayload { v: 1; kid: string; sub: string; did: string; name: string; aud: string; iat: number; exp: number; jti: string }`
  - `type TicketError = 'malformed' | 'bad_version' | 'unknown_kid' | 'bad_signature' | 'wrong_audience' | 'expired' | 'not_yet_valid'`
  - `signTicket(payload: TicketPayload, privateKey: KeyObject): string`
  - `verifyTicket(ticket: string, opts: { publicKeys: ReadonlyMap<string, KeyObject>; audience: string; nowSeconds: number }): { ok: true; payload: TicketPayload } | { ok: false; error: TicketError }`
  - `privateKeyFromSeed(seed: Buffer): KeyObject`, `generatePrivateKey(): KeyObject`, `publicKeyToRaw(key: KeyObject): string`, `publicKeyFromRaw(raw: string): KeyObject`
  - zod schemas + tipos: `HeartbeatBody`, `ServerListing`, `ServerInfo`, `ManifestFile`, `ResourceManifest`, `ErrorBody`, `TicketRequest`, `TicketResponse`, `PublicKeysResponse`, `Tag`
  - `buildVectors(): TicketVectors`

- [ ] **Step 1: Branch e workspace**

```bash
git checkout -b sub1-hub
```

`package.json` (raiz):
```json
{
  "name": "base-cb-multiplayer",
  "private": true,
  "type": "module",
  "workspaces": ["packages/*", "hub"],
  "engines": { "node": ">=22" },
  "scripts": {
    "test": "npm test --workspaces --if-present",
    "typecheck": "npm run typecheck --workspaces --if-present"
  }
}
```

`tsconfig.base.json`:
```json
{
  "compilerOptions": {
    "target": "ES2023",
    "module": "ESNext",
    "moduleResolution": "Bundler",
    "strict": true,
    "noEmit": true,
    "skipLibCheck": true,
    "esModuleInterop": true,
    "resolveJsonModule": true,
    "types": ["node"]
  }
}
```

`packages/contracts/package.json`:
```json
{
  "name": "@cbmp/contracts",
  "version": "0.1.0",
  "private": true,
  "type": "module",
  "exports": { ".": "./src/index.ts" },
  "scripts": {
    "test": "vitest run",
    "typecheck": "tsc -p tsconfig.json",
    "vectors": "tsx scripts/gen-vectors.ts"
  }
}
```

`packages/contracts/tsconfig.json`:
```json
{ "extends": "../../tsconfig.base.json", "include": ["src", "scripts", "test"] }
```

Adicionar `node_modules/` ao `.gitignore`. Instalar:
```bash
npm install -w @cbmp/contracts zod@3
npm install -D -w @cbmp/contracts typescript vitest tsx @types/node
```

- [ ] **Step 2: Testes de ticket (falhando)**

`packages/contracts/test/ticket.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { createPublicKey } from 'node:crypto';
import {
  CLOCK_SKEW_SECONDS, TICKET_TTL_SECONDS, privateKeyFromSeed, publicKeyFromRaw, publicKeyToRaw,
  signTicket, verifyTicket, type TicketPayload,
} from '../src/index';

const seed = Buffer.alloc(32, 7);
const priv = privateKeyFromSeed(seed);
const pubRaw = publicKeyToRaw(createPublicKey(priv));
const keys = new Map([['k1', publicKeyFromRaw(pubRaw)]]);
const base: TicketPayload = {
  v: 1, kid: 'k1', sub: 'u_1', did: '123456789012345678', name: 'Robson', aud: 'srv_abc',
  iat: 1_800_000_000, exp: 1_800_000_000 + TICKET_TTL_SECONDS, jti: '00000000-0000-4000-8000-000000000001',
};

describe('ticket', () => {
  it('chave pública raw tem 32 bytes', () => {
    expect(Buffer.from(pubRaw, 'base64url').length).toBe(32);
  });
  it('assina e valida', () => {
    const t = signTicket(base, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat + 1 }))
      .toEqual({ ok: true, payload: base });
  });
  it('assinatura é determinística', () => {
    expect(signTicket(base, priv)).toBe(signTicket(base, priv));
  });
  it('rejeita audience errada', () => {
    const t = signTicket(base, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_outro', nowSeconds: base.iat })).toEqual({ ok: false, error: 'wrong_audience' });
  });
  it('rejeita expirado além da tolerância e aceita dentro', () => {
    const t = signTicket(base, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.exp + CLOCK_SKEW_SECONDS }).ok).toBe(true);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.exp + CLOCK_SKEW_SECONDS + 1 })).toEqual({ ok: false, error: 'expired' });
  });
  it('rejeita emitido no futuro além da tolerância', () => {
    const t = signTicket(base, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat - CLOCK_SKEW_SECONDS - 1 })).toEqual({ ok: false, error: 'not_yet_valid' });
  });
  it('rejeita kid desconhecido', () => {
    const t = signTicket({ ...base, kid: 'k2' }, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat })).toEqual({ ok: false, error: 'unknown_kid' });
  });
  it('rejeita payload adulterado', () => {
    const [, sig] = signTicket(base, priv).split('.');
    const forged = Buffer.from(JSON.stringify({ ...base, name: 'Outro' })).toString('base64url') + '.' + sig;
    expect(verifyTicket(forged, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat })).toEqual({ ok: false, error: 'bad_signature' });
  });
  it('rejeita versão diferente', () => {
    const t = signTicket({ ...base, v: 2 as 1 }, priv);
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat })).toEqual({ ok: false, error: 'bad_version' });
  });
  it.each(['', 'abc', 'a.b.c', '!!.??', `${Buffer.from('não é json').toString('base64url')}.AAAA`])('rejeita malformado %j', (t) => {
    expect(verifyTicket(t, { publicKeys: keys, audience: 'srv_abc', nowSeconds: base.iat })).toEqual({ ok: false, error: 'malformed' });
  });
});
```

- [ ] **Step 3: Rodar e ver falhar**

Run: `npm test -w @cbmp/contracts`
Expected: FAIL — `Failed to resolve import "../src/index"`.

- [ ] **Step 4: Implementar `ticket.ts`**

`packages/contracts/src/ticket.ts`:
```ts
import {
  createPrivateKey, createPublicKey, generateKeyPairSync, sign, verify, type KeyObject,
} from 'node:crypto';

export const TICKET_TTL_SECONDS = 300;
export const CLOCK_SKEW_SECONDS = 60;

export interface TicketPayload {
  v: 1;
  kid: string;
  sub: string;
  did: string;
  name: string;
  aud: string;
  iat: number;
  exp: number;
  jti: string;
}

export type TicketError =
  | 'malformed' | 'bad_version' | 'unknown_kid' | 'bad_signature'
  | 'wrong_audience' | 'expired' | 'not_yet_valid';

export type VerifyResult = { ok: true; payload: TicketPayload } | { ok: false; error: TicketError };

// Prefixo DER PKCS#8 de uma chave privada Ed25519; seguido dos 32 bytes da seed.
const PKCS8_ED25519_PREFIX = Buffer.from('302e020100300506032b657004220420', 'hex');
const B64URL = /^[A-Za-z0-9_-]+$/;

export function privateKeyFromSeed(seed: Buffer): KeyObject {
  if (seed.length !== 32) throw new Error('seed Ed25519 precisa ter 32 bytes');
  return createPrivateKey({ key: Buffer.concat([PKCS8_ED25519_PREFIX, seed]), format: 'der', type: 'pkcs8' });
}

export function generatePrivateKey(): KeyObject {
  return generateKeyPairSync('ed25519').privateKey;
}

/** Chave pública Ed25519 como 32 bytes raw em base64url. Aceita chave pública ou privada. */
export function publicKeyToRaw(key: KeyObject): string {
  const pub = key.type === 'private' ? createPublicKey(key) : key;
  const jwk = pub.export({ format: 'jwk' });
  if (typeof jwk.x !== 'string') throw new Error('chave não é Ed25519');
  return jwk.x;
}

export function publicKeyFromRaw(raw: string): KeyObject {
  return createPublicKey({ key: { kty: 'OKP', crv: 'Ed25519', x: raw }, format: 'jwk' });
}

export function signTicket(payload: TicketPayload, privateKey: KeyObject): string {
  const body = Buffer.from(JSON.stringify(payload), 'utf8');
  return `${body.toString('base64url')}.${sign(null, body, privateKey).toString('base64url')}`;
}

function isPayloadShape(p: unknown): p is TicketPayload {
  if (typeof p !== 'object' || p === null) return false;
  const o = p as Record<string, unknown>;
  return typeof o.v === 'number'
    && ['kid', 'sub', 'did', 'name', 'aud', 'jti'].every((k) => typeof o[k] === 'string')
    && Number.isInteger(o.iat) && Number.isInteger(o.exp);
}

/**
 * Ordem das checagens (o validador C++ deve seguir a mesma):
 * formato → JSON/forma → versão → kid → assinatura → audience → iat futuro → expiração.
 */
export function verifyTicket(
  ticket: string,
  opts: { publicKeys: ReadonlyMap<string, KeyObject>; audience: string; nowSeconds: number },
): VerifyResult {
  const parts = ticket.split('.');
  if (parts.length !== 2 || !B64URL.test(parts[0]!) || !B64URL.test(parts[1]!)) return { ok: false, error: 'malformed' };
  const body = Buffer.from(parts[0]!, 'base64url');
  const sig = Buffer.from(parts[1]!, 'base64url');
  let payload: unknown;
  try {
    payload = JSON.parse(body.toString('utf8'));
  } catch {
    return { ok: false, error: 'malformed' };
  }
  if (!isPayloadShape(payload)) return { ok: false, error: 'malformed' };
  if (payload.v !== 1) return { ok: false, error: 'bad_version' };
  const key = opts.publicKeys.get(payload.kid);
  if (!key) return { ok: false, error: 'unknown_kid' };
  if (sig.length !== 64 || !verify(null, body, key, sig)) return { ok: false, error: 'bad_signature' };
  if (payload.aud !== opts.audience) return { ok: false, error: 'wrong_audience' };
  if (payload.iat > opts.nowSeconds + CLOCK_SKEW_SECONDS) return { ok: false, error: 'not_yet_valid' };
  if (payload.exp + CLOCK_SKEW_SECONDS < opts.nowSeconds) return { ok: false, error: 'expired' };
  return { ok: true, payload };
}
```

`packages/contracts/src/index.ts`:
```ts
export * from './ticket';
export * from './schemas';
export * from './vectors';
```

- [ ] **Step 5: Testes de schemas e vetores (falhando)**

`packages/contracts/test/schemas.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { HeartbeatBody, ResourceManifest } from '../src/index';

const hb = { name: 'Castelo Bruxo', players: 10, maxPlayers: 512, gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1 };

describe('HeartbeatBody', () => {
  it('aplica defaults', () => {
    expect(HeartbeatBody.parse(hb)).toEqual({ ...hb, description: '', tags: [], locale: 'pt-BR' });
  });
  it('rejeita players > maxPlayers', () => {
    expect(HeartbeatBody.safeParse({ ...hb, players: 600 }).success).toBe(false);
  });
  it('rejeita tag inválida e porta fora da faixa', () => {
    expect(HeartbeatBody.safeParse({ ...hb, tags: ['Com Espaço'] }).success).toBe(false);
    expect(HeartbeatBody.safeParse({ ...hb, gamePort: 70000 }).success).toBe(false);
  });
});

describe('ResourceManifest', () => {
  it('exige sha256 hex de 64 chars', () => {
    const m = { serverId: 'srv_x', protocol: 1, totalSize: 1, resources: [{ name: 'a', version: '1', files: [{ path: 'client/a.lua', sha256: 'z', size: 1 }] }] };
    expect(ResourceManifest.safeParse(m).success).toBe(false);
    m.resources[0]!.files[0]!.sha256 = 'a'.repeat(64);
    expect(ResourceManifest.safeParse(m).success).toBe(true);
  });
});
```

`packages/contracts/test/vectors.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { buildVectors, publicKeyFromRaw, verifyTicket } from '../src/index';

const file = new URL('../test-vectors/tickets.json', import.meta.url);

describe('vetores de ticket', () => {
  it('arquivo commitado é igual ao gerado (rode npm run vectors se mudar)', () => {
    expect(readFileSync(file, 'utf8')).toBe(JSON.stringify(buildVectors(), null, 2) + '\n');
  });
  it('cada caso dá o resultado esperado', () => {
    const v = buildVectors();
    const keys = new Map(v.publicKeys.map((k) => [k.kid, publicKeyFromRaw(k.publicKey)] as const));
    for (const c of v.cases) {
      const r = verifyTicket(c.ticket, { publicKeys: keys, audience: c.audience, nowSeconds: c.nowSeconds });
      expect(r.ok ? 'ok' : r.error, c.name).toBe(c.expected);
    }
  });
});
```

- [ ] **Step 6: Rodar e ver falhar**

Run: `npm test -w @cbmp/contracts`
Expected: FAIL (`HeartbeatBody`/`buildVectors` não exportados).

- [ ] **Step 7: Implementar schemas, vetores e script**

`packages/contracts/src/schemas.ts`:
```ts
import { z } from 'zod';

export const PROTOCOL_VERSION = 1;

export const Tag = z.string().min(1).max(24).regex(/^[a-z0-9-]+$/);
const Port = z.number().int().min(1).max(65535);
const Locale = z.string().regex(/^[a-z]{2}(-[A-Z]{2})?$/);

export const HeartbeatBody = z.object({
  name: z.string().min(3).max(64),
  description: z.string().max(500).default(''),
  players: z.number().int().min(0).max(2048),
  maxPlayers: z.number().int().min(1).max(2048),
  gamePort: Port,
  httpPort: Port,
  version: z.string().min(1).max(32),
  protocol: z.number().int().min(1),
  tags: z.array(Tag).max(10).default([]),
  locale: Locale.default('pt-BR'),
}).refine((b) => b.players <= b.maxPlayers, { message: 'players maior que maxPlayers', path: ['players'] });
export type HeartbeatBody = z.infer<typeof HeartbeatBody>;

export const ServerListing = z.object({
  id: z.string(),
  name: z.string(),
  description: z.string(),
  tags: z.array(z.string()),
  locale: z.string(),
  players: z.number().int(),
  maxPlayers: z.number().int(),
  host: z.string(),
  gamePort: Port,
  httpPort: Port,
  version: z.string(),
  protocol: z.number().int(),
});
export type ServerListing = z.infer<typeof ServerListing>;

/** Resposta de GET /info do cb-server. */
export const ServerInfo = z.object({
  serverId: z.string(),
  name: z.string(),
  players: z.number().int(),
  maxPlayers: z.number().int(),
  protocol: z.number().int(),
  version: z.string(),
});
export type ServerInfo = z.infer<typeof ServerInfo>;

export const ManifestFile = z.object({
  path: z.string().min(1),
  sha256: z.string().regex(/^[0-9a-f]{64}$/),
  size: z.number().int().min(0),
});
export const ResourceManifest = z.object({
  serverId: z.string(),
  protocol: z.number().int(),
  resources: z.array(z.object({ name: z.string(), version: z.string(), files: z.array(ManifestFile) })),
  totalSize: z.number().int().min(0),
});
export type ResourceManifest = z.infer<typeof ResourceManifest>;

export const ErrorBody = z.object({ error: z.string(), message: z.string() });
export type ErrorBody = z.infer<typeof ErrorBody>;

export const TicketRequest = z.object({ serverId: z.string().min(1).max(64) });
export const TicketResponse = z.object({ ticket: z.string(), expiresAt: z.number().int() });
export const PublicKeysResponse = z.array(z.object({ kid: z.string(), publicKey: z.string() }));
export type PublicKeysResponse = z.infer<typeof PublicKeysResponse>;
```

`packages/contracts/src/vectors.ts`:
```ts
import { privateKeyFromSeed, publicKeyToRaw, signTicket, TICKET_TTL_SECONDS, type TicketError, type TicketPayload } from './ticket';

export interface TicketVectorCase {
  name: string;
  ticket: string;
  audience: string;
  nowSeconds: number;
  expected: 'ok' | TicketError;
}
export interface TicketVectors {
  description: string;
  seedHex: string;
  publicKeys: { kid: string; publicKey: string }[];
  cases: TicketVectorCase[];
}

/** Vetores determinísticos (Ed25519 é determinístico) usados pelos testes TS e C++. */
export function buildVectors(): TicketVectors {
  const seed = Buffer.from(Array.from({ length: 32 }, (_, i) => i + 1));
  const priv = privateKeyFromSeed(seed);
  const payload: TicketPayload = {
    v: 1, kid: 'test-1', sub: 'usr_test', did: '111111111111111111', name: 'Jogador Ção',
    aud: 'srv_test', iat: 1_800_000_000, exp: 1_800_000_000 + TICKET_TTL_SECONDS,
    jti: '11111111-1111-4111-8111-111111111111',
  };
  const good = signTicket(payload, priv);
  const [, goodSig] = good.split('.');
  const tampered = Buffer.from(JSON.stringify({ ...payload, name: 'Outro' }), 'utf8').toString('base64url') + '.' + goodSig;
  return {
    description: 'Vetores de ticket CB-MP. Ordem das checagens: formato, JSON, versão, kid, assinatura, audience, iat futuro (>now+60), expiração (exp+60<now).',
    seedHex: seed.toString('hex'),
    publicKeys: [{ kid: 'test-1', publicKey: publicKeyToRaw(priv) }],
    cases: [
      { name: 'valido', ticket: good, audience: 'srv_test', nowSeconds: payload.iat + 10, expected: 'ok' },
      { name: 'limite_expiracao', ticket: good, audience: 'srv_test', nowSeconds: payload.exp + 60, expected: 'ok' },
      { name: 'expirado', ticket: good, audience: 'srv_test', nowSeconds: payload.exp + 61, expected: 'expired' },
      { name: 'futuro', ticket: good, audience: 'srv_test', nowSeconds: payload.iat - 61, expected: 'not_yet_valid' },
      { name: 'audience_errada', ticket: good, audience: 'srv_outro', nowSeconds: payload.iat, expected: 'wrong_audience' },
      { name: 'adulterado', ticket: tampered, audience: 'srv_test', nowSeconds: payload.iat, expected: 'bad_signature' },
      { name: 'kid_desconhecido', ticket: signTicket({ ...payload, kid: 'test-2' }, priv), audience: 'srv_test', nowSeconds: payload.iat, expected: 'unknown_kid' },
      { name: 'versao_2', ticket: signTicket({ ...payload, v: 2 as 1 }, priv), audience: 'srv_test', nowSeconds: payload.iat, expected: 'bad_version' },
      { name: 'malformado', ticket: 'abc', audience: 'srv_test', nowSeconds: payload.iat, expected: 'malformed' },
    ],
  };
}
```

`packages/contracts/scripts/gen-vectors.ts`:
```ts
import { mkdirSync, writeFileSync } from 'node:fs';
import { buildVectors } from '../src/vectors';

const dir = new URL('../test-vectors/', import.meta.url);
mkdirSync(dir, { recursive: true });
writeFileSync(new URL('tickets.json', dir), JSON.stringify(buildVectors(), null, 2) + '\n');
console.log('test-vectors/tickets.json gerado');
```

Run: `npm run vectors -w @cbmp/contracts`
Expected: `test-vectors/tickets.json gerado`.

- [ ] **Step 8: Rodar testes e typecheck**

Run: `npm test -w @cbmp/contracts && npm run typecheck -w @cbmp/contracts`
Expected: todos os testes PASS (ticket 13, schemas 4, vetores 2); typecheck sem erros.

- [ ] **Step 9: Commit**

```bash
git add package.json package-lock.json tsconfig.base.json .gitignore packages/contracts
git commit -m "feat(contracts): tickets Ed25519, schemas e vetores de teste"
```

---

### Task 2: Base do Hub (config, banco, store, app)

**Files:**
- Create: `hub/package.json`, `hub/tsconfig.json`, `hub/src/{config,db,store,errors,app}.ts`, `hub/test/helpers.ts`, `hub/test/store.test.ts`, `hub/test/app.test.ts`
- Note: `app.ts` aqui registra só `/health` e o handler de erros; as rotas entram nas Tasks 3–6 (cada uma adiciona uma linha `await app.register(...)`).

**Interfaces:**
- Consumes: `HeartbeatBody` (Task 1).
- Produces:
  - `interface HubConfig { port: number; host: string; publicUrl: string; dataDir: string; discordClientId: string; discordClientSecret: string; platformAdmins: string[]; devLogin: boolean; trustProxy: boolean; rateLimit: boolean; logLevel: string }`; `loadConfig(env?: NodeJS.ProcessEnv): HubConfig`
  - `openDb(file: string): DB` (`DB = Database.Database`)
  - `class Store` (métodos listados no código), tipos `UserRow`, `ServerRow`, `StatusRow`, `SessionTokens`, `ServerInput`, `ServerPatch`, `ClientKind = 'launcher' | 'panel'`
  - `class HttpError(status: number, code: string, message: string)`
  - `interface HubDeps { config: HubConfig; store: Store; keyring: KeyRing; discord: DiscordClient; probe: ProbeFn; now: () => number }` e `buildApp(deps: HubDeps): Promise<FastifyInstance>`; `app.deps` decorado
  - Testes: `makeTestApp(opts?)` em `hub/test/helpers.ts`

- [ ] **Step 1: Pacote do Hub**

`hub/package.json`:
```json
{
  "name": "@cbmp/hub",
  "version": "0.1.0",
  "private": true,
  "type": "module",
  "scripts": {
    "dev": "tsx watch src/main.ts",
    "start": "tsx src/main.ts",
    "test": "vitest run",
    "typecheck": "tsc -p tsconfig.json"
  }
}
```

`hub/tsconfig.json`:
```json
{ "extends": "../tsconfig.base.json", "include": ["src", "test"] }
```

```bash
npm install -w @cbmp/hub @cbmp/contracts fastify @fastify/cookie @fastify/formbody @fastify/rate-limit better-sqlite3 zod@3
npm install -D -w @cbmp/hub typescript vitest tsx @types/node @types/better-sqlite3
```
(Se `better-sqlite3` não tiver binário pronto para o Node instalado, a instalação compila com o VS 2026 que existe na máquina; deixe compilar.)

- [ ] **Step 2: `errors.ts`, `config.ts`, `db.ts`**

`hub/src/errors.ts`:
```ts
export class HttpError extends Error {
  constructor(readonly status: number, readonly code: string, message: string) {
    super(message);
  }
}
```

`hub/src/config.ts`:
```ts
export interface HubConfig {
  port: number;
  host: string;
  publicUrl: string;
  dataDir: string;
  discordClientId: string;
  discordClientSecret: string;
  platformAdmins: string[];
  devLogin: boolean;
  trustProxy: boolean;
  rateLimit: boolean;
  logLevel: string;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): HubConfig {
  const port = Number(env.HUB_PORT ?? 8787);
  const host = env.HUB_HOST ?? '127.0.0.1';
  const devLogin = env.HUB_DEV_LOGIN === '1';
  const cfg: HubConfig = {
    port,
    host,
    publicUrl: (env.HUB_PUBLIC_URL ?? `http://localhost:${port}`).replace(/\/$/, ''),
    dataDir: env.HUB_DATA_DIR ?? './data',
    discordClientId: env.DISCORD_CLIENT_ID ?? '',
    discordClientSecret: env.DISCORD_CLIENT_SECRET ?? '',
    platformAdmins: (env.HUB_PLATFORM_ADMINS ?? '').split(',').map((s) => s.trim()).filter(Boolean),
    devLogin,
    trustProxy: env.HUB_TRUST_PROXY === '1',
    rateLimit: env.HUB_RATE_LIMIT !== '0',
    logLevel: env.HUB_LOG_LEVEL ?? 'info',
  };
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('HUB_PORT inválida');
  if (!devLogin && (!cfg.discordClientId || !cfg.discordClientSecret)) {
    throw new Error('DISCORD_CLIENT_ID e DISCORD_CLIENT_SECRET são obrigatórios (ou HUB_DEV_LOGIN=1 para testes locais)');
  }
  if (devLogin && host !== '127.0.0.1' && host !== 'localhost') {
    throw new Error('HUB_DEV_LOGIN=1 só é permitido com HUB_HOST=127.0.0.1');
  }
  return cfg;
}
```

`hub/src/db.ts`:
```ts
import Database from 'better-sqlite3';

export type DB = Database.Database;

const MIGRATIONS: string[] = [
  `
  CREATE TABLE users (
    id TEXT PRIMARY KEY, discord_id TEXT NOT NULL UNIQUE, name TEXT NOT NULL, avatar TEXT, created_at INTEGER NOT NULL
  );
  CREATE TABLE sessions (
    id TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, client TEXT NOT NULL,
    access_hash TEXT NOT NULL UNIQUE, access_exp INTEGER NOT NULL,
    refresh_hash TEXT UNIQUE, refresh_exp INTEGER, created_at INTEGER NOT NULL
  );
  CREATE TABLE oauth_states (state TEXT PRIMARY KEY, client TEXT NOT NULL, created_at INTEGER NOT NULL);
  CREATE TABLE login_codes (
    code_hash TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, created_at INTEGER NOT NULL
  );
  CREATE TABLE servers (
    id TEXT PRIMARY KEY, owner_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, key_hash TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '[]',
    locale TEXT NOT NULL DEFAULT 'pt-BR', use_global_bans INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL
  );
  CREATE TABLE server_status (
    server_id TEXT PRIMARY KEY REFERENCES servers(id) ON DELETE CASCADE, ip TEXT NOT NULL,
    game_port INTEGER NOT NULL, http_port INTEGER NOT NULL, players INTEGER NOT NULL, max_players INTEGER NOT NULL,
    version TEXT NOT NULL, protocol INTEGER NOT NULL, last_heartbeat INTEGER NOT NULL,
    verified INTEGER NOT NULL DEFAULT 0, verified_at INTEGER NOT NULL DEFAULT 0
  );
  CREATE TABLE global_bans (
    discord_id TEXT PRIMARY KEY, reason TEXT NOT NULL, created_by TEXT NOT NULL, created_at INTEGER NOT NULL
  );
  `,
];

export function openDb(file: string): DB {
  const db = new Database(file);
  db.pragma('journal_mode = WAL');
  db.pragma('foreign_keys = ON');
  const version = db.pragma('user_version', { simple: true }) as number;
  for (let i = version; i < MIGRATIONS.length; i++) {
    db.transaction(() => {
      db.exec(MIGRATIONS[i]!);
      db.pragma(`user_version = ${i + 1}`);
    })();
  }
  return db;
}
```

- [ ] **Step 3: Testes do store (falhando)**

`hub/test/store.test.ts`:
```ts
import { beforeEach, describe, expect, it } from 'vitest';
import { openDb } from '../src/db';
import { Store } from '../src/store';

let store: Store;
const T = 1_800_000_000;
beforeEach(() => { store = new Store(openDb(':memory:')); });

const hb = { name: 'Srv', description: '', players: 3, maxPlayers: 64, gamePort: 30500, httpPort: 30500, version: '0.1', protocol: 1, tags: ['rp'], locale: 'pt-BR' };

describe('Store', () => {
  it('upsert de usuário por discord id', () => {
    const a = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const b = store.upsertUserFromDiscord({ discordId: '1', name: 'A2', avatar: 'x' }, T + 5);
    expect(b.id).toBe(a.id);
    expect(store.getUserById(a.id)).toEqual({ id: a.id, discordId: '1', name: 'A2', avatar: 'x' });
  });

  it('sessão launcher: access expira, refresh gira tokens', () => {
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const s = store.createSession(u.id, 'launcher', T);
    expect(s.access.startsWith('at_')).toBe(true);
    expect(s.refresh?.startsWith('rt_')).toBe(true);
    expect(store.findUserByAccess(s.access, T + 3599)?.id).toBe(u.id);
    expect(store.findUserByAccess(s.access, T + 3601)).toBeNull();
    const r = store.refreshSession(s.refresh!, T + 3601)!;
    expect(r.access).not.toBe(s.access);
    expect(store.refreshSession(s.refresh!, T + 3602)).toBeNull(); // refresh antigo não vale mais
    expect(store.findUserByAccess(r.access, T + 3602)?.id).toBe(u.id);
  });

  it('sessão painel não tem refresh e dura 12 h', () => {
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const s = store.createSession(u.id, 'panel', T);
    expect(s.refresh).toBeNull();
    expect(store.findUserByAccess(s.access, T + 12 * 3600 - 1)).not.toBeNull();
    expect(store.findUserByAccess(s.access, T + 12 * 3600 + 1)).toBeNull();
  });

  it('oauth state e login code são de uso único e expiram', () => {
    const st = store.createOauthState('launcher', T);
    expect(store.consumeOauthState(st, T + 1)).toBe('launcher');
    expect(store.consumeOauthState(st, T + 2)).toBeNull();
    expect(store.consumeOauthState(store.createOauthState('panel', T), T + 601)).toBeNull();
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const code = store.createLoginCode(u.id, T);
    expect(store.consumeLoginCode(code, T + 59)).toBe(u.id);
    expect(store.consumeLoginCode(code, T + 59)).toBeNull();
    expect(store.consumeLoginCode(store.createLoginCode(u.id, T), T + 61)).toBeNull();
  });

  it('servidor: cria com chave, acha por chave, gira chave, apaga só do dono', () => {
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const other = store.upsertUserFromDiscord({ discordId: '2', name: 'B', avatar: null }, T);
    const { server, key } = store.createServer(u.id, { name: 'Meu', description: 'd', tags: ['rp'], locale: 'pt-BR', useGlobalBans: true }, T);
    expect(server.id).toMatch(/^srv_[A-Za-z0-9_-]{12}$/);
    expect(key).toMatch(/^sk_[A-Za-z0-9_-]{43}$/);
    expect(store.getServerByKey(key)?.id).toBe(server.id);
    const k2 = store.rotateKey(server.id, u.id)!;
    expect(store.getServerByKey(key)).toBeNull();
    expect(store.getServerByKey(k2)?.id).toBe(server.id);
    expect(store.rotateKey(server.id, other.id)).toBeNull();
    expect(store.deleteServer(server.id, other.id)).toBe(false);
    expect(store.deleteServer(server.id, u.id)).toBe(true);
    expect(store.getServer(server.id)).toBeNull();
  });

  it('heartbeat + lista online respeita verificação e janela de 90 s', () => {
    const u = store.upsertUserFromDiscord({ discordId: '1', name: 'A', avatar: null }, T);
    const { server } = store.createServer(u.id, { name: 'Meu', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T);
    store.recordHeartbeat(server.id, hb, '1.2.3.4', T);
    expect(store.listOnline(T)).toEqual([]); // não verificado
    store.setVerified(server.id, true, T);
    expect(store.listOnline(T + 90).map((s) => s.server.id)).toEqual([server.id]);
    expect(store.getServer(server.id)?.name).toBe('Srv'); // heartbeat atualiza nome
    expect(store.listOnline(T + 91)).toEqual([]);
  });

  it('bans globais', () => {
    store.addGlobalBan('9', 'cheat', 'admin1', T);
    expect(store.isGloballyBanned('9')).toBe(true);
    expect(store.listGlobalBans()).toEqual([{ discordId: '9', reason: 'cheat', createdBy: 'admin1', createdAt: T }]);
    expect(store.removeGlobalBan('9')).toBe(true);
    expect(store.isGloballyBanned('9')).toBe(false);
  });
});
```

- [ ] **Step 4: Rodar e ver falhar**

Run: `npm test -w @cbmp/hub`
Expected: FAIL — `Failed to resolve import "../src/store"`.

- [ ] **Step 5: Implementar `store.ts`**

`hub/src/store.ts`:
```ts
import { createHash, randomBytes, randomUUID } from 'node:crypto';
import type { HeartbeatBody } from '@cbmp/contracts';
import type { DB } from './db';

export type ClientKind = 'launcher' | 'panel';
export interface UserRow { id: string; discordId: string; name: string; avatar: string | null }
export interface ServerRow {
  id: string; ownerId: string; name: string; description: string; tags: string[]; locale: string;
  useGlobalBans: boolean; createdAt: number;
}
export interface StatusRow {
  serverId: string; ip: string; gamePort: number; httpPort: number; players: number; maxPlayers: number;
  version: string; protocol: number; lastHeartbeat: number; verified: boolean; verifiedAt: number;
}
export interface SessionTokens { access: string; accessExpiresAt: number; refresh: string | null; refreshExpiresAt: number | null }
export interface ServerInput { name: string; description: string; tags: string[]; locale: string; useGlobalBans: boolean }
export type ServerPatch = Partial<ServerInput>;
export interface GlobalBan { discordId: string; reason: string; createdBy: string; createdAt: number }

export const LAUNCHER_ACCESS_TTL = 3600;
export const LAUNCHER_REFRESH_TTL = 30 * 24 * 3600;
export const PANEL_ACCESS_TTL = 12 * 3600;
export const OAUTH_STATE_TTL = 600;
export const LOGIN_CODE_TTL = 60;
export const ONLINE_WINDOW = 90;

const sha256 = (s: string) => createHash('sha256').update(s).digest('hex');
const token = (prefix: string) => prefix + randomBytes(32).toString('base64url');

type Raw = Record<string, unknown>;
const toUser = (r: Raw): UserRow => ({ id: r.id as string, discordId: r.discord_id as string, name: r.name as string, avatar: (r.avatar as string | null) ?? null });
const toServer = (r: Raw): ServerRow => ({
  id: r.id as string, ownerId: r.owner_id as string, name: r.name as string, description: r.description as string,
  tags: JSON.parse(r.tags as string) as string[], locale: r.locale as string, useGlobalBans: r.use_global_bans === 1,
  createdAt: r.created_at as number,
});
const toStatus = (r: Raw): StatusRow => ({
  serverId: r.server_id as string, ip: r.ip as string, gamePort: r.game_port as number, httpPort: r.http_port as number,
  players: r.players as number, maxPlayers: r.max_players as number, version: r.version as string,
  protocol: r.protocol as number, lastHeartbeat: r.last_heartbeat as number, verified: r.verified === 1,
  verifiedAt: r.verified_at as number,
});

export class Store {
  constructor(private readonly db: DB) {}

  // ---- usuários
  upsertUserFromDiscord(u: { discordId: string; name: string; avatar: string | null }, now: number): UserRow {
    this.db.prepare(`INSERT INTO users (id, discord_id, name, avatar, created_at) VALUES (?, ?, ?, ?, ?)
      ON CONFLICT(discord_id) DO UPDATE SET name = excluded.name, avatar = excluded.avatar`)
      .run(`usr_${randomUUID()}`, u.discordId, u.name, u.avatar, now);
    return toUser(this.db.prepare('SELECT * FROM users WHERE discord_id = ?').get(u.discordId) as Raw);
  }

  getUserById(id: string): UserRow | null {
    const r = this.db.prepare('SELECT * FROM users WHERE id = ?').get(id) as Raw | undefined;
    return r ? toUser(r) : null;
  }

  // ---- sessões
  createSession(userId: string, client: ClientKind, now: number): SessionTokens {
    const access = token('at_');
    const refresh = client === 'launcher' ? token('rt_') : null;
    const accessExpiresAt = now + (client === 'launcher' ? LAUNCHER_ACCESS_TTL : PANEL_ACCESS_TTL);
    const refreshExpiresAt = refresh ? now + LAUNCHER_REFRESH_TTL : null;
    this.db.prepare(`INSERT INTO sessions (id, user_id, client, access_hash, access_exp, refresh_hash, refresh_exp, created_at)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?)`)
      .run(randomUUID(), userId, client, sha256(access), accessExpiresAt, refresh ? sha256(refresh) : null, refreshExpiresAt, now);
    return { access, accessExpiresAt, refresh, refreshExpiresAt };
  }

  findUserByAccess(access: string, now: number): UserRow | null {
    const r = this.db.prepare(`SELECT u.* FROM sessions s JOIN users u ON u.id = s.user_id
      WHERE s.access_hash = ? AND s.access_exp >= ?`).get(sha256(access), now) as Raw | undefined;
    return r ? toUser(r) : null;
  }

  /** Gira access + refresh. O refresh usado deixa de valer. */
  refreshSession(refresh: string, now: number): SessionTokens | null {
    const s = this.db.prepare(`SELECT id, user_id FROM sessions WHERE refresh_hash = ? AND refresh_exp >= ?`)
      .get(sha256(refresh), now) as Raw | undefined;
    if (!s) return null;
    this.db.prepare('DELETE FROM sessions WHERE id = ?').run(s.id);
    return this.createSession(s.user_id as string, 'launcher', now);
  }

  deleteSessionByAccess(access: string): void {
    this.db.prepare('DELETE FROM sessions WHERE access_hash = ?').run(sha256(access));
  }

  // ---- OAuth state e códigos de login (uso único)
  createOauthState(client: ClientKind, now: number): string {
    const state = randomBytes(24).toString('base64url');
    this.db.prepare('INSERT INTO oauth_states (state, client, created_at) VALUES (?, ?, ?)').run(state, client, now);
    return state;
  }

  consumeOauthState(state: string, now: number): ClientKind | null {
    const r = this.db.prepare('DELETE FROM oauth_states WHERE state = ? RETURNING client, created_at').get(state) as Raw | undefined;
    if (!r || now - (r.created_at as number) > OAUTH_STATE_TTL) return null;
    return r.client as ClientKind;
  }

  createLoginCode(userId: string, now: number): string {
    const code = randomBytes(24).toString('base64url');
    this.db.prepare('INSERT INTO login_codes (code_hash, user_id, created_at) VALUES (?, ?, ?)').run(sha256(code), userId, now);
    return code;
  }

  consumeLoginCode(code: string, now: number): string | null {
    const r = this.db.prepare('DELETE FROM login_codes WHERE code_hash = ? RETURNING user_id, created_at').get(sha256(code)) as Raw | undefined;
    if (!r || now - (r.created_at as number) > LOGIN_CODE_TTL) return null;
    return r.user_id as string;
  }

  // ---- servidores
  createServer(ownerId: string, input: ServerInput, now: number): { server: ServerRow; key: string } {
    const id = `srv_${randomBytes(9).toString('base64url')}`;
    const key = token('sk_');
    this.db.prepare(`INSERT INTO servers (id, owner_id, key_hash, name, description, tags, locale, use_global_bans, created_at)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`)
      .run(id, ownerId, sha256(key), input.name, input.description, JSON.stringify(input.tags), input.locale, input.useGlobalBans ? 1 : 0, now);
    return { server: this.getServer(id)!, key };
  }

  getServer(id: string): ServerRow | null {
    const r = this.db.prepare('SELECT * FROM servers WHERE id = ?').get(id) as Raw | undefined;
    return r ? toServer(r) : null;
  }

  getServerByKey(key: string): ServerRow | null {
    const r = this.db.prepare('SELECT * FROM servers WHERE key_hash = ?').get(sha256(key)) as Raw | undefined;
    return r ? toServer(r) : null;
  }

  countServersByOwner(ownerId: string): number {
    return (this.db.prepare('SELECT COUNT(*) AS n FROM servers WHERE owner_id = ?').get(ownerId) as { n: number }).n;
  }

  listServersByOwner(ownerId: string): { server: ServerRow; status: StatusRow | null }[] {
    const rows = this.db.prepare('SELECT * FROM servers WHERE owner_id = ? ORDER BY created_at').all(ownerId) as Raw[];
    return rows.map((r) => ({ server: toServer(r), status: this.getStatus(r.id as string) }));
  }

  updateServer(id: string, ownerId: string, patch: ServerPatch): ServerRow | null {
    const cur = this.getServer(id);
    if (!cur || cur.ownerId !== ownerId) return null;
    const next = { ...cur, ...patch };
    this.db.prepare('UPDATE servers SET name = ?, description = ?, tags = ?, locale = ?, use_global_bans = ? WHERE id = ?')
      .run(next.name, next.description, JSON.stringify(next.tags), next.locale, next.useGlobalBans ? 1 : 0, id);
    return this.getServer(id);
  }

  rotateKey(id: string, ownerId: string): string | null {
    const key = token('sk_');
    const res = this.db.prepare('UPDATE servers SET key_hash = ? WHERE id = ? AND owner_id = ?').run(sha256(key), id, ownerId);
    return res.changes === 1 ? key : null;
  }

  deleteServer(id: string, ownerId: string): boolean {
    return this.db.prepare('DELETE FROM servers WHERE id = ? AND owner_id = ?').run(id, ownerId).changes === 1;
  }

  // ---- status / heartbeat
  getStatus(serverId: string): StatusRow | null {
    const r = this.db.prepare('SELECT * FROM server_status WHERE server_id = ?').get(serverId) as Raw | undefined;
    return r ? toStatus(r) : null;
  }

  /** Grava status e atualiza nome/descrição/tags/locale (server.cfg é a fonte da verdade). */
  recordHeartbeat(serverId: string, hb: HeartbeatBody, ip: string, now: number): void {
    this.db.transaction(() => {
      this.db.prepare(`INSERT INTO server_status (server_id, ip, game_port, http_port, players, max_players, version, protocol, last_heartbeat)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(server_id) DO UPDATE SET ip = excluded.ip, game_port = excluded.game_port, http_port = excluded.http_port,
          players = excluded.players, max_players = excluded.max_players, version = excluded.version,
          protocol = excluded.protocol, last_heartbeat = excluded.last_heartbeat`)
        .run(serverId, ip, hb.gamePort, hb.httpPort, hb.players, hb.maxPlayers, hb.version, hb.protocol, now);
      this.db.prepare('UPDATE servers SET name = ?, description = ?, tags = ?, locale = ? WHERE id = ?')
        .run(hb.name, hb.description, JSON.stringify(hb.tags), hb.locale, serverId);
    })();
  }

  setVerified(serverId: string, verified: boolean, now: number): void {
    this.db.prepare('UPDATE server_status SET verified = ?, verified_at = ? WHERE server_id = ?').run(verified ? 1 : 0, now, serverId);
  }

  listOnline(now: number): { server: ServerRow; status: StatusRow }[] {
    const rows = this.db.prepare(`SELECT s.*, st.* FROM servers s JOIN server_status st ON st.server_id = s.id
      WHERE st.verified = 1 AND st.last_heartbeat >= ? ORDER BY st.players DESC, s.name`).all(now - ONLINE_WINDOW) as Raw[];
    return rows.map((r) => ({ server: toServer(r), status: toStatus(r) }));
  }

  // ---- bans globais
  addGlobalBan(discordId: string, reason: string, createdBy: string, now: number): void {
    this.db.prepare(`INSERT INTO global_bans (discord_id, reason, created_by, created_at) VALUES (?, ?, ?, ?)
      ON CONFLICT(discord_id) DO UPDATE SET reason = excluded.reason, created_by = excluded.created_by, created_at = excluded.created_at`)
      .run(discordId, reason, createdBy, now);
  }

  removeGlobalBan(discordId: string): boolean {
    return this.db.prepare('DELETE FROM global_bans WHERE discord_id = ?').run(discordId).changes === 1;
  }

  isGloballyBanned(discordId: string): boolean {
    return this.db.prepare('SELECT 1 FROM global_bans WHERE discord_id = ?').get(discordId) !== undefined;
  }

  listGlobalBans(): GlobalBan[] {
    return (this.db.prepare('SELECT * FROM global_bans ORDER BY created_at').all() as Raw[]).map((r) => ({
      discordId: r.discord_id as string, reason: r.reason as string, createdBy: r.created_by as string, createdAt: r.created_at as number,
    }));
  }
}
```

- [ ] **Step 6: Rodar testes do store**

Run: `npx vitest run test/store.test.ts` (dentro de `hub/`)
Expected: 7 PASS.

- [ ] **Step 7: Esqueleto do app + helpers de teste (teste falhando primeiro)**

As Tasks 3–6 criam `keyring.ts`, `discord.ts`, `probe.ts`; para o app compilar agora, crie-os já com estas interfaces (a implementação completa vem nas tasks seguintes, que **substituem** estes arquivos):

`hub/src/keyring.ts` (provisório):
```ts
import type { KeyObject } from 'node:crypto';
export interface KeyRing {
  active: { kid: string; privateKey: KeyObject };
  publicKeys(): { kid: string; publicKey: string }[];
}
```

`hub/src/discord.ts` (provisório):
```ts
export interface DiscordUser { id: string; username: string; global_name: string | null; avatar: string | null }
export interface DiscordClient {
  exchangeCode(code: string, redirectUri: string): Promise<string>;
  getUser(accessToken: string): Promise<DiscordUser>;
}
```

`hub/src/probe.ts` (provisório):
```ts
export type ProbeFn = (host: string, httpPort: number) => Promise<{ serverId: string } | null>;
```

`hub/test/app.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { makeTestApp } from './helpers';

describe('app', () => {
  it('GET /health', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/health' });
    expect(res.statusCode).toBe(200);
    expect(res.json()).toEqual({ ok: true });
  });
  it('404 no formato de erro padrão', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/nao-existe' });
    expect(res.statusCode).toBe(404);
    expect(res.json()).toEqual({ error: 'not_found', message: 'rota não encontrada' });
  });
});
```

`hub/test/helpers.ts`:
```ts
import { privateKeyFromSeed, publicKeyToRaw } from '@cbmp/contracts';
import type { HubConfig } from '../src/config';
import { openDb } from '../src/db';
import { Store } from '../src/store';
import { buildApp } from '../src/app';
import type { KeyRing } from '../src/keyring';
import type { DiscordClient, DiscordUser } from '../src/discord';
import type { ProbeFn } from '../src/probe';

export const T0 = 1_800_000_000;

export function testConfig(over: Partial<HubConfig> = {}): HubConfig {
  return {
    port: 8787, host: '127.0.0.1', publicUrl: 'http://localhost:8787', dataDir: ':memory:',
    discordClientId: 'cid', discordClientSecret: 'secret', platformAdmins: ['999'],
    devLogin: false, trustProxy: false, rateLimit: false, logLevel: 'silent', ...over,
  };
}

export function testKeyRing(): KeyRing {
  const privateKey = privateKeyFromSeed(Buffer.alloc(32, 3));
  return { active: { kid: 'kt', privateKey }, publicKeys: () => [{ kid: 'kt', publicKey: publicKeyToRaw(privateKey) }] };
}

export class FakeDiscord implements DiscordClient {
  users = new Map<string, DiscordUser>(); // code -> usuário
  async exchangeCode(code: string): Promise<string> {
    if (!this.users.has(code)) throw new Error('código inválido');
    return `tok_${code}`;
  }
  async getUser(accessToken: string): Promise<DiscordUser> {
    return this.users.get(accessToken.slice(4))!;
  }
}

export async function makeTestApp(opts: { config?: Partial<HubConfig>; probe?: ProbeFn } = {}) {
  const clock = { t: T0 };
  const store = new Store(openDb(':memory:'));
  const discord = new FakeDiscord();
  const probeAnswers = new Map<string, string>(); // "host:port" -> serverId
  const probe: ProbeFn = opts.probe ?? (async (host, port) => {
    const id = probeAnswers.get(`${host}:${port}`);
    return id ? { serverId: id } : null;
  });
  const config = testConfig(opts.config);
  const keyring = testKeyRing();
  const app = await buildApp({ config, store, keyring, discord, probe, now: () => clock.t });
  return { app, store, clock, discord, probeAnswers, keyring, config };
}

/** Cria usuário + sessão de launcher direto no store. */
export function loginAs(store: Store, discordId: string, name = `user${discordId}`, now = T0) {
  const user = store.upsertUserFromDiscord({ discordId, name, avatar: null }, now);
  const tokens = store.createSession(user.id, 'launcher', now);
  return { user, tokens, auth: { authorization: `Bearer ${tokens.access}` } };
}
```

Run: `npx vitest run test/app.test.ts` (em `hub/`) → FAIL (`../src/app` não existe).

- [ ] **Step 8: Implementar `app.ts`**

`hub/src/app.ts`:
```ts
import Fastify, { type FastifyError, type FastifyInstance } from 'fastify';
import cookie from '@fastify/cookie';
import formbody from '@fastify/formbody';
import rateLimit from '@fastify/rate-limit';
import { ZodError } from 'zod';
import type { HubConfig } from './config';
import type { Store, UserRow } from './store';
import type { KeyRing } from './keyring';
import type { DiscordClient } from './discord';
import type { ProbeFn } from './probe';
import { HttpError } from './errors';

export interface HubDeps {
  config: HubConfig;
  store: Store;
  keyring: KeyRing;
  discord: DiscordClient;
  probe: ProbeFn;
  /** Relógio em segundos unix (injetável nos testes). */
  now: () => number;
}

declare module 'fastify' {
  interface FastifyInstance { deps: HubDeps }
  interface FastifyRequest { cachedUser?: UserRow | null }
}

export async function buildApp(deps: HubDeps): Promise<FastifyInstance> {
  const app = Fastify({
    logger: deps.config.logLevel === 'silent' ? false : { level: deps.config.logLevel },
    trustProxy: deps.config.trustProxy,
    bodyLimit: 64 * 1024,
  });
  app.decorate('deps', deps);
  await app.register(cookie);
  await app.register(formbody);
  await app.register(rateLimit, {
    global: deps.config.rateLimit,
    max: 120,
    timeWindow: '1 minute',
    errorResponseBuilder: () => new HttpError(429, 'rate_limited', 'muitas requisições, aguarde um pouco'),
  });

  app.setErrorHandler((err: FastifyError | Error, req, reply) => {
    if (err instanceof HttpError) return reply.status(err.status).send({ error: err.code, message: err.message });
    if (err instanceof ZodError) {
      return reply.status(400).send({
        error: 'invalid_body',
        message: err.issues.map((i) => `${i.path.join('.') || '(corpo)'}: ${i.message}`).join('; '),
      });
    }
    const status = (err as FastifyError).statusCode;
    if (status && status >= 400 && status < 500) return reply.status(status).send({ error: 'bad_request', message: err.message });
    req.log.error(err);
    return reply.status(500).send({ error: 'internal', message: 'erro interno' });
  });
  app.setNotFoundHandler((_req, reply) => reply.status(404).send({ error: 'not_found', message: 'rota não encontrada' }));

  app.get('/health', async () => ({ ok: true }));
  // Tasks seguintes registram rotas aqui.
  return app;
}
```

- [ ] **Step 9: Rodar todos os testes do Hub + typecheck**

Run: `npm test -w @cbmp/hub && npm run typecheck -w @cbmp/hub`
Expected: store 7 PASS, app 2 PASS; typecheck sem erros.

- [ ] **Step 10: Commit**

```bash
git add hub package.json package-lock.json
git commit -m "feat(hub): config, banco SQLite, store e esqueleto do app"
```

---

### Task 3: Chaves, login Discord e sessões

**Files:**
- Modify (substituir inteiro): `hub/src/keyring.ts`, `hub/src/discord.ts`
- Create: `hub/src/auth.ts`, `hub/src/routes/auth.ts`, `hub/test/keyring.test.ts`, `hub/test/auth.test.ts`
- Modify: `hub/src/app.ts` (registrar `authRoutes`)

**Interfaces:**
- Consumes: `Store`, `HttpError`, `HubDeps`, `buildApp`, helpers de teste (Task 2); `generatePrivateKey`, `publicKeyToRaw` (Task 1).
- Produces:
  - `KeyRing` (mesma interface da Task 2) + `loadOrCreateKeyRing(dir: string, nowDate?: Date): KeyRing`
  - `createDiscordClient(clientId: string, clientSecret: string, fetchImpl?: typeof fetch): DiscordClient`
  - `SESSION_COOKIE = 'cbmp_session'`, `currentUser(req: FastifyRequest): UserRow | null`, `requireUser(req): UserRow`, `isPlatformAdmin(config: HubConfig, user: UserRow): boolean`
  - Rotas: `GET /v1/auth/discord/login`, `GET /v1/auth/discord/callback`, `GET /v1/auth/dev-login` (só com `devLogin`), `POST /v1/auth/exchange`, `POST /v1/auth/refresh`, `POST /v1/auth/logout`, `GET /v1/me`
  - Resposta de tokens: `{ accessToken, accessExpiresAt, refreshToken, refreshExpiresAt }`

- [ ] **Step 1: Testes (falhando)**

`hub/test/keyring.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { mkdtempSync, readdirSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { generatePrivateKey, publicKeyToRaw, signTicket, verifyTicket, publicKeyFromRaw, TICKET_TTL_SECONDS } from '@cbmp/contracts';
import { loadOrCreateKeyRing } from '../src/keyring';

describe('keyring', () => {
  it('cria uma chave na primeira vez e reusa depois', () => {
    const dir = mkdtempSync(join(tmpdir(), 'cbmp-keys-'));
    const a = loadOrCreateKeyRing(dir, new Date('2026-11-09T12:00:00Z'));
    expect(a.active.kid).toBe('k20261109120000');
    expect(readdirSync(dir)).toEqual(['k20261109120000.pem']);
    const b = loadOrCreateKeyRing(dir);
    expect(b.publicKeys()).toEqual(a.publicKeys());
  });
  it('a chave mais nova (por nome) é a ativa e as antigas continuam publicadas', () => {
    const dir = mkdtempSync(join(tmpdir(), 'cbmp-keys-'));
    loadOrCreateKeyRing(dir, new Date('2026-01-01T00:00:00Z'));
    const nova = generatePrivateKey();
    writeFileSync(join(dir, 'k20270101000000.pem'), nova.export({ format: 'pem', type: 'pkcs8' }));
    const ring = loadOrCreateKeyRing(dir);
    expect(ring.active.kid).toBe('k20270101000000');
    expect(ring.publicKeys().map((k) => k.kid)).toEqual(['k20260101000000', 'k20270101000000']);
    const t = signTicket({ v: 1, kid: ring.active.kid, sub: 's', did: 'd', name: 'n', aud: 'a', iat: 1, exp: 1 + TICKET_TTL_SECONDS, jti: 'j' }, ring.active.privateKey);
    const keys = new Map(ring.publicKeys().map((k) => [k.kid, publicKeyFromRaw(k.publicKey)] as const));
    expect(verifyTicket(t, { publicKeys: keys, audience: 'a', nowSeconds: 2 }).ok).toBe(true);
    expect(ring.publicKeys()[1]!.publicKey).toBe(publicKeyToRaw(nova));
  });
});
```

`hub/test/auth.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { makeTestApp, loginAs } from './helpers';

async function startLogin(app: Awaited<ReturnType<typeof makeTestApp>>['app'], client: 'launcher' | 'panel') {
  const res = await app.inject({ method: 'GET', url: `/v1/auth/discord/login?client=${client}` });
  expect(res.statusCode).toBe(302);
  const loc = new URL(res.headers.location as string);
  expect(loc.origin + loc.pathname).toBe('https://discord.com/oauth2/authorize');
  expect(loc.searchParams.get('scope')).toBe('identify');
  expect(loc.searchParams.get('redirect_uri')).toBe('http://localhost:8787/v1/auth/discord/callback');
  return loc.searchParams.get('state')!;
}

describe('auth', () => {
  it('launcher: OAuth → cbmp://auth?code → exchange → /v1/me', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c1', { id: '123', username: 'robson', global_name: 'Robson', avatar: 'av' });
    const state = await startLogin(app, 'launcher');
    const cb = await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c1&state=${state}` });
    expect(cb.statusCode).toBe(302);
    const deep = new URL(cb.headers.location as string);
    expect(deep.protocol).toBe('cbmp:');
    const code = deep.searchParams.get('code')!;
    const ex = await app.inject({ method: 'POST', url: '/v1/auth/exchange', payload: { code } });
    expect(ex.statusCode).toBe(200);
    const tokens = ex.json();
    expect(tokens.accessToken).toMatch(/^at_/);
    expect(tokens.refreshToken).toMatch(/^rt_/);
    const me = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: `Bearer ${tokens.accessToken}` } });
    expect(me.json()).toMatchObject({ discordId: '123', name: 'Robson', avatar: 'av', isPlatformAdmin: false });
    const again = await app.inject({ method: 'POST', url: '/v1/auth/exchange', payload: { code } });
    expect(again.json().error).toBe('bad_code');
  });

  it('painel: OAuth grava cookie e redireciona para /panel', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c2', { id: '999', username: 'adm', global_name: null, avatar: null });
    const state = await startLogin(app, 'panel');
    const cb = await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c2&state=${state}` });
    expect(cb.headers.location).toBe('/panel');
    const cookie = cb.cookies.find((c) => c.name === 'cbmp_session')!;
    expect(cookie.httpOnly).toBe(true);
    expect(cookie.sameSite).toBe('Strict');
    const me = await app.inject({ method: 'GET', url: '/v1/me', cookies: { cbmp_session: cookie.value } });
    expect(me.json()).toMatchObject({ name: 'adm', isPlatformAdmin: true });
  });

  it('state inválido ou reutilizado é recusado', async () => {
    const { app, discord } = await makeTestApp();
    discord.users.set('c3', { id: '1', username: 'x', global_name: null, avatar: null });
    const state = await startLogin(app, 'launcher');
    await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c3&state=${state}` });
    const res = await app.inject({ method: 'GET', url: `/v1/auth/discord/callback?code=c3&state=${state}` });
    expect(res.statusCode).toBe(400);
    expect(res.json().error).toBe('bad_state');
  });

  it('client inválido no login é 400', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/v1/auth/discord/login?client=outro' });
    expect(res.statusCode).toBe(400);
  });

  it('refresh gira tokens; logout invalida', async () => {
    const { app, store } = await makeTestApp();
    const { tokens } = loginAs(store, '5');
    const r = await app.inject({ method: 'POST', url: '/v1/auth/refresh', payload: { refreshToken: tokens.refresh } });
    expect(r.statusCode).toBe(200);
    const novo = r.json();
    expect(novo.accessToken).not.toBe(tokens.access);
    const bad = await app.inject({ method: 'POST', url: '/v1/auth/refresh', payload: { refreshToken: tokens.refresh } });
    expect(bad.json().error).toBe('bad_refresh');
    const out = await app.inject({ method: 'POST', url: '/v1/auth/logout', headers: { authorization: `Bearer ${novo.accessToken}` } });
    expect(out.statusCode).toBe(200);
    const me = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: `Bearer ${novo.accessToken}` } });
    expect(me.statusCode).toBe(401);
  });

  it('/v1/me sem sessão é 401 unauthorized', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/v1/me' });
    expect(res.statusCode).toBe(401);
    expect(res.json().error).toBe('unauthorized');
  });

  it('dev-login só existe com devLogin=true', async () => {
    const off = await makeTestApp();
    expect((await off.app.inject({ method: 'GET', url: '/v1/auth/dev-login?client=panel&discordId=1&name=A' })).statusCode).toBe(404);
    const on = await makeTestApp({ config: { devLogin: true } });
    const res = await on.app.inject({ method: 'GET', url: '/v1/auth/dev-login?client=panel&discordId=42&name=Dev' });
    expect(res.headers.location).toBe('/panel');
    expect(res.cookies.some((c) => c.name === 'cbmp_session')).toBe(true);
  });
});
```

Run: `npm test -w @cbmp/hub` → FAIL (`loadOrCreateKeyRing` não existe; rotas 404).

- [ ] **Step 2: `keyring.ts` e `discord.ts`**

`hub/src/keyring.ts`:
```ts
import { createPrivateKey, type KeyObject } from 'node:crypto';
import { mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { generatePrivateKey, publicKeyToRaw } from '@cbmp/contracts';

export interface KeyRing {
  active: { kid: string; privateKey: KeyObject };
  publicKeys(): { kid: string; publicKey: string }[];
}

/**
 * Chaves em `<dir>/<kid>.pem` (PKCS#8). A ativa é a de maior nome (kid = k + AAAAMMDDhhmmss).
 * Rotação: gerar novo .pem com kid maior; as antigas continuam publicadas até serem removidas.
 */
export function loadOrCreateKeyRing(dir: string, nowDate: Date = new Date()): KeyRing {
  mkdirSync(dir, { recursive: true });
  let files = readdirSync(dir).filter((f) => f.endsWith('.pem')).sort();
  if (files.length === 0) {
    const kid = 'k' + nowDate.toISOString().replace(/\D/g, '').slice(0, 14);
    writeFileSync(join(dir, `${kid}.pem`), generatePrivateKey().export({ format: 'pem', type: 'pkcs8' }), { mode: 0o600 });
    files = [`${kid}.pem`];
  }
  const entries = files.map((f) => ({ kid: f.slice(0, -4), privateKey: createPrivateKey(readFileSync(join(dir, f))) }));
  const published = entries.map((e) => ({ kid: e.kid, publicKey: publicKeyToRaw(e.privateKey) }));
  return { active: entries[entries.length - 1]!, publicKeys: () => published };
}
```

`hub/src/discord.ts`:
```ts
import { HttpError } from './errors';

export interface DiscordUser { id: string; username: string; global_name: string | null; avatar: string | null }
export interface DiscordClient {
  exchangeCode(code: string, redirectUri: string): Promise<string>;
  getUser(accessToken: string): Promise<DiscordUser>;
}

const API = 'https://discord.com/api/v10';

export function createDiscordClient(clientId: string, clientSecret: string, fetchImpl: typeof fetch = fetch): DiscordClient {
  return {
    async exchangeCode(code, redirectUri) {
      const res = await fetchImpl(`${API}/oauth2/token`, {
        method: 'POST',
        headers: { 'content-type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ client_id: clientId, client_secret: clientSecret, grant_type: 'authorization_code', code, redirect_uri: redirectUri }),
        signal: AbortSignal.timeout(10_000),
      });
      if (!res.ok) throw new HttpError(502, 'discord_error', 'falha ao validar login no Discord');
      const body = (await res.json()) as { access_token?: string };
      if (!body.access_token) throw new HttpError(502, 'discord_error', 'resposta inesperada do Discord');
      return body.access_token;
    },
    async getUser(accessToken) {
      const res = await fetchImpl(`${API}/users/@me`, { headers: { authorization: `Bearer ${accessToken}` }, signal: AbortSignal.timeout(10_000) });
      if (!res.ok) throw new HttpError(502, 'discord_error', 'falha ao ler usuário do Discord');
      return (await res.json()) as DiscordUser;
    },
  };
}
```

- [ ] **Step 3: `auth.ts` e `routes/auth.ts`**

`hub/src/auth.ts`:
```ts
import type { FastifyRequest } from 'fastify';
import type { HubConfig } from './config';
import type { UserRow } from './store';
import { HttpError } from './errors';

export const SESSION_COOKIE = 'cbmp_session';

export function bearer(req: FastifyRequest, prefix: string): string | null {
  const h = req.headers.authorization;
  if (!h?.startsWith('Bearer ')) return null;
  const t = h.slice(7).trim();
  return t.startsWith(prefix) ? t : null;
}

/** Sessão via `Authorization: Bearer at_...` (launcher) ou cookie (painel). */
export function currentUser(req: FastifyRequest): UserRow | null {
  if (req.cachedUser !== undefined) return req.cachedUser;
  const { store, now } = req.server.deps;
  const token = bearer(req, 'at_') ?? req.cookies[SESSION_COOKIE] ?? null;
  req.cachedUser = token ? store.findUserByAccess(token, now()) : null;
  return req.cachedUser;
}

export function requireUser(req: FastifyRequest): UserRow {
  const u = currentUser(req);
  if (!u) throw new HttpError(401, 'unauthorized', 'faça login');
  return u;
}

export function isPlatformAdmin(config: HubConfig, user: UserRow): boolean {
  return config.platformAdmins.includes(user.discordId);
}
```

`hub/src/routes/auth.ts`:
```ts
import type { FastifyInstance, FastifyReply } from 'fastify';
import { z } from 'zod';
import { HttpError } from '../errors';
import { SESSION_COOKIE, bearer, isPlatformAdmin, requireUser } from '../auth';
import type { ClientKind, SessionTokens } from '../store';

const ClientQuery = z.object({ client: z.enum(['launcher', 'panel']) });

const tokenResponse = (s: SessionTokens) => ({
  accessToken: s.access, accessExpiresAt: s.accessExpiresAt, refreshToken: s.refresh, refreshExpiresAt: s.refreshExpiresAt,
});

export async function authRoutes(app: FastifyInstance) {
  const { config, store, discord, now } = app.deps;
  const redirectUri = `${config.publicUrl}/v1/auth/discord/callback`;

  function finishLogin(reply: FastifyReply, userId: string, client: ClientKind) {
    if (client === 'panel') {
      const s = store.createSession(userId, 'panel', now());
      reply.setCookie(SESSION_COOKIE, s.access, {
        path: '/', httpOnly: true, sameSite: 'strict', secure: config.publicUrl.startsWith('https://'),
        maxAge: s.accessExpiresAt - now(),
      });
      return reply.redirect('/panel');
    }
    const code = store.createLoginCode(userId, now());
    return reply.redirect(`cbmp://auth?code=${encodeURIComponent(code)}`);
  }

  app.get('/v1/auth/discord/login', async (req, reply) => {
    const { client } = ClientQuery.parse(req.query);
    const url = new URL('https://discord.com/oauth2/authorize');
    url.search = new URLSearchParams({
      client_id: config.discordClientId, redirect_uri: redirectUri, response_type: 'code', scope: 'identify',
      state: store.createOauthState(client, now()), prompt: 'none',
    }).toString();
    return reply.redirect(url.toString());
  });

  app.get('/v1/auth/discord/callback', async (req, reply) => {
    const q = z.object({ code: z.string().min(1), state: z.string().min(1) }).parse(req.query);
    const client = store.consumeOauthState(q.state, now());
    if (!client) throw new HttpError(400, 'bad_state', 'login expirado ou inválido, tente de novo');
    const du = await discord.getUser(await discord.exchangeCode(q.code, redirectUri));
    const user = store.upsertUserFromDiscord({ discordId: du.id, name: du.global_name ?? du.username, avatar: du.avatar }, now());
    return finishLogin(reply, user.id, client);
  });

  if (config.devLogin) {
    app.get('/v1/auth/dev-login', async (req, reply) => {
      const q = ClientQuery.extend({ discordId: z.string().regex(/^\d{1,20}$/), name: z.string().min(1).max(32) }).parse(req.query);
      const user = store.upsertUserFromDiscord({ discordId: q.discordId, name: q.name, avatar: null }, now());
      return finishLogin(reply, user.id, q.client);
    });
  }

  app.post('/v1/auth/exchange', async (req) => {
    const { code } = z.object({ code: z.string().min(1) }).parse(req.body);
    const userId = store.consumeLoginCode(code, now());
    if (!userId) throw new HttpError(400, 'bad_code', 'código de login inválido ou expirado');
    return tokenResponse(store.createSession(userId, 'launcher', now()));
  });

  app.post('/v1/auth/refresh', async (req) => {
    const { refreshToken } = z.object({ refreshToken: z.string().min(1) }).parse(req.body);
    const s = store.refreshSession(refreshToken, now());
    if (!s) throw new HttpError(401, 'bad_refresh', 'sessão expirada, faça login de novo');
    return tokenResponse(s);
  });

  app.post('/v1/auth/logout', async (req, reply) => {
    const token = bearer(req, 'at_') ?? req.cookies[SESSION_COOKIE];
    if (token) store.deleteSessionByAccess(token);
    reply.clearCookie(SESSION_COOKIE, { path: '/' });
    return { ok: true };
  });

  app.get('/v1/me', async (req) => {
    const u = requireUser(req);
    return { id: u.id, discordId: u.discordId, name: u.name, avatar: u.avatar, isPlatformAdmin: isPlatformAdmin(config, u) };
  });
}
```

Em `hub/src/app.ts`, adicionar o import e o registro no lugar do comentário:
```ts
import { authRoutes } from './routes/auth';
// ...
  await app.register(authRoutes);
  // Tasks seguintes registram rotas aqui.
```

- [ ] **Step 4: Rodar testes + typecheck**

Run: `npm test -w @cbmp/hub && npm run typecheck -w @cbmp/hub`
Expected: todos PASS (keyring 2, auth 7, store 7, app 2).

- [ ] **Step 5: Commit**

```bash
git add hub
git commit -m "feat(hub): keyring Ed25519, login Discord, sessões e /v1/me"
```

---

### Task 4: Servidores do dono + tickets

**Files:**
- Create: `hub/src/routes/owner.ts`, `hub/src/routes/tickets.ts`, `hub/test/owner.test.ts`, `hub/test/tickets.test.ts`
- Modify: `hub/src/app.ts` (registrar as duas rotas)

**Interfaces:**
- Consumes: `Store` (createServer/listServersByOwner/updateServer/rotateKey/deleteServer/countServersByOwner/getServer/isGloballyBanned), `requireUser`, `HttpError`, `KeyRing` (Tasks 2–3); `signTicket`, `TicketRequest`, `TICKET_TTL_SECONDS`, `Tag` (Task 1).
- Produces:
  - `CreateServerBody`, `PatchServerBody` (zod) exportados de `hub/src/routes/owner.ts` (usados pelo painel na Task 6)
  - `MAX_SERVERS_PER_OWNER = 10`
  - `serverView(server: ServerRow, status: StatusRow | null, now: number)` exportado de `owner.ts` → `{ id, name, description, tags, locale, useGlobalBans, createdAt, online, verified, players, maxPlayers, host, gamePort, httpPort, version, lastHeartbeat }`
  - Rotas: `POST/GET /v1/owner/servers`, `PATCH/DELETE /v1/owner/servers/:id`, `POST /v1/owner/servers/:id/rotate-key`, `POST /v1/tickets`, `GET /v1/keys/public`

- [ ] **Step 1: Testes (falhando)**

`hub/test/owner.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { makeTestApp, loginAs } from './helpers';

describe('owner servers', () => {
  it('cria, lista, edita, gira chave e apaga', async () => {
    const { app, store } = await makeTestApp();
    const { auth } = loginAs(store, '1');
    const c = await app.inject({ method: 'POST', url: '/v1/owner/servers', headers: auth, payload: { name: 'Castelo', tags: ['rp'] } });
    expect(c.statusCode).toBe(201);
    const { server, key } = c.json();
    expect(key).toMatch(/^sk_/);
    expect(server).toMatchObject({ name: 'Castelo', description: '', tags: ['rp'], locale: 'pt-BR', useGlobalBans: false, online: false });

    const l = await app.inject({ method: 'GET', url: '/v1/owner/servers', headers: auth });
    expect(l.json().map((s: { id: string }) => s.id)).toEqual([server.id]);
    expect(JSON.stringify(l.json())).not.toContain('sk_');

    const p = await app.inject({ method: 'PATCH', url: `/v1/owner/servers/${server.id}`, headers: auth, payload: { description: 'RP bruxo', useGlobalBans: true } });
    expect(p.json()).toMatchObject({ description: 'RP bruxo', useGlobalBans: true, name: 'Castelo' });

    const r = await app.inject({ method: 'POST', url: `/v1/owner/servers/${server.id}/rotate-key`, headers: auth });
    expect(r.json().key).not.toBe(key);

    const d = await app.inject({ method: 'DELETE', url: `/v1/owner/servers/${server.id}`, headers: auth });
    expect(d.statusCode).toBe(204);
  });

  it('outro usuário não vê nem mexe no servidor', async () => {
    const { app, store } = await makeTestApp();
    const a = loginAs(store, '1');
    const b = loginAs(store, '2');
    const { server } = (await app.inject({ method: 'POST', url: '/v1/owner/servers', headers: a.auth, payload: { name: 'Castelo' } })).json();
    expect((await app.inject({ method: 'GET', url: '/v1/owner/servers', headers: b.auth })).json()).toEqual([]);
    expect((await app.inject({ method: 'PATCH', url: `/v1/owner/servers/${server.id}`, headers: b.auth, payload: { name: 'Roubado' } })).statusCode).toBe(404);
    expect((await app.inject({ method: 'POST', url: `/v1/owner/servers/${server.id}/rotate-key`, headers: b.auth })).statusCode).toBe(404);
    expect((await app.inject({ method: 'DELETE', url: `/v1/owner/servers/${server.id}`, headers: b.auth })).statusCode).toBe(404);
  });

  it('valida corpo e limita 10 servidores por dono', async () => {
    const { app, store } = await makeTestApp();
    const { auth } = loginAs(store, '1');
    const bad = await app.inject({ method: 'POST', url: '/v1/owner/servers', headers: auth, payload: { name: 'x' } });
    expect(bad.statusCode).toBe(400);
    expect(bad.json().error).toBe('invalid_body');
    for (let i = 0; i < 10; i++) {
      expect((await app.inject({ method: 'POST', url: '/v1/owner/servers', headers: auth, payload: { name: `Srv ${i}` } })).statusCode).toBe(201);
    }
    const over = await app.inject({ method: 'POST', url: '/v1/owner/servers', headers: auth, payload: { name: 'Srv 11' } });
    expect(over.statusCode).toBe(409);
    expect(over.json().error).toBe('too_many_servers');
  });

  it('exige login', async () => {
    const { app } = await makeTestApp();
    expect((await app.inject({ method: 'GET', url: '/v1/owner/servers' })).statusCode).toBe(401);
  });
});
```

`hub/test/tickets.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { publicKeyFromRaw, verifyTicket } from '@cbmp/contracts';
import { makeTestApp, loginAs, T0 } from './helpers';

async function setup(useGlobalBans = false) {
  const t = await makeTestApp();
  const owner = loginAs(t.store, '1');
  const { server } = t.store.createServer(owner.user.id, { name: 'Srv', description: '', tags: [], locale: 'pt-BR', useGlobalBans }, T0);
  const player = loginAs(t.store, '77', 'Jogador');
  return { ...t, server, player };
}

describe('tickets', () => {
  it('emite ticket válido para o servidor, verificável com /v1/keys/public', async () => {
    const { app, server, player } = await setup();
    const res = await app.inject({ method: 'POST', url: '/v1/tickets', headers: player.auth, payload: { serverId: server.id } });
    expect(res.statusCode).toBe(200);
    const { ticket, expiresAt } = res.json();
    expect(expiresAt).toBe(T0 + 300);
    const pubs = (await app.inject({ method: 'GET', url: '/v1/keys/public' })).json() as { kid: string; publicKey: string }[];
    const keys = new Map(pubs.map((k) => [k.kid, publicKeyFromRaw(k.publicKey)] as const));
    const r = verifyTicket(ticket, { publicKeys: keys, audience: server.id, nowSeconds: T0 + 5 });
    expect(r.ok && r.payload).toMatchObject({ v: 1, kid: 'kt', sub: player.user.id, did: '77', name: 'Jogador', aud: server.id, iat: T0, exp: T0 + 300 });
  });

  it('jti é único por ticket', async () => {
    const { app, server, player } = await setup();
    const a = (await app.inject({ method: 'POST', url: '/v1/tickets', headers: player.auth, payload: { serverId: server.id } })).json().ticket;
    const b = (await app.inject({ method: 'POST', url: '/v1/tickets', headers: player.auth, payload: { serverId: server.id } })).json().ticket;
    const jti = (t: string) => JSON.parse(Buffer.from(t.split('.')[0]!, 'base64url').toString()).jti;
    expect(jti(a)).not.toBe(jti(b));
  });

  it('servidor inexistente → 404; sem login → 401', async () => {
    const { app, player } = await setup();
    expect((await app.inject({ method: 'POST', url: '/v1/tickets', headers: player.auth, payload: { serverId: 'srv_nao' } })).statusCode).toBe(404);
    expect((await app.inject({ method: 'POST', url: '/v1/tickets', payload: { serverId: 'srv_nao' } })).statusCode).toBe(401);
  });

  it('ban global só bloqueia em servidor que optou', async () => {
    const optIn = await setup(true);
    optIn.store.addGlobalBan('77', 'cheat', '999', T0);
    const r1 = await optIn.app.inject({ method: 'POST', url: '/v1/tickets', headers: optIn.player.auth, payload: { serverId: optIn.server.id } });
    expect(r1.statusCode).toBe(403);
    expect(r1.json().error).toBe('banned_global');

    const optOut = await setup(false);
    optOut.store.addGlobalBan('77', 'cheat', '999', T0);
    const r2 = await optOut.app.inject({ method: 'POST', url: '/v1/tickets', headers: optOut.player.auth, payload: { serverId: optOut.server.id } });
    expect(r2.statusCode).toBe(200);
  });
});
```

Run: `npm test -w @cbmp/hub` → FAIL (rotas 404).

- [ ] **Step 2: `routes/owner.ts`**

```ts
import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { Tag } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { requireUser } from '../auth';
import { ONLINE_WINDOW, type ServerRow, type StatusRow } from '../store';

export const MAX_SERVERS_PER_OWNER = 10;

export const CreateServerBody = z.object({
  name: z.string().trim().min(3).max(64),
  description: z.string().max(500).default(''),
  tags: z.array(Tag).max(10).default([]),
  locale: z.string().regex(/^[a-z]{2}(-[A-Z]{2})?$/).default('pt-BR'),
  useGlobalBans: z.boolean().default(false),
});
export const PatchServerBody = z.object({
  name: z.string().trim().min(3).max(64),
  description: z.string().max(500),
  tags: z.array(Tag).max(10),
  locale: z.string().regex(/^[a-z]{2}(-[A-Z]{2})?$/),
  useGlobalBans: z.boolean(),
}).partial();

export function serverView(server: ServerRow, status: StatusRow | null, now: number) {
  const online = !!status && status.verified && status.lastHeartbeat >= now - ONLINE_WINDOW;
  return {
    id: server.id, name: server.name, description: server.description, tags: server.tags, locale: server.locale,
    useGlobalBans: server.useGlobalBans, createdAt: server.createdAt,
    online, verified: status?.verified ?? false, players: status?.players ?? 0, maxPlayers: status?.maxPlayers ?? 0,
    host: status?.ip ?? null, gamePort: status?.gamePort ?? null, httpPort: status?.httpPort ?? null,
    version: status?.version ?? null, lastHeartbeat: status?.lastHeartbeat ?? null,
  };
}

const IdParams = z.object({ id: z.string().min(1).max(64) });

export async function ownerRoutes(app: FastifyInstance) {
  const { store, now } = app.deps;

  app.post('/v1/owner/servers', async (req, reply) => {
    const user = requireUser(req);
    const body = CreateServerBody.parse(req.body);
    if (store.countServersByOwner(user.id) >= MAX_SERVERS_PER_OWNER) {
      throw new HttpError(409, 'too_many_servers', `limite de ${MAX_SERVERS_PER_OWNER} servidores por conta`);
    }
    const { server, key } = store.createServer(user.id, body, now());
    return reply.status(201).send({ server: serverView(server, null, now()), key });
  });

  app.get('/v1/owner/servers', async (req) => {
    const user = requireUser(req);
    return store.listServersByOwner(user.id).map(({ server, status }) => serverView(server, status, now()));
  });

  app.patch('/v1/owner/servers/:id', async (req) => {
    const user = requireUser(req);
    const { id } = IdParams.parse(req.params);
    const updated = store.updateServer(id, user.id, PatchServerBody.parse(req.body));
    if (!updated) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return serverView(updated, store.getStatus(id), now());
  });

  app.post('/v1/owner/servers/:id/rotate-key', async (req) => {
    const user = requireUser(req);
    const { id } = IdParams.parse(req.params);
    const key = store.rotateKey(id, user.id);
    if (!key) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return { key };
  });

  app.delete('/v1/owner/servers/:id', async (req, reply) => {
    const user = requireUser(req);
    const { id } = IdParams.parse(req.params);
    if (!store.deleteServer(id, user.id)) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return reply.status(204).send();
  });
}
```

- [ ] **Step 3: `routes/tickets.ts`**

```ts
import { randomUUID } from 'node:crypto';
import type { FastifyInstance } from 'fastify';
import { TICKET_TTL_SECONDS, TicketRequest, signTicket, type TicketPayload } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { requireUser } from '../auth';

export async function ticketRoutes(app: FastifyInstance) {
  const { store, keyring, now } = app.deps;

  app.get('/v1/keys/public', async () => keyring.publicKeys());

  app.post('/v1/tickets', { config: { rateLimit: { max: 30, timeWindow: '1 minute' } } }, async (req) => {
    const user = requireUser(req);
    const { serverId } = TicketRequest.parse(req.body);
    const server = store.getServer(serverId);
    if (!server) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    if (server.useGlobalBans && store.isGloballyBanned(user.discordId)) {
      throw new HttpError(403, 'banned_global', 'sua conta está na lista global de banimentos');
    }
    const iat = now();
    const payload: TicketPayload = {
      v: 1, kid: keyring.active.kid, sub: user.id, did: user.discordId, name: user.name,
      aud: server.id, iat, exp: iat + TICKET_TTL_SECONDS, jti: randomUUID(),
    };
    return { ticket: signTicket(payload, keyring.active.privateKey), expiresAt: payload.exp };
  });
}
```

Em `hub/src/app.ts`, registrar após `authRoutes`:
```ts
import { ownerRoutes } from './routes/owner';
import { ticketRoutes } from './routes/tickets';
// ...
  await app.register(ownerRoutes);
  await app.register(ticketRoutes);
```

- [ ] **Step 4: Rodar testes + typecheck**

Run: `npm test -w @cbmp/hub && npm run typecheck -w @cbmp/hub`
Expected: todos PASS (owner 4, tickets 4 + anteriores).

- [ ] **Step 5: Commit**

```bash
git add hub
git commit -m "feat(hub): CRUD de servidores do dono e emissão de tickets"
```

---

### Task 5: Heartbeat verificado, lista pública, bans globais e rate limit

**Files:**
- Modify (substituir inteiro): `hub/src/probe.ts`
- Create: `hub/src/routes/servers.ts`, `hub/src/routes/bans.ts`, `hub/test/servers.test.ts`, `hub/test/bans.test.ts`, `hub/test/probe.test.ts`
- Modify: `hub/src/app.ts` (registrar as duas rotas)

**Interfaces:**
- Consumes: `Store` (getServerByKey, getStatus, recordHeartbeat, setVerified, listOnline, getServer, global bans), `bearer`, `requireUser`, `isPlatformAdmin`, `serverView` (Tasks 2–4); `HeartbeatBody`, `ServerInfo` (Task 1).
- Produces:
  - `ProbeFn` (mesma assinatura) + `httpProbe: ProbeFn`; `normalizeIp(ip: string): string`
  - `HEARTBEAT_INTERVAL = 30`, `PROBE_INTERVAL = 600`, `LIST_CACHE_SECONDS = 5`
  - Rotas: `POST /v1/servers/heartbeat`, `GET /v1/servers`, `GET /v1/servers/:id`, `GET /v1/bans/global`, `POST /v1/admin/bans/global`, `DELETE /v1/admin/bans/global/:discordId`

- [ ] **Step 1: Testes (falhando)**

`hub/test/servers.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { makeTestApp, loginAs, T0 } from './helpers';

const hb = { name: 'Castelo Bruxo', players: 12, maxPlayers: 512, gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1, tags: ['rp'] };

async function setup(config = {}) {
  const t = await makeTestApp({ config });
  const owner = loginAs(t.store, '1');
  const { server, key } = t.store.createServer(owner.user.id, { name: 'Inicial', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
  return { ...t, server, key };
}
const send = (app: Awaited<ReturnType<typeof setup>>['app'], key: string, body: object = hb) =>
  app.inject({ method: 'POST', url: '/v1/servers/heartbeat', headers: { authorization: `Bearer ${key}` }, payload: body, remoteAddress: '203.0.113.7' });

describe('heartbeat e lista', () => {
  it('lista só depois do probe confirmar o serverId', async () => {
    const { app, server, key, probeAnswers, clock } = await setup();
    const r1 = await send(app, key);
    expect(r1.json()).toEqual({ ok: true, listed: false, nextHeartbeatSeconds: 30, reason: 'probe_failed' });
    expect((await app.inject({ method: 'GET', url: '/v1/servers' })).json()).toEqual([]);

    probeAnswers.set('203.0.113.7:30500', server.id);
    clock.t += 30;
    const r2 = await send(app, key);
    expect(r2.json()).toEqual({ ok: true, listed: true, nextHeartbeatSeconds: 30 });
    clock.t += 5; // vence o cache de 5 s
    const list = (await app.inject({ method: 'GET', url: '/v1/servers' })).json();
    expect(list).toEqual([{
      id: server.id, name: 'Castelo Bruxo', description: '', tags: ['rp'], locale: 'pt-BR', players: 12, maxPlayers: 512,
      host: '203.0.113.7', gamePort: 30500, httpPort: 30500, version: '0.1.0', protocol: 1,
    }]);
  });

  it('probe com serverId de outro servidor não lista', async () => {
    const { app, key, probeAnswers } = await setup();
    probeAnswers.set('203.0.113.7:30500', 'srv_outro');
    expect((await send(app, key)).json().listed).toBe(false);
  });

  it('não repete o probe antes de 10 min, repete depois', async () => {
    let calls = 0;
    const t = await makeTestApp({ probe: async () => { calls++; return { serverId: id }; } });
    const owner = loginAs(t.store, '1');
    const { server, key } = t.store.createServer(owner.user.id, { name: 'Inicial', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
    const id = server.id;
    await send(t.app, key);
    t.clock.t += 30; await send(t.app, key);
    t.clock.t += 30; await send(t.app, key);
    expect(calls).toBe(1);
    t.clock.t += 600; await send(t.app, key);
    expect(calls).toBe(2);
  });

  it('sai da lista após 90 s sem heartbeat', async () => {
    const { app, server, key, probeAnswers, clock } = await setup();
    probeAnswers.set('203.0.113.7:30500', server.id);
    await send(app, key);
    clock.t += 90;
    expect((await app.inject({ method: 'GET', url: '/v1/servers' })).json()).toHaveLength(1);
    clock.t += 6;
    expect((await app.inject({ method: 'GET', url: '/v1/servers' })).json()).toHaveLength(0);
  });

  it('IP vem da conexão, não do corpo', async () => {
    const { app, server, key, probeAnswers, clock } = await setup();
    probeAnswers.set('203.0.113.7:30500', server.id);
    await send(app, key, { ...hb, host: '1.1.1.1', ip: '1.1.1.1' });
    clock.t += 5;
    expect((await app.inject({ method: 'GET', url: '/v1/servers' })).json()[0].host).toBe('203.0.113.7');
  });

  it('chave inválida → 401; corpo inválido → 400', async () => {
    const { app, key } = await setup();
    expect((await send(app, 'sk_errada')).statusCode).toBe(401);
    const res = await app.inject({ method: 'POST', url: '/v1/servers/heartbeat', payload: hb });
    expect(res.statusCode).toBe(401);
    expect((await send(app, key, { ...hb, players: 9999 })).statusCode).toBe(400);
  });

  it('GET /v1/servers/:id mostra online/offline', async () => {
    const { app, server, key, probeAnswers } = await setup();
    const off = (await app.inject({ method: 'GET', url: `/v1/servers/${server.id}` })).json();
    expect(off).toMatchObject({ id: server.id, name: 'Inicial', online: false });
    probeAnswers.set('203.0.113.7:30500', server.id);
    await send(app, key);
    const on = (await app.inject({ method: 'GET', url: `/v1/servers/${server.id}` })).json();
    expect(on).toMatchObject({ id: server.id, online: true, players: 12, host: '203.0.113.7' });
    expect(JSON.stringify(on)).not.toContain('useGlobalBans');
    expect((await app.inject({ method: 'GET', url: '/v1/servers/srv_nada' })).statusCode).toBe(404);
  });

  it('rate limit do heartbeat: 11º em 1 min → 429', async () => {
    const { app, key } = await setup({ rateLimit: true });
    for (let i = 0; i < 10; i++) expect((await send(app, key)).statusCode).toBe(200);
    const r = await send(app, key);
    expect(r.statusCode).toBe(429);
    expect(r.json().error).toBe('rate_limited');
  });
});
```

`hub/test/bans.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { makeTestApp, loginAs, T0 } from './helpers';

describe('bans globais', () => {
  it('admin da plataforma adiciona/remove; servidor lista com sua chave', async () => {
    const { app, store } = await makeTestApp();
    const admin = loginAs(store, '999');
    const { key } = store.createServer(admin.user.id, { name: 'Srv', description: '', tags: [], locale: 'pt-BR', useGlobalBans: true }, T0);
    const add = await app.inject({ method: 'POST', url: '/v1/admin/bans/global', headers: admin.auth, payload: { discordId: '77', reason: 'speedhack' } });
    expect(add.statusCode).toBe(201);
    const list = await app.inject({ method: 'GET', url: '/v1/bans/global', headers: { authorization: `Bearer ${key}` } });
    expect(list.json()).toEqual([{ discordId: '77', reason: 'speedhack', createdBy: '999', createdAt: T0 }]);
    const del = await app.inject({ method: 'DELETE', url: '/v1/admin/bans/global/77', headers: admin.auth });
    expect(del.statusCode).toBe(204);
    expect((await app.inject({ method: 'DELETE', url: '/v1/admin/bans/global/77', headers: admin.auth })).statusCode).toBe(404);
  });

  it('usuário comum não administra; lista exige chave de servidor', async () => {
    const { app, store } = await makeTestApp();
    const user = loginAs(store, '5');
    const r = await app.inject({ method: 'POST', url: '/v1/admin/bans/global', headers: user.auth, payload: { discordId: '77', reason: 'x' } });
    expect(r.statusCode).toBe(403);
    expect(r.json().error).toBe('forbidden');
    expect((await app.inject({ method: 'GET', url: '/v1/bans/global' })).statusCode).toBe(401);
  });
});
```

`hub/test/probe.test.ts`:
```ts
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { createServer, type Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { httpProbe, normalizeIp } from '../src/probe';

let srv: Server;
let port: number;
beforeAll(async () => {
  srv = createServer((req, res) => {
    if (req.url === '/info') {
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ serverId: 'srv_ok', name: 'X', players: 0, maxPlayers: 10, protocol: 1, version: '1' }));
    } else { res.statusCode = 404; res.end(); }
  });
  await new Promise<void>((r) => srv.listen(0, '127.0.0.1', r));
  port = (srv.address() as AddressInfo).port;
});
afterAll(() => srv.close());

describe('probe', () => {
  it('lê serverId de /info', async () => {
    expect(await httpProbe('127.0.0.1', port)).toEqual({ serverId: 'srv_ok' });
  });
  it('porta fechada → null', async () => {
    expect(await httpProbe('127.0.0.1', 1)).toBeNull();
  });
  it('normaliza IPv4 mapeado em IPv6', () => {
    expect(normalizeIp('::ffff:203.0.113.7')).toBe('203.0.113.7');
    expect(normalizeIp('2001:db8::1')).toBe('2001:db8::1');
  });
});
```

Run: `npm test -w @cbmp/hub` → FAIL.

- [ ] **Step 2: `probe.ts`**

```ts
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
```

- [ ] **Step 3: `routes/servers.ts`**

```ts
import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { HeartbeatBody, type ServerListing } from '@cbmp/contracts';
import { HttpError } from '../errors';
import { bearer } from '../auth';
import { normalizeIp } from '../probe';
import { serverView } from './owner';

export const HEARTBEAT_INTERVAL = 30;
export const PROBE_INTERVAL = 600;
export const LIST_CACHE_SECONDS = 5;

export async function serverRoutes(app: FastifyInstance) {
  const { store, probe, now } = app.deps;
  let cache: { at: number; body: ServerListing[] } | null = null;

  app.post('/v1/servers/heartbeat', {
    config: { rateLimit: { max: 10, timeWindow: '1 minute', keyGenerator: (req) => req.headers.authorization ?? req.ip } },
  }, async (req) => {
    const key = bearer(req, 'sk_');
    if (!key) throw new HttpError(401, 'no_key', 'informe a chave do servidor');
    const server = store.getServerByKey(key);
    if (!server) throw new HttpError(401, 'bad_key', 'chave do servidor inválida');
    const hb = HeartbeatBody.parse(req.body);
    const ip = normalizeIp(req.ip);
    const t = now();
    const prev = store.getStatus(server.id);
    store.recordHeartbeat(server.id, hb, ip, t);
    let verified = prev?.verified ?? false;
    const needsProbe = !prev || !prev.verified || prev.ip !== ip || prev.httpPort !== hb.httpPort || t - prev.verifiedAt >= PROBE_INTERVAL;
    if (needsProbe) {
      const info = await probe(ip, hb.httpPort);
      verified = info?.serverId === server.id;
      store.setVerified(server.id, verified, t);
    }
    return verified
      ? { ok: true, listed: true, nextHeartbeatSeconds: HEARTBEAT_INTERVAL }
      : { ok: true, listed: false, nextHeartbeatSeconds: HEARTBEAT_INTERVAL, reason: 'probe_failed' };
  });

  app.get('/v1/servers', async () => {
    const t = now();
    if (cache && t - cache.at < LIST_CACHE_SECONDS) return cache.body;
    const body: ServerListing[] = store.listOnline(t).map(({ server, status }) => ({
      id: server.id, name: server.name, description: server.description, tags: server.tags, locale: server.locale,
      players: status.players, maxPlayers: status.maxPlayers, host: status.ip, gamePort: status.gamePort,
      httpPort: status.httpPort, version: status.version, protocol: status.protocol,
    }));
    cache = { at: t, body };
    return body;
  });

  app.get('/v1/servers/:id', async (req) => {
    const { id } = z.object({ id: z.string().min(1).max(64) }).parse(req.params);
    const server = store.getServer(id);
    if (!server) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    const v = serverView(server, store.getStatus(id), now());
    return {
      id: v.id, name: v.name, description: v.description, tags: v.tags, locale: v.locale, online: v.online,
      ...(v.online ? { players: v.players, maxPlayers: v.maxPlayers, host: v.host, gamePort: v.gamePort, httpPort: v.httpPort, version: v.version } : {}),
    };
  });
}
```

- [ ] **Step 4: `routes/bans.ts`**

```ts
import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { HttpError } from '../errors';
import { bearer, isPlatformAdmin, requireUser } from '../auth';

export async function banRoutes(app: FastifyInstance) {
  const { config, store, now } = app.deps;

  app.get('/v1/bans/global', async (req) => {
    const key = bearer(req, 'sk_');
    if (!key || !store.getServerByKey(key)) throw new HttpError(401, 'bad_key', 'chave do servidor inválida');
    return store.listGlobalBans();
  });

  const requireAdmin = (req: Parameters<typeof requireUser>[0]) => {
    const u = requireUser(req);
    if (!isPlatformAdmin(config, u)) throw new HttpError(403, 'forbidden', 'apenas administradores da plataforma');
    return u;
  };

  app.post('/v1/admin/bans/global', async (req, reply) => {
    const admin = requireAdmin(req);
    const body = z.object({ discordId: z.string().regex(/^\d{1,20}$/), reason: z.string().min(1).max(200) }).parse(req.body);
    store.addGlobalBan(body.discordId, body.reason, admin.discordId, now());
    return reply.status(201).send({ ok: true });
  });

  app.delete('/v1/admin/bans/global/:discordId', async (req, reply) => {
    requireAdmin(req);
    const { discordId } = z.object({ discordId: z.string().regex(/^\d{1,20}$/) }).parse(req.params);
    if (!store.removeGlobalBan(discordId)) throw new HttpError(404, 'not_found', 'ban não encontrado');
    return reply.status(204).send();
  });
}
```

Em `hub/src/app.ts`, registrar:
```ts
import { serverRoutes } from './routes/servers';
import { banRoutes } from './routes/bans';
// ...
  await app.register(serverRoutes);
  await app.register(banRoutes);
```

Nota sobre rate limit: com `rateLimit` desligado (`global: false`), o limite por rota ainda vale para rotas que declaram `config.rateLimit`. Os testes usam `rateLimit: false` por padrão e fazem no máximo 4 heartbeats por chave, então não batem no limite; só o teste de rate limit liga `rateLimit: true`.

- [ ] **Step 5: Rodar testes + typecheck**

Run: `npm test -w @cbmp/hub && npm run typecheck -w @cbmp/hub`
Expected: todos PASS (servers 8, bans 2, probe 3 + anteriores).

- [ ] **Step 6: Commit**

```bash
git add hub
git commit -m "feat(hub): heartbeat verificado por probe, lista pública e bans globais"
```

---

### Task 6: Painel web, processo real e documentação

**Files:**
- Create: `hub/src/routes/panel.ts`, `hub/src/main.ts`, `hub/.env.example`, `hub/README.md`, `hub/test/panel.test.ts`
- Modify: `hub/src/app.ts` (registrar `panelRoutes`), `.gitignore` (adicionar `hub/data/`)

**Interfaces:**
- Consumes: `Store`, `currentUser`, `isPlatformAdmin`, `SESSION_COOKIE`, `CreateServerBody`, `MAX_SERVERS_PER_OWNER`, `serverView`, `loadConfig`, `openDb`, `loadOrCreateKeyRing`, `createDiscordClient`, `httpProbe`, `buildApp` (Tasks 2–5).
- Produces: rotas `GET /panel`, `POST /panel/servers`, `POST /panel/servers/:id/rotate`, `POST /panel/servers/:id/delete`, `POST /panel/logout`; processo `npm start -w @cbmp/hub`.

- [ ] **Step 1: Testes do painel (falhando)**

`hub/test/panel.test.ts`:
```ts
import { describe, expect, it } from 'vitest';
import { makeTestApp, T0 } from './helpers';

const ORIGIN = { origin: 'http://localhost:8787' };
const FORM = { ...ORIGIN, 'content-type': 'application/x-www-form-urlencoded' };

async function panelSession() {
  const t = await makeTestApp();
  const user = t.store.upsertUserFromDiscord({ discordId: '1', name: 'Dono <b>', avatar: null }, T0);
  const s = t.store.createSession(user.id, 'panel', T0);
  return { ...t, user, cookies: { cbmp_session: s.access } };
}

describe('painel', () => {
  it('sem sessão mostra botão de login Discord', async () => {
    const { app } = await makeTestApp();
    const res = await app.inject({ method: 'GET', url: '/panel' });
    expect(res.statusCode).toBe(200);
    expect(res.headers['content-type']).toContain('text/html');
    expect(res.body).toContain('/v1/auth/discord/login?client=panel');
  });

  it('logado: cria servidor e mostra a chave uma única vez, escapando HTML', async () => {
    const { app, cookies, store, user } = await panelSession();
    const res = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: FORM, payload: 'name=Castelo+%3Cscript%3E&description=RP' });
    expect(res.statusCode).toBe(200);
    const key = /sk_[A-Za-z0-9_-]{43}/.exec(res.body)?.[0];
    expect(key).toBeDefined();
    expect(res.body).toContain('Castelo &lt;script&gt;');
    expect(res.body).not.toContain('<script>');
    expect(res.body).toContain('Dono &lt;b&gt;');
    const again = await app.inject({ method: 'GET', url: '/panel', cookies });
    expect(again.body).not.toContain(key!);
    expect(store.listServersByOwner(user.id)).toHaveLength(1);
  });

  it('POST sem Origin igual ao publicUrl é recusado (CSRF)', async () => {
    const { app, cookies } = await panelSession();
    const res = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: { origin: 'https://evil.example', 'content-type': 'application/x-www-form-urlencoded' }, payload: 'name=Castelo' });
    expect(res.statusCode).toBe(403);
  });

  it('gira chave e apaga', async () => {
    const { app, cookies, store, user } = await panelSession();
    const { server } = store.createServer(user.id, { name: 'Srv', description: '', tags: [], locale: 'pt-BR', useGlobalBans: false }, T0);
    const rot = await app.inject({ method: 'POST', url: `/panel/servers/${server.id}/rotate`, cookies, headers: ORIGIN });
    expect(rot.body).toMatch(/sk_[A-Za-z0-9_-]{43}/);
    const del = await app.inject({ method: 'POST', url: `/panel/servers/${server.id}/delete`, cookies, headers: ORIGIN });
    expect(del.statusCode).toBe(303);
    expect(store.listServersByOwner(user.id)).toHaveLength(0);
  });

  it('erro de validação aparece na página', async () => {
    const { app, cookies } = await panelSession();
    const res = await app.inject({ method: 'POST', url: '/panel/servers', cookies, headers: FORM, payload: 'name=x' });
    expect(res.statusCode).toBe(400);
    expect(res.body).toContain('name');
  });
});
```

Run: `npm test -w @cbmp/hub` → FAIL.

- [ ] **Step 2: `routes/panel.ts`**

```ts
import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify';
import { z, ZodError } from 'zod';
import { HttpError } from '../errors';
import { SESSION_COOKIE, currentUser } from '../auth';
import { CreateServerBody, MAX_SERVERS_PER_OWNER, serverView } from './owner';
import type { UserRow } from '../store';

const esc = (s: unknown) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);

function page(title: string, body: string): string {
  return `<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${esc(title)}</title><style>
body{font-family:system-ui,sans-serif;max-width:860px;margin:2rem auto;padding:0 1rem;background:#14111c;color:#ece6f5}
a,button{color:#d9b45a}button{background:#2a2238;border:1px solid #d9b45a;padding:.35rem .8rem;border-radius:6px;cursor:pointer}
input,textarea{width:100%;padding:.4rem;background:#1e1829;color:inherit;border:1px solid #4a3d63;border-radius:6px}
table{width:100%;border-collapse:collapse}td,th{padding:.4rem;border-bottom:1px solid #2f2642;text-align:left}
.key{font-family:monospace;background:#0c0a12;padding:.6rem;border:1px solid #d9b45a;word-break:break-all}
.err{color:#ff8a8a}.ok{color:#8affb0}form.inline{display:inline}
</style></head><body>${body}</body></html>`;
}

export async function panelRoutes(app: FastifyInstance) {
  const { config, store, now } = app.deps;
  const origin = new URL(config.publicUrl).origin;

  function requirePanelUser(req: FastifyRequest): UserRow {
    if (req.headers.origin !== origin) throw new HttpError(403, 'bad_origin', 'origem inválida');
    const u = currentUser(req);
    if (!u) throw new HttpError(401, 'unauthorized', 'faça login');
    return u;
  }

  function dashboard(user: UserRow, extra = ''): string {
    const rows = store.listServersByOwner(user.id).map(({ server, status }) => {
      const v = serverView(server, status, now());
      const estado = v.online ? `<span class="ok">online</span> ${v.players}/${v.maxPlayers}` : v.verified ? 'offline' : 'aguardando 1º heartbeat';
      return `<tr><td>${esc(v.name)}<br><small>${esc(v.id)}</small></td><td>${estado}</td><td>
        <form class="inline" method="post" action="/panel/servers/${esc(v.id)}/rotate"><button>Gerar nova chave</button></form>
        <form class="inline" method="post" action="/panel/servers/${esc(v.id)}/delete" onsubmit="return confirm('Apagar servidor?')"><button>Apagar</button></form>
      </td></tr>`;
    }).join('');
    return page('Painel - servidores', `
      <h1>Seus servidores</h1>
      <p>Logado como <b>${esc(user.name)}</b> <form class="inline" method="post" action="/panel/logout"><button>Sair</button></form></p>
      ${extra}
      <table><tr><th>Servidor</th><th>Estado</th><th></th></tr>${rows || '<tr><td colspan="3">Nenhum servidor ainda.</td></tr>'}</table>
      <h2>Novo servidor (máx. ${MAX_SERVERS_PER_OWNER})</h2>
      <form method="post" action="/panel/servers">
        <p><label>Nome<input name="name" required minlength="3" maxlength="64"></label></p>
        <p><label>Descrição<textarea name="description" maxlength="500"></textarea></label></p>
        <p><button>Criar</button></p>
      </form>`);
  }

  const keyBox = (key: string) =>
    `<p class="ok">Chave do servidor (aparece só agora; cole em <code>server.cfg</code>):</p><p class="key">${esc(key)}</p>`;

  const sendHtml = (reply: FastifyReply, html: string, status = 200) => reply.status(status).type('text/html; charset=utf-8').send(html);

  app.get('/panel', async (req, reply) => {
    const u = currentUser(req);
    if (!u) {
      const dev = config.devLogin
        ? `<p><a href="/v1/auth/dev-login?client=panel&discordId=1&name=Dev">Entrar como Dev (somente local)</a></p>` : '';
      return sendHtml(reply, page('Painel', `<h1>Painel de servidores</h1><p><a href="/v1/auth/discord/login?client=panel"><button>Entrar com Discord</button></a></p>${dev}`));
    }
    return sendHtml(reply, dashboard(u));
  });

  app.post('/panel/servers', async (req, reply) => {
    const u = requirePanelUser(req);
    const form = z.object({ name: z.string(), description: z.string().optional() }).parse(req.body);
    try {
      const body = CreateServerBody.parse({ name: form.name, description: form.description ?? '' });
      if (store.countServersByOwner(u.id) >= MAX_SERVERS_PER_OWNER) {
        return sendHtml(reply, dashboard(u, `<p class="err">Limite de ${MAX_SERVERS_PER_OWNER} servidores atingido.</p>`), 409);
      }
      const { key } = store.createServer(u.id, body, now());
      return sendHtml(reply, dashboard(u, keyBox(key)));
    } catch (e) {
      if (e instanceof ZodError) {
        const msg = e.issues.map((i) => `${i.path.join('.')}: ${i.message}`).join('; ');
        return sendHtml(reply, dashboard(u, `<p class="err">${esc(msg)}</p>`), 400);
      }
      throw e;
    }
  });

  app.post('/panel/servers/:id/rotate', async (req, reply) => {
    const u = requirePanelUser(req);
    const { id } = z.object({ id: z.string().max(64) }).parse(req.params);
    const key = store.rotateKey(id, u.id);
    if (!key) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return sendHtml(reply, dashboard(u, keyBox(key)));
  });

  app.post('/panel/servers/:id/delete', async (req, reply) => {
    const u = requirePanelUser(req);
    const { id } = z.object({ id: z.string().max(64) }).parse(req.params);
    if (!store.deleteServer(id, u.id)) throw new HttpError(404, 'not_found', 'servidor não encontrado');
    return reply.redirect('/panel', 303);
  });

  app.post('/panel/logout', async (req, reply) => {
    if (req.headers.origin !== origin) throw new HttpError(403, 'bad_origin', 'origem inválida');
    const token = req.cookies[SESSION_COOKIE];
    if (token) store.deleteSessionByAccess(token);
    reply.clearCookie(SESSION_COOKIE, { path: '/' });
    return reply.redirect('/panel', 303);
  });
}
```

Em `hub/src/app.ts`, registrar `panelRoutes` (import de `./routes/panel`) após `banRoutes`.

- [ ] **Step 3: `main.ts`, `.env.example`, README**

`hub/src/main.ts`:
```ts
import { mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { loadConfig } from './config';
import { openDb } from './db';
import { Store } from './store';
import { loadOrCreateKeyRing } from './keyring';
import { createDiscordClient } from './discord';
import { httpProbe } from './probe';
import { buildApp } from './app';

const config = loadConfig();
mkdirSync(config.dataDir, { recursive: true });
const store = new Store(openDb(join(config.dataDir, 'hub.sqlite')));
const keyring = loadOrCreateKeyRing(join(config.dataDir, 'keys'));
const discord = createDiscordClient(config.discordClientId, config.discordClientSecret);
const app = await buildApp({ config, store, keyring, discord, probe: httpProbe, now: () => Math.floor(Date.now() / 1000) });
await app.listen({ port: config.port, host: config.host });
app.log.info(`Hub em ${config.publicUrl} (chave ativa ${keyring.active.kid})`);
```

`hub/.env.example`:
```
# Porta e endereço de escuta (atrás do nginx em produção)
HUB_PORT=8787
HUB_HOST=127.0.0.1
# URL pública (usada no redirect do Discord e na checagem de Origin do painel)
HUB_PUBLIC_URL=http://localhost:8787
HUB_DATA_DIR=./data
# App Discord: https://discord.com/developers/applications → OAuth2
# Redirect: <HUB_PUBLIC_URL>/v1/auth/discord/callback
DISCORD_CLIENT_ID=
DISCORD_CLIENT_SECRET=
# Discord IDs dos administradores da plataforma (bans globais), separados por vírgula
HUB_PLATFORM_ADMINS=
# 1 = permite /v1/auth/dev-login (somente com HUB_HOST=127.0.0.1, para testes sem Discord)
HUB_DEV_LOGIN=0
# 1 = confiar em X-Forwarded-For (ligar quando estiver atrás do nginx)
HUB_TRUST_PROXY=0
HUB_RATE_LIMIT=1
HUB_LOG_LEVEL=info
```

`hub/README.md`:
````markdown
# Hub

API + painel de donos da plataforma. Detalhes: `docs/superpowers/specs/2026-11-09-plataforma-design.md`.

## Rodar local sem Discord

```bash
npm install
HUB_DEV_LOGIN=1 npm start -w @cbmp/hub
```
Abra http://localhost:8787/panel → "Entrar como Dev". (PowerShell: `$env:HUB_DEV_LOGIN='1'; npm start -w @cbmp/hub`)

## Rodar com Discord

1. Crie um app em https://discord.com/developers/applications → OAuth2.
2. Adicione o redirect `<HUB_PUBLIC_URL>/v1/auth/discord/callback`.
3. Copie `.env.example` para `.env`, preencha `DISCORD_CLIENT_ID`/`DISCORD_CLIENT_SECRET` e exporte as variáveis antes de `npm start -w @cbmp/hub`.

## Chaves de assinatura

Ficam em `<HUB_DATA_DIR>/keys/<kid>.pem`. A de maior nome é a ativa. Para girar: crie um novo `.pem` com kid maior
(`k` + AAAAMMDDhhmmss) e reinicie; remova a antiga depois de 10 minutos (tickets duram 5 min).

## Testes

```bash
npm test
```
````

Adicionar `hub/data/` ao `.gitignore`.

- [ ] **Step 4: Testes, typecheck e smoke manual**

Run: `npm test && npm run typecheck`
Expected: contratos e Hub todos PASS; typecheck sem erros.

Smoke (PowerShell, raiz do repo):
```powershell
$env:HUB_DEV_LOGIN='1'; $env:HUB_DATA_DIR="$env:TEMP\cbmp-hub-smoke"; Start-Job { Set-Location $using:PWD; npm start -w @cbmp/hub }
```
Depois:
- `curl http://localhost:8787/health` → `{"ok":true}`
- `curl http://localhost:8787/v1/keys/public` → lista com 1 chave
- `curl http://localhost:8787/v1/servers` → `[]`
Parar o job (`Get-Job | Stop-Job | Remove-Job`) e apagar a pasta temporária.

- [ ] **Step 5: Commit**

```bash
git add hub .gitignore
git commit -m "feat(hub): painel web de donos, processo principal e README"
```

---

## Fim do subprojeto 1

Critério de pronto (spec): login Discord no painel; criar servidor e girar chave; heartbeat de teste aparece em `/v1/servers` e some após 90 s; ticket emitido valida com `@cbmp/contracts`; vetores gerados; testes verdes. Login Discord real exige criar o app no Discord (passo manual do Robson); sem isso, `HUB_DEV_LOGIN=1` cobre o fluxo local.
