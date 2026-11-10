// Ponto de entrada de `npm run tauri`. A CLI do Tauri só procura `tauri.conf.json` abaixo da pasta atual, e o
// `src-tauri` é irmão desta pasta (`launcher/src-tauri`); `TAURI_APP_PATH` aponta para ele sem depender do shell.
import { fileURLToPath } from "node:url";
import cli from "@tauri-apps/cli";

process.env.TAURI_APP_PATH ??= fileURLToPath(new URL("../src-tauri", import.meta.url));

cli.run(process.argv.slice(2), "npm run tauri").catch((err) => {
  cli.logError(err.message);
  process.exit(1);
});
