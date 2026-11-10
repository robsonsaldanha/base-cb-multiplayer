# vcpkg-triplets

Triplets locais que substituem os do vcpkg (`VCPKG_OVERLAY_TRIPLETS` no preset `base` de `CMakePresets.json`).

## x64-linux

Igual ao `x64-linux` oficial (bibliotecas estáticas), exceto a `libmariadb` (MariaDB Connector/C, LGPL-2.1), que
é construída como `libmariadb.so` para o `cb-server` ligar com ela dinamicamente (exigência da LGPL para não
distribuir o binário com o cliente estático). No Windows, `x64-windows` já é dinâmico (`libmariadb.dll`).

Na distribuição do servidor Linux, `libmariadb.so*` vai junto do executável (o vcpkg grava RPATH relativo com
`VCPKG_FIXUP_ELF_RPATH`; o build tree acha a biblioteca pelo RPATH que o CMake grava). Os plugins de autenticação
dinâmicos da libmariadb (`plugins/libmariadb/*.so`, ex. `caching_sha2_password`) só são necessários para MySQL 8
com esse método; `mysql_native_password` (MariaDB) é embutido.
