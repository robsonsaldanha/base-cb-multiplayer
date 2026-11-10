# Avisos de terceiros (cb-server, cb-bots)

Os executáveis nativos (`cb-server`, `cb-bots`) usam as bibliotecas abaixo, instaladas pelo vcpkg (`vcpkg.json`,
baseline `ec48c11e…`; Lua pelo overlay `vcpkg-overlays/lua`). Quem distribuir um binário precisa levar **este arquivo
e o texto integral de cada licença**: o vcpkg grava cada um em `<vcpkg_installed>/<triplet>/share/<port>/copyright`
(copie a pasta inteira, ou pelo menos os ports desta tabela, para `licenses/<port>/` do pacote).

Ao trocar a baseline ou acrescentar uma dependência, confira a lista com `vcpkg list` e atualize esta tabela.
Catch2 (BSL-1.0) só entra nos executáveis de teste, que não são distribuídos.

| Biblioteca | Licença | Aviso de copyright | Ligação |
|---|---|---|---|
| Lua 5.4.7 (com patches do projeto, compilado como C++) | MIT | Copyright © 1994–2024 Lua.org, PUC-Rio | estática (Linux) / `lua-c++.dll` (Windows) |
| sol2 3.5 | MIT | Copyright (c) 2013-2022 Rapptz, ThePhD, and contributors | só cabeçalhos |
| MariaDB Connector/C (`libmariadb`) | **LGPL-2.1-or-later** | Copyright (C) MariaDB Corporation e contribuidores; ver `share/libmariadb/copyright` | **dinâmica**: `libmariadb.so*` / `libmariadb.dll` + `plugins/libmariadb/` |
| OpenSSL 3 | Apache-2.0 | Copyright The OpenSSL Project Authors (ver `share/openssl/copyright`) | `libssl`/`libcrypto` (dinâmica no Windows, estática no Linux) |
| cpp-httplib | MIT | Copyright (c) 2017 yhirose | só cabeçalhos |
| nlohmann/json | MIT | Copyright (c) 2013-2025 Niels Lohmann | só cabeçalhos |
| spdlog | MIT | Copyright (c) 2016 - present, Gabi Melman and spdlog contributors | estática / `spdlog.dll` |
| {fmt} (dependência do spdlog) | MIT | Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors | estática / `fmt.dll` |
| libsodium | ISC | Copyright (c) 2013-2026 Frank Denis | estática / `libsodium.dll` |
| GameNetworkingSockets | BSD-3-Clause | Copyright (c) 2018, Valve Corporation | estática / `GameNetworkingSockets.dll` |
| Protocol Buffers (dependência do GNS) | BSD-3-Clause | Copyright 2008 Google Inc. | estática / `libprotobuf*.dll` |
| Abseil (dependência do protobuf) | Apache-2.0 | Copyright The Abseil Authors (ver `share/abseil/copyright`) | estática / `abseil_dll.dll` |
| utf8_range (dependência do protobuf) | MIT | Copyright (c) 2019 Yibo Cai; Copyright 2022 Google LLC | estática |
| Brotli (cpp-httplib) | MIT | Copyright (c) 2009, 2010, 2013-2016 by the Brotli Authors | estática / `brotli*.dll` |
| zlib (cpp-httplib) | zlib | (C) 1995-2026 Jean-loup Gailly and Mark Adler | estática / `z.dll` |

## Apache-2.0 (OpenSSL, Abseil)

A Apache-2.0 exige levar uma cópia da licença e, se o projeto tiver arquivo `NOTICE`, o conteúdo dele. O texto vem em
`share/openssl/copyright` e `share/abseil/copyright`; inclua também o `NOTICE` do OpenSSL se a versão distribuída
tiver um.

## MIT, BSD, ISC e zlib

Exigem manter o aviso de copyright e o texto da licença junto do binário (BSD-3-Clause: também não usar os nomes dos
autores para promover o produto). Os arquivos `share/<port>/copyright` têm o texto exato de cada um.

## MariaDB Connector/C (LGPL-2.1) — ligação dinâmica e reposição

- O `cb-server` liga a `libmariadb` **dinamicamente** nos dois sistemas (no Linux pelo triplet
  `vcpkg-triplets/x64-linux.cmake`, que abre exceção só para ela; o CI confere com `ldd`). Nenhum código da
  biblioteca é copiado para o executável.
- Quem recebe o pacote pode **trocar a biblioteca** por outra build compatível do MariaDB Connector/C (mesma ABI
  `libmariadb.so.3` / `libmariadb.dll`): basta substituir o arquivo ao lado do executável (no Linux o RPATH do
  `cb-server` aponta para a pasta dele; `LD_LIBRARY_PATH` também funciona). Os plugins de autenticação ficam em
  `plugins/libmariadb/` e podem ser trocados do mesmo jeito.
- O pacote precisa levar o texto da LGPL-2.1 (`share/libmariadb/copyright`) e informar onde obter o código-fonte da
  versão distribuída (a versão está em `vcpkg list`; fonte em https://github.com/mariadb-corporation/mariadb-connector-c).
- Os patches que o projeto aplica valem só para o Lua; a `libmariadb` é usada sem modificação.

## Lua (patches do projeto)

O Lua é usado com três patches pequenos (`vcpkg-overlays/lua/cbmp-*.patch`, descritos em `vcpkg-overlays/README.md`).
A licença MIT do Lua permite isso; o aviso acima e o texto em `share/lua/copyright` continuam obrigatórios.
