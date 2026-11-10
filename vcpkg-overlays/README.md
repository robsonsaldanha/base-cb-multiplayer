# vcpkg-overlays

Ports locais que substituem os do registro do vcpkg. Ativados por `VCPKG_OVERLAY_PORTS` no preset `base`
(`CMakePresets.json`), então valem para todos os presets locais e do CI.

## lua (5.4.7 + patches do sandbox)

Cópia do port oficial `lua` 5.4.7 (árvore `97783a4c337df419c0a6a75e9599545c54259d86` do microsoft/vcpkg, a mesma da
baseline `ec48c11e…`) com três patches pequenos usados pela vigia dos scripts (`server/script/src/resource.cpp`):

| Patch | Arquivo | O quê |
|---|---|---|
| `cbmp-c1-closethread-allowhook.patch` | `lstate.c` (`luaE_resetthread`) | Religa hooks (`allowhook = 1`) antes de rodar `__close` pendentes de uma thread fechada. Desde o round 3 o sandbox não usa hooks; mantido por segurança caso hooks voltem. |
| `cbmp-c2-vm-interrupt.patch` | `lstate.h`, `lstate.c`, `lapi.c`, `lua.h`, `lvm.c`, `ldebug.c` | Pedido de interrupção vindo de outra thread do SO: `global_State.cbmp_interrupt` (`volatile sig_atomic_t`), gravado só por `lua_cbmp_setinterrupt(L, on)`. A VM junta a flag ao `trap` (`updatetrap` e entrada de função), então qualquer laço Lua a vê no próximo salto; em `luaG_traceexec` chama o callback do host guardado no extra space da thread (`luaE_cbmpinterrupt`), que lança o erro de tempo ou desliga a flag. `lua_cbmp_poll(L)` deixa bibliotecas C conferirem a flag. Zera o extra space da thread principal em `lua_newstate`. |
| `cbmp-i1-lib-interrupt.patch` | `lstrlib.c`, `ltablib.c`, `loslib.c` | `lua_cbmp_poll` nos laços C longos: casamento de padrões (passos de `match`, `singlematch` pesado pelo tamanho da classe, `%b`, busca simples `lmemfind` pesada pelo tamanho da agulha) a cada 4096 unidades; `string.rep` de pedaços vazios devolve `""` na hora; `table.insert/remove/move/concat` por iteração; cada comparação do `table.sort`; `os.date` por conversão. Ao atualizar o Lua, refaça a varredura de laços (tabela no report da Task 2 do sub3). |

`port-version` 100 só marca que é o port local.

### Ao atualizar o Lua

1. Copie o port novo do vcpkg para cá (mantendo a linha `PATCHES` com os três `cbmp-*.patch`).
2. Reaplique/ajuste os patches (os trechos tocados estão citados no cabeçalho de cada `.patch`).
3. Rode `cbscript_tests` (os casos `escape *` falham — por prazo rígido, sem travar — se um patch não estiver ativo).

### Reinstalar localmente (Windows)

O vcpkg recompila o `lua` sozinho quando a ABI do port muda (o hash inclui os arquivos do overlay). Basta reconfigurar:

```powershell
$env:VCPKG_ROOT = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg"
cmake --preset windows-release -DVCPKG_INSTALLED_DIR=C:/cbmp-vi
```

Para conferir: `C:/cbmp-vi/vcpkg/blds/lua/` mostra os logs `patch-x64-windows-2/3/4-*.log` dos três patches.
