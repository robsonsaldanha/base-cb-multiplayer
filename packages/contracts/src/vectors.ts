import { sign } from 'node:crypto';
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
  const sigBytes = Buffer.from(goodSig!, 'base64url');
  const sign64 = (p: object) => signTicket(p as TicketPayload, priv);
  const { jti: _jti, ...semJti } = payload;
  const lastIdx = goodSig!.length - 1;
  const alt = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
  // 64 bytes => último char carrega só 4 bits úteis; outro char com mesmos 4 bits altos decodifica igual
  const lastVal = alt.indexOf(goodSig![lastIdx]!);
  const altChar = alt[lastVal ^ 1]!;
  const nonCanonical = good.slice(0, good.length - 1) + altChar;
  const b64 = (o: string) => Buffer.from(o, 'utf8').toString('base64url');
  let badLenBody = good.split('.')[0]!;
  while (badLenBody.length % 4 !== 1) badLenBody += 'A';
  const floatBody = Buffer.from(JSON.stringify(payload).replace('"iat":1800000000', '"iat":1800000000.0'), 'utf8');
  const floatTicket = floatBody.toString('base64url') + '.' + sign(null, floatBody, priv).toString('base64url');
  const at = (name: string, ticket: string, expected: 'ok' | TicketError, extra: Partial<TicketVectorCase> = {}): TicketVectorCase =>
    ({ name, ticket, audience: 'srv_test', nowSeconds: payload.iat, expected, ...extra });
  return {
    description: 'Vetores de ticket CB-MP. Ordem das checagens: formato/charset, base64url canônico, JSON, forma (incluindo inteiros seguros; v, iat e exp devem ser literais inteiros simples no JSON: sem 1.0, sem expoente), versão, kid, assinatura (64 bytes + verificação), audience, iat > now+60 (not_yet_valid), exp+60 < now (expired).',
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
      at('limite_futuro', good, 'ok', { nowSeconds: payload.iat - 60 }),
      at('assinatura_curta', good.split('.')[0] + '.' + sigBytes.subarray(0, 63).toString('base64url'), 'bad_signature'),
      at('json_invalido_b64_valido', b64('{"v":1') + '.' + goodSig, 'malformed'),
      at('forma_invalida', sign64(semJti), 'malformed'),
      at('iat_nao_inteiro', sign64({ ...payload, iat: 1.5 }), 'malformed'),
      at('ordem_aud_e_expirado', good, 'wrong_audience', { audience: 'srv_outro', nowSeconds: payload.exp + 100 }),
      at('ordem_versao_e_kid', sign64({ ...payload, v: 2, kid: 'test-2' }), 'bad_version'),
      at('b64_nao_canonico', nonCanonical, 'malformed'),
      at('b64_tamanho_invalido', badLenBody + '.' + goodSig, 'malformed'),
      at('iat_literal_float', floatTicket, 'malformed'),
    ],
  };
}
