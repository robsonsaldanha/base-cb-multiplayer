// package.json + package-lock.json mínimos do Hub empacotado (ambiente de teste online, tools/teste-online.mjs): só
// better-sqlite3 e as dependências de produção dele, com as MESMAS versões, URLs e hashes de integridade do
// package-lock.json do repositório. Na VPS: `npm ci --omit=dev --ignore-scripts`.

/** @param {any} repoLock conteúdo do package-lock.json do repositório (lockfileVersion 3) */
export function hubLockfile(repoLock, root = 'better-sqlite3') {
  const packages = {};
  const visit = (name) => {
    const key = `node_modules/${name}`;
    if (packages[key]) return;
    const e = repoLock?.packages?.[key];
    if (!e?.version || !e.resolved || !e.integrity) throw new Error(`package-lock.json sem ${key} (version/resolved/integrity)`);
    if (!/^sha512-/.test(e.integrity)) throw new Error(`${key}: integridade sem sha512`);
    const { dev, devOptional, peer, ...entry } = e;
    void dev; void devOptional; void peer;
    packages[key] = entry;
    for (const d of Object.keys(e.dependencies ?? {})) visit(d);
  };
  visit(root);
  const version = packages[`node_modules/${root}`].version;
  const pkg = { name: 'cbmp-hub-teste', private: true, type: 'module', dependencies: { [root]: version } };
  const lock = { name: pkg.name, lockfileVersion: 3, requires: true, packages: { '': { name: pkg.name, dependencies: pkg.dependencies }, ...packages } };
  return { pkg, lock };
}
