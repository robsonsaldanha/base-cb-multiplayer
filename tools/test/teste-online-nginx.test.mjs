// nginx_apply de tools/teste-online/remote-install.sh: com `nginx -t` falhando, o site volta ao estado anterior
// (arquivo e link) e nada é recarregado. Roda o script de verdade com `sudo`, `nginx` e `systemctl` falsos no PATH.
// Precisa de bash e de links simbólicos de verdade (Linux; no Windows só com symlinks nativos, senão é pulado).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, lstatSync, mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync, chmodSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT = resolve(dirname(fileURLToPath(import.meta.url)), '..', 'teste-online', 'remote-install.sh');
const HOST = 'cbmultiplayer.cntoficial.com.br';
const toBash = (p) => (process.platform === 'win32' ? p.replace(/\\/g, '/').replace(/^([A-Za-z]):/, (_, d) => `/${d.toLowerCase()}`) : p);

function canSymlink() {
  const d = mkdtempSync(join(tmpdir(), 'cbmp-ln-'));
  try {
    writeFileSync(join(d, 'a'), 'x');
    const r = spawnSync('bash', ['-c', `cd "${toBash(d)}" && ln -s a b && [ -L b ]`]);
    return r.status === 0;
  } catch { return false; } finally { rmSync(d, { recursive: true, force: true }); }
}

function sandbox() {
  const d = mkdtempSync(join(tmpdir(), 'cbmp-nginx-'));
  const bin = join(d, 'bin');
  const avail = join(d, 'sites-available');
  const enabled = join(d, 'sites-enabled');
  const confd = join(d, 'conf.d');
  for (const x of [bin, avail, enabled, confd]) mkdirSync(x);
  const calls = join(d, 'calls.log');
  writeFileSync(join(bin, 'sudo'), '#!/usr/bin/env bash\n[ "$1" = "-n" ] && shift\nexec "$@"\n');
  // nginx -t falso: falha se algum site habilitado tiver "BROKEN".
  writeFileSync(join(bin, 'nginx'), `#!/usr/bin/env bash\nfor f in "$CBMP_NGINX_ENABLED"/*; do [ -e "$f" ] && grep -q BROKEN "$f" && exit 1; done\nexit 0\n`);
  writeFileSync(join(bin, 'systemctl'), `#!/usr/bin/env bash\necho "systemctl $*" >> "${toBash(calls)}"\n`);
  for (const f of ['sudo', 'nginx', 'systemctl']) chmodSync(join(bin, f), 0o755);
  const env = {
    ...process.env,
    PATH: `${toBash(bin)}:${process.env.PATH}`,
    CBMP_NGINX_AVAILABLE: toBash(avail), CBMP_NGINX_ENABLED: toBash(enabled), CBMP_NGINX_CONFD: toBash(confd), CBMP_NGINX_OWNER: '',
  };
  const apply = (file) => spawnSync('bash', [toBash(SCRIPT), 'nginx-apply', file ? toBash(file) : ''], { env, encoding: 'utf8' });
  const conf = (name, body) => { const p = join(d, name); writeFileSync(p, body); return p; };
  const reloads = () => (existsSync(calls) ? readFileSync(calls, 'utf8').split('\n').filter((l) => l.includes('reload')).length : 0);
  const site = join(avail, HOST);
  const link = join(enabled, HOST);
  const isLink = () => { try { return lstatSync(link).isSymbolicLink(); } catch { return false; } };
  return { d, apply, conf, reloads, site, link, isLink, enabled, cleanup: () => rmSync(d, { recursive: true, force: true }) };
}

const skip = !canSymlink() && 'sem links simbólicos de verdade neste sistema';

test('nginx_apply: instala, recusa config quebrada restaurando a anterior e nunca recarrega com erro', { skip }, () => {
  const s = sandbox();
  try {
    const good = s.conf('good.conf', `server { server_name ${HOST}; } # v1\n`);
    let r = s.apply(good);
    assert.equal(r.status, 0, r.stderr);
    assert.equal(readFileSync(s.site, 'utf8'), readFileSync(good, 'utf8'));
    assert.ok(s.isLink());
    assert.equal(s.reloads(), 1);

    // Atualização quebrada: falha, volta para o v1, sem reload.
    const bad = s.conf('bad.conf', `server { server_name ${HOST}; BROKEN }\n`);
    r = s.apply(bad);
    assert.notEqual(r.status, 0);
    assert.match(r.stderr, /restaurado/);
    assert.equal(readFileSync(s.site, 'utf8'), readFileSync(good, 'utf8'));
    assert.ok(s.isLink());
    assert.equal(s.reloads(), 1);

    // Remoção: some arquivo e link, com reload.
    r = s.apply('');
    assert.equal(r.status, 0, r.stderr);
    assert.ok(!existsSync(s.site) && !s.isLink());
    assert.equal(s.reloads(), 2);

    // Primeira instalação já quebrada: não sobra nada habilitado.
    r = s.apply(bad);
    assert.notEqual(r.status, 0);
    assert.ok(!existsSync(s.site) && !s.isLink());
    assert.equal(s.reloads(), 2);
  } finally { s.cleanup(); }
});

test('nginx_apply: recusa se outro site já usa o nome, sem mexer em nada', { skip }, () => {
  const s = sandbox();
  try {
    const other = join(s.d, 'outro');
    writeFileSync(other, `server { server_name ${HOST}; }\n`);
    symlinkSync(other, join(s.enabled, 'outro'));
    const r = s.apply(s.conf('good.conf', `server { server_name ${HOST}; }\n`));
    assert.notEqual(r.status, 0);
    assert.match(r.stderr, /outro site nginx/);
    assert.ok(!existsSync(s.site) && !s.isLink());
    assert.equal(s.reloads(), 0);
  } finally { s.cleanup(); }
});

test('nginx_apply: config compartilhada já quebrada por outro site → nada é recarregado', { skip }, () => {
  const s = sandbox();
  try {
    const other = join(s.d, 'outro-quebrado');
    writeFileSync(other, 'server { server_name outro.example; BROKEN }\n');
    symlinkSync(other, join(s.enabled, 'outro-quebrado'));
    const r = s.apply(s.conf('good.conf', `server { server_name ${HOST}; }\n`));
    assert.notEqual(r.status, 0);
    assert.match(r.stderr, /não é deste site/);
    assert.ok(!existsSync(s.site) && !s.isLink());
    assert.equal(s.reloads(), 0);
  } finally { s.cleanup(); }
});
