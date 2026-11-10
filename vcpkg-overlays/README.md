# vcpkg-overlays

Ports locais que substituem os do registro do vcpkg. Ativados por `VCPKG_OVERLAY_PORTS` no preset `base`
(`CMakePresets.json`), então valem para todos os presets locais e do CI.

## lua (5.4.7 + patches do sandbox)

Cópia do port oficial `lua` 5.4.7 (árvore `97783a4c337df419c0a6a75e9599545c54259d86` do microsoft/vcpkg, a mesma da
baseline `ec48c11e…`) com dois patches pequenos usados pela vigia dos scripts (`server/script/src/resource.cpp`):

| Patch | Arquivo | O quê |
|---|---|---|
| `cbmp-c1-closethread-allowhook.patch` | `lstate.c` (`luaE_resetthread`) | Religa hooks (`allowhook = 1`) antes de rodar `__close` pendentes de uma thread fechada (`coroutine.close`, erro em `coroutine.wrap`). Sem isso, uma corrotina morta por erro lançado no hook roda `__close` sem vigia. |
| `cbmp-i1-match-interrupt.patch` | `lstrlib.c` (`match`) | A cada 4096 passos do casamento de padrões chama `check(L)` de um `struct cbmp_interrupt*` guardado no extra space da thread (ou nada, se NULL). O host lança erro ali quando o prazo passou. |

`port-version` 100 só marca que é o port local.

### Ao atualizar o Lua

1. Copie o port novo do vcpkg para cá (mantendo a linha `PATCHES` com os dois `cbmp-*.patch`).
2. Reaplique/ajuste os patches (os trechos tocados estão citados no cabeçalho de cada `.patch`).
3. Rode `cbscript_tests` (casos `escape C1`/`escape I1` falham se um patch não estiver ativo).

### Reinstalar localmente (Windows)

O vcpkg recompila o `lua` sozinho quando a ABI do port muda (o hash inclui os arquivos do overlay). Basta reconfigurar:

```powershell
$env:VCPKG_ROOT = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg"
cmake --preset windows-release -DVCPKG_INSTALLED_DIR=C:/cbmp-vi
```

Para conferir: `C:/cbmp-vi/vcpkg/blds/lua/` mostra os logs `patch-x64-windows-2/3-*.log` dos dois patches.
