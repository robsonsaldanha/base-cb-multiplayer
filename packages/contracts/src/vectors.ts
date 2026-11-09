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
