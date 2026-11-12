import { test } from 'node:test';
import assert from 'node:assert/strict';
import { checkModZip } from '../check-mod-zip.mjs';
import { readZip, writeStoredZip } from '../lib/zip.mjs';

const dll = () => Buffer.concat([Buffer.from('MZ'), Buffer.alloc(8192, 1)]);
const INI = '[General]\r\nConsoleEnabled = 0\r\nGuiConsoleEnabled = 0\r\nGuiConsoleVisible = 0\r\nEnableHotReloadSystem = 0\r\n\r\n[CrashDump]\r\nEnableDumping = 0\r\n\r\n[Hooks]\r\nHookLoadMap = 1\r\n';
const META = { layout: 2, version: '0.2.0', protocol: 1, ue4ss: '3.0.1+4fb9b615', minLauncher: '0.2.0' };

function good() {
  return new Map([
    ['cbmp-mod.json', JSON.stringify(META)],
    ['loader/dwmapi.dll', dll()],
    ['ue4ss/UE4SS.dll', dll()],
    ['ue4ss/UE4SS-settings.ini', INI],
    ['ue4ss/Mods/mods.txt', 'cb-mp : 1\r\nKeybinds : 0\r\n'],
    ['ue4ss/Mods/cb-mp/enabled.txt', ''],
    ['ue4ss/Mods/cb-mp/dlls/main.dll', dll()],
    ['licenses/UE4SS-LICENSE.txt', 'MIT License ...'],
    ['licenses/THIRD-PARTY-NOTICES.txt', 'GameNetworkingSockets protobuf abseil OpenSSL nlohmann-json'],
    ['LEIA-ME.txt', 'CB Multiplayer'],
  ]);
}
const check = (m, opts = { version: '0.2.0', protocol: 1 }) => checkModZip(writeStoredZip(m), opts);
const has = (problems, text) => assert.ok(problems.some((p) => p.includes(text)), `esperava "${text}" em ${JSON.stringify(problems)}`);

test('zip bom não tem problemas (e o readZip lê o que o writeStoredZip grava)', () => {
  assert.deepEqual(check(good()), []);
  assert.equal(readZip(writeStoredZip(good())).get('LEIA-ME.txt').toString(), 'CB Multiplayer');
});

test('sem --version/--protocol confere só o conteúdo', () => {
  assert.deepEqual(check(good(), {}), []);
});

test('arquivo faltando', () => {
  const m = good(); m.delete('LEIA-ME.txt');
  has(check(m), 'LEIA-ME.txt');
});

test('arquivo a mais', () => {
  const m = good(); m.set('ue4ss/Mods/Keybinds/enabled.txt', '');
  has(check(m), 'Keybinds');
});

test('layout 1', () => {
  const m = good(); m.set('cbmp-mod.json', JSON.stringify({ ...META, layout: 1 }));
  has(check(m), 'layout');
});

test('versão diferente de --version', () => {
  has(check(good(), { version: '0.2.1', protocol: 1 }), 'version');
});

test('protocolo diferente de --protocol', () => {
  has(check(good(), { version: '0.2.0', protocol: 2 }), 'protocol');
});

test('ue4ss e minLauncher exatos', () => {
  const m = good(); m.set('cbmp-mod.json', JSON.stringify({ ...META, ue4ss: '3.0.0', minLauncher: '0.1.0' }));
  const p = check(m);
  has(p, 'ue4ss'); has(p, 'minLauncher');
});

test('cbmp-mod.json com campo extra', () => {
  const m = good(); m.set('cbmp-mod.json', JSON.stringify({ ...META, extra: 1 }));
  has(check(m), 'extra');
});

test('ini com GuiConsoleEnabled=1', () => {
  const m = good(); m.set('ue4ss/UE4SS-settings.ini', INI.replace('GuiConsoleEnabled = 0', 'GuiConsoleEnabled = 1'));
  has(check(m), 'GuiConsoleEnabled');
});

test('ini sem [Hooks]', () => {
  const m = good(); m.set('ue4ss/UE4SS-settings.ini', INI.replace('[Hooks]', '[Other]'));
  has(check(m), '[Hooks]');
});

test('mods.txt sem Keybinds : 0', () => {
  const m = good(); m.set('ue4ss/Mods/mods.txt', 'cb-mp : 1\r\n');
  has(check(m), 'Keybinds : 0');
});

test('licença do UE4SS vazia', () => {
  const m = good(); m.set('licenses/UE4SS-LICENSE.txt', '');
  has(check(m), 'UE4SS-LICENSE');
});

test('terceiros sem OpenSSL', () => {
  const m = good(); m.set('licenses/THIRD-PARTY-NOTICES.txt', 'GameNetworkingSockets protobuf abseil nlohmann-json');
  has(check(m), 'OpenSSL');
});

test('DLL sem MZ', () => {
  const m = good(); m.set('ue4ss/UE4SS.dll', Buffer.alloc(8192, 1));
  has(check(m), 'MZ');
});

test('DLL pequena demais', () => {
  const m = good(); m.set('loader/dwmapi.dll', Buffer.from('MZ'));
  has(check(m), '4 KiB');
});

test('LEIA-ME vazio', () => {
  const m = good(); m.set('LEIA-ME.txt', '');
  has(check(m), 'LEIA-ME.txt');
});

test('caminho inseguro ../x', () => {
  const m = good(); m.set('../x', 'x');
  has(check(m), '..');
});

test('não é um zip', () => {
  has(checkModZip(Buffer.from('nada'), {}), 'zip');
});
