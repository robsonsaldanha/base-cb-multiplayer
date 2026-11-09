import { mkdirSync, writeFileSync } from 'node:fs';
import { buildVectors } from '../src/vectors';

const dir = new URL('../test-vectors/', import.meta.url);
mkdirSync(dir, { recursive: true });
writeFileSync(new URL('tickets.json', dir), JSON.stringify(buildVectors(), null, 2) + '\n');
console.log('test-vectors/tickets.json gerado');
