// Conferência do zip do mod (layout 2), usada por tools/check-mod-zip.mjs, teste-online.mjs e dev-env.mjs.
import { readZip } from './zip.mjs';

export const EXPECTED_FILES = [
  'cbmp-mod.json',
  'loader/dwmapi.dll',
  'ue4ss/UE4SS.dll',
  'ue4ss/UE4SS-settings.ini',
  'ue4ss/Mods/mods.txt',
  'ue4ss/Mods/cb-mp/enabled.txt',
  'ue4ss/Mods/cb-mp/dlls/main.dll',
  'licenses/UE4SS-LICENSE.txt',
  'licenses/THIRD-PARTY-NOTICES.txt',
  'LEIA-ME.txt',
];
const DLLS = ['loader/dwmapi.dll', 'ue4ss/UE4SS.dll', 'ue4ss/Mods/cb-mp/dlls/main.dll'];
const INI_ZERO_KEYS = ['ConsoleEnabled', 'GuiConsoleEnabled', 'GuiConsoleVisible', 'EnableHotReloadSystem', 'EnableDumping'];
const THIRD_PARTY = ['GameNetworkingSockets', 'protobuf', 'abseil', 'OpenSSL', 'nlohmann-json'];
export const UE4SS_ID = '3.0.1+4fb9b615';
export const MIN_LAUNCHER = '0.2.0';
const META_KEYS = ['layout', 'version', 'protocol', 'ue4ss', 'minLauncher'];

/** @returns {string[]} problemas (vazio = ok) */
export function checkModZip(buffer, { version, protocol } = {}) {
  let files;
  try { files = readZip(buffer); } catch (e) { return [`zip inválido: ${e.message}`]; }
  const problems = [];
  const names = [...files.keys()];
  for (const n of EXPECTED_FILES) if (!files.has(n)) problems.push(`falta ${n}`);
  for (const n of names) if (!EXPECTED_FILES.includes(n)) problems.push(`arquivo a mais: ${n}`);

  const text = (n) => (files.has(n) ? files.get(n).toString('utf8') : null);

  const metaText = text('cbmp-mod.json');
  if (metaText !== null) {
    let meta = null;
    try { meta = JSON.parse(metaText.replace(/^﻿/, '')); } catch { problems.push('cbmp-mod.json não é JSON válido'); }
    if (metaText.charCodeAt(0) === 0xfeff) problems.push('cbmp-mod.json com BOM');
    if (meta && typeof meta === 'object') {
      if (meta.layout !== 2) problems.push(`cbmp-mod.json: layout ${JSON.stringify(meta.layout)}, esperado 2`);
      if (typeof meta.version !== 'string' || !/^\d+\.\d+\.\d+$/.test(meta.version)) problems.push(`cbmp-mod.json: version inválida ${JSON.stringify(meta.version)}`);
      if (!Number.isInteger(meta.protocol)) problems.push(`cbmp-mod.json: protocol inválido ${JSON.stringify(meta.protocol)}`);
      if (meta.ue4ss !== UE4SS_ID) problems.push(`cbmp-mod.json: ue4ss ${JSON.stringify(meta.ue4ss)}, esperado "${UE4SS_ID}"`);
      if (meta.minLauncher !== MIN_LAUNCHER) problems.push(`cbmp-mod.json: minLauncher ${JSON.stringify(meta.minLauncher)}, esperado "${MIN_LAUNCHER}"`);
      for (const k of Object.keys(meta)) if (!META_KEYS.includes(k)) problems.push(`cbmp-mod.json: campo inesperado ${k}`);
      if (version !== undefined && meta.version !== version) problems.push(`cbmp-mod.json: version ${JSON.stringify(meta.version)}, esperado ${version}`);
      if (protocol !== undefined && meta.protocol !== Number(protocol)) problems.push(`cbmp-mod.json: protocol ${JSON.stringify(meta.protocol)}, esperado ${protocol}`);
    } else if (meta !== null) problems.push('cbmp-mod.json não é um objeto');
  }

  for (const n of DLLS) {
    const d = files.get(n);
    if (!d) continue;
    if (d.length <= 4096) problems.push(`${n}: ${d.length} bytes (precisa de mais de 4 KiB)`);
    if (d.length < 2 || d[0] !== 0x4d || d[1] !== 0x5a) problems.push(`${n}: não começa com MZ`);
  }

  const ini = text('ue4ss/UE4SS-settings.ini');
  if (ini !== null) {
    for (const k of INI_ZERO_KEYS) {
      const m = ini.match(new RegExp(`^[ \\t]*${k}[ \\t]*=[ \\t]*([^\\s;#]*)`, 'm'));
      if (!m) problems.push(`UE4SS-settings.ini: falta a chave ${k}`);
      else if (m[1] !== '0') problems.push(`UE4SS-settings.ini: ${k}=${m[1]}, esperado 0`);
    }
    if (!/^\s*\[Hooks\]\s*$/m.test(ini)) problems.push('UE4SS-settings.ini: falta a seção [Hooks]');
  }

  const mods = text('ue4ss/Mods/mods.txt');
  if (mods !== null) {
    const ls = mods.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
    for (const want of ['cb-mp : 1', 'Keybinds : 0']) if (!ls.includes(want)) problems.push(`mods.txt: falta a linha "${want}"`);
    for (const l of ls) if (!['cb-mp : 1', 'Keybinds : 0'].includes(l)) problems.push(`mods.txt: linha inesperada "${l}"`);
  }

  const lic = text('licenses/UE4SS-LICENSE.txt');
  if (lic !== null && lic.trim() === '') problems.push('licenses/UE4SS-LICENSE.txt vazio');
  const third = text('licenses/THIRD-PARTY-NOTICES.txt');
  if (third !== null) {
    if (third.trim() === '') problems.push('licenses/THIRD-PARTY-NOTICES.txt vazio');
    else for (const n of THIRD_PARTY) if (!third.includes(n)) problems.push(`THIRD-PARTY-NOTICES.txt não cita ${n}`);
  }
  const readme = text('LEIA-ME.txt');
  if (readme !== null && readme.trim() === '') problems.push('LEIA-ME.txt vazio');
  return problems;
}
