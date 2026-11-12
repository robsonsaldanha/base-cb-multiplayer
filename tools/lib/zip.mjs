// Leitura e escrita mínimas de zip (sem dependências): `readZip` lê stored e deflate pelo diretório central;
// `writeStoredZip` grava um zip sem compressão (usado pelos testes). Recusa os mesmos caminhos que o launcher
// (`..`, absoluto, `\`, `:`).
import { inflateRawSync } from 'node:zlib';

const EOCD = 0x06054b50;
const CEN = 0x02014b50;
const LOC = 0x04034b50;

let crcTable;
export function crc32(buf) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let c = 0xffffffff;
  for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/** Motivo da recusa do caminho, ou null se for seguro. */
export function unsafePathReason(name) {
  if (name === '') return 'caminho vazio';
  if (name.includes('\\')) return 'contém barra invertida';
  if (name.includes(':')) return 'contém ":"';
  if (name.startsWith('/')) return 'caminho absoluto';
  if (name.includes('\0')) return 'contém NUL';
  const parts = name.replace(/\/$/, '').split('/');
  if (parts.some((p) => p === '..')) return 'contém ".."';
  if (parts.some((p) => p === '' || p === '.')) return 'segmento vazio ou "."';
  return null;
}

/** @returns {Map<string, Buffer>} nome → conteúdo (diretórios são ignorados). */
export function readZip(buffer) {
  let eocd = -1;
  for (let i = buffer.length - 22; i >= Math.max(0, buffer.length - 22 - 0xffff); i--) {
    if (buffer.readUInt32LE(i) === EOCD) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('não é um zip (diretório central não encontrado)');
  const total = buffer.readUInt16LE(eocd + 10);
  let p = buffer.readUInt32LE(eocd + 16);
  const out = new Map();
  for (let n = 0; n < total; n++) {
    if (p + 46 > buffer.length || buffer.readUInt32LE(p) !== CEN) throw new Error('diretório central corrompido');
    const flags = buffer.readUInt16LE(p + 8);
    const method = buffer.readUInt16LE(p + 10);
    const crc = buffer.readUInt32LE(p + 16);
    const csize = buffer.readUInt32LE(p + 20);
    const usize = buffer.readUInt32LE(p + 24);
    const nlen = buffer.readUInt16LE(p + 28);
    const xlen = buffer.readUInt16LE(p + 30);
    const clen = buffer.readUInt16LE(p + 32);
    const loff = buffer.readUInt32LE(p + 42);
    const name = buffer.toString('utf8', p + 46, p + 46 + nlen);
    p += 46 + nlen + xlen + clen;
    const why = unsafePathReason(name);
    if (why) throw new Error(`caminho inseguro no zip "${name}": ${why}`);
    if (name.endsWith('/')) continue;
    if (flags & 1) throw new Error(`"${name}": zip cifrado`);
    if (out.has(name)) throw new Error(`entrada repetida no zip: ${name}`);
    if (loff + 30 > buffer.length || buffer.readUInt32LE(loff) !== LOC) throw new Error(`"${name}": cabeçalho local inválido`);
    const start = loff + 30 + buffer.readUInt16LE(loff + 26) + buffer.readUInt16LE(loff + 28);
    const raw = buffer.subarray(start, start + csize);
    let data;
    if (method === 0) data = Buffer.from(raw);
    else if (method === 8) data = inflateRawSync(raw);
    else throw new Error(`"${name}": método de compressão ${method} não suportado`);
    if (data.length !== usize || crc32(data) !== crc) throw new Error(`"${name}": tamanho ou CRC não confere`);
    out.set(name, data);
  }
  return out;
}

/** @param {Array<[string, Buffer|string]>|Map<string, Buffer|string>} entries */
export function writeStoredZip(entries) {
  const list = [...(entries instanceof Map ? entries.entries() : entries)];
  const locals = [];
  const centrals = [];
  let offset = 0;
  for (const [name, content] of list) {
    const data = Buffer.isBuffer(content) ? content : Buffer.from(content);
    const nameBuf = Buffer.from(name, 'utf8');
    const crc = crc32(data);
    const loc = Buffer.alloc(30);
    loc.writeUInt32LE(LOC, 0); loc.writeUInt16LE(20, 4); loc.writeUInt16LE(0x0800, 6);
    loc.writeUInt32LE(crc, 14); loc.writeUInt32LE(data.length, 18); loc.writeUInt32LE(data.length, 22);
    loc.writeUInt16LE(nameBuf.length, 26);
    const cen = Buffer.alloc(46);
    cen.writeUInt32LE(CEN, 0); cen.writeUInt16LE(20, 4); cen.writeUInt16LE(20, 6); cen.writeUInt16LE(0x0800, 8);
    cen.writeUInt32LE(crc, 16); cen.writeUInt32LE(data.length, 20); cen.writeUInt32LE(data.length, 24);
    cen.writeUInt16LE(nameBuf.length, 28); cen.writeUInt32LE(offset, 42);
    locals.push(loc, nameBuf, data);
    centrals.push(cen, nameBuf);
    offset += 30 + nameBuf.length + data.length;
  }
  const cd = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(EOCD, 0);
  end.writeUInt16LE(list.length, 8); end.writeUInt16LE(list.length, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, cd, end]);
}
