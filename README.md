# Base CB Multiplayer

Plataforma de multiplayer para Hogwarts Legacy no estilo FiveM: Hub, servidores dedicados e launcher.

## Estrutura

- `packages/contracts`: contratos compartilhados (tickets, mensagens) em TypeScript.
- `hub/`: API e painel de donos de servidores (Node); veja `hub/README.md`.
- `launcher/`: launcher Windows "CB Multiplayer" (Tauri 2 + Svelte 5); veja `launcher/README.md`.
- `client/`: mod spike do cliente.
- `proto/`: protocolo binário em C++ (`cbmp`), com vetores compartilhados com o TypeScript.
- `server/`: `cb-server`, o servidor dedicado; veja `server/README.md`.
- `bots/`: `cb-bots`, bots de carga e segurança.
- `tools/loadtest.py`: teste de carga e segurança; veja `docs/LOAD-TEST.md`.

## Nativo

Código C++ (`proto`, `server`, `bots`) compilado com CMake + vcpkg; presets em `CMakePresets.json`
(`linux-release`, `linux-asan`, `linux-fuzz`, `windows-release`, `windows-ci`).

```bash
export VCPKG_ROOT=~/vcpkg      # commit fixado em .github/workflows/native.yml
cmake --preset linux-release && cmake --build --preset linux-release
ctest --test-dir build-native/linux-release --output-on-failure
```

No Windows local use um caminho curto para os pacotes: `-DVCPKG_INSTALLED_DIR=C:/cbmp-vi`.
O workflow `native` roda build Linux (ASan) e Windows, testes, fuzz de 120 s e carga de 256 bots. Antes de produção,
rode o teste de 512 bots descrito em `docs/LOAD-TEST.md`.
