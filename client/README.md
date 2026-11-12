# Cliente: o mod `cb-mp`

O mod que roda dentro do Hogwarts Legacy, carregado pelo UE4SS 3.0.1 (commit `4fb9b615a24c724041ef23e2c4073f2610c4c6d0`).
Ele lê o bilhete que o launcher deixa, conecta ao `cb-server` pelo protocolo 1 (`proto/`), manda a posição do seu
personagem e desenha os outros jogadores como "bonecos" (clones do personagem). Versão em `client/VERSION` (`0.2.0`).

Spec: `docs/superpowers/specs/2026-11-11-sub5a-mod-conectar-design.md`. Teste no jogo: `docs/TESTE-NO-JOGO-5A.md`.
Visão geral em linguagem simples: `docs/COMO-FUNCIONA.md`.

- [Arquitetura: núcleo e casca](#arquitetura-núcleo-e-casca)
- [Compilar e testar o núcleo](#compilar-e-testar-o-núcleo)
- [Integração `client-e2e`](#integração-client-e2e)
- [Build do mod (UE4SS)](#build-do-mod-ue4ss)
- [Contrato do cliente](#contrato-do-cliente)
- [Regras do repositório para este código](#regras-do-repositório-para-este-código)

## Arquitetura: núcleo e casca

```
client/
  core/        núcleo em C++23 puro (sem UE4SS, sem Windows.h); compila em Windows e Linux
  tests/       cbmp_client_tests (Catch2): FakeGame, FakeTransport, FakeGate, relógio falso
  tests/platform/  testes das portas Win32 (só Windows)
  headless/    cbmp-headless: núcleo real + GnsTransport + ScriptedGame, sem o jogo (usado pelo e2e)
  src/         casca do mod (Windows + UE4SS): CbMpMod, dllmain, CrashDump, FileHash, SafeCall, Utf8Path
    platform/  portas Win32 sem UE4SS: WinProcessGate, WinConnectFs, Arming
    game/      Ue4ssGame (IGame) e Reflect: a ÚNICA pasta que fala com o Unreal
  package/     LEIA-ME.txt e THIRD-PARTY-NOTICES.txt que entram no zip do mod
  vcpkg.json   pacotes do build do mod (gamenetworkingsockets, nlohmann-json)
  vcpkg-overlays/  ports locais só do mod (GNS com protobuf estático)
  deps/RE-UE4SS    UE4SS no commit fixo (git-ignored; tools/fetch-ue4ss.ps1)
```

Regras de dependência:

- `core` depende só de `cbmp_codec` (bitstream e mensagens da `proto/`), GameNetworkingSockets e nlohmann-json.
  **Nada de UE4SS, Unreal nem `Windows.h` em `client/core`**: um teste de guarda (`test_guard.cpp`) falha se um
  arquivo do núcleo incluir esses cabeçalhos. O `core` só enxerga o jogo pela interface `IGame` e a rede por
  `ITransport`.
- `client/src/platform` usa Win32 mas não UE4SS; compila também no build nativo da raiz no Windows (para ter teste).
- `client/src/game` é a única pasta que inclui o Unreal; `CbMpMod.*` e `dllmain.cpp` são a casca do mod.
- `client/headless` é uma ferramenta de teste e **pode** incluir `windows.h` (a regra acima vale só para `core`).
- Namespace `cbmp::client`, cabeçalhos em `<cbmp/client/...>`, métodos em PascalCase (como `IGame`), casos Catch2 com
  nome começando por `client: `.

Módulos do núcleo: `ConnectFile` (ponteiro e `connect.json`), `ProcessGate` (porta dos dois processos), `Session`
(máquina de estados; única entrada `Tick(agora)`), `NetClient` e `ITransport`/`GnsTransport` (rede),
`SnapshotMirror` e `RemoteStore` (estado remoto), `Interp` (interpolação), `WorldTracker` (menu/carregando/no mundo),
`LocalSampler` (estado local → `PlayerState`), `SendScheduler` (20 Hz), `PuppetDirector` (bonecos), `Limits` (todos
os limites num cabeçalho), `Log`, `ModConfig` (`cb-mp.json`).

## Compilar e testar o núcleo

O núcleo e os testes entram no build nativo da raiz (alvos `cbmp_client_core`, `cbmp_client_tests`, `cbmp-headless`).
No Windows (Developer PowerShell do Visual Studio), a partir da raiz:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -SkipAutomaticLocation
$env:VCPKG_ROOT='C:\vcpkg'
cmake --preset windows-release -DVCPKG_INSTALLED_DIR=C:/cbmp-vi      # nunca apague C:\cbmp-vi
cmake --build --preset windows-release --target cbmp_client_tests
ctest --test-dir build-native/windows-release -C Release --output-on-failure -R "^client: "
```

No Linux, o preset `linux-release` (ou `linux-asan`) com o mesmo alvo. O workflow `native` roda tudo em Linux (ASan)
e Windows. O build do mod (abaixo) não tem testes: o núcleo e as portas são testados no build da raiz.

## Integração `client-e2e`

`tools/client-e2e.py` sobe o `cb-server` real em modo dev com a mesma configuração de movimento do servidor de teste
(spawn na origem e limite `apparate` alto) e dois `cbmp-headless`, sem o jogo e sem Hub:

```bash
python tools/client-e2e.py --case all --out-dir client-e2e-out     # walk, loss, server-down
```

Pré-requisito: `cb-server`, `cb-bots` (gera os tickets) e `cbmp-headless` compilados no build da raiz. Os casos
(`walk`, `loss` com 20% de perda falsa, `server-down`) estão descritos no cabeçalho do script. O ticket chega ao
`cbmp-headless` só por um `connect.json` temporário, nunca por linha de comando. Roda no `native.yml` (Linux e
Windows).

## Build do mod (UE4SS)

Local, no Windows, com o UE4SS baixado (o submódulo `UEPseudo` dele é **privado**: exige uma conta do GitHub ligada
à Epic Games):

```powershell
tools\fetch-ue4ss.ps1                 # clona o UE4SS no commit fixo em client\deps\RE-UE4SS (usa o login do `gh`)
# Developer PowerShell do VS 2022 17.13+ ou do VS 2026 (o UE4SS exige MSVC 19.43+)
$env:VCPKG_ROOT='C:\vcpkg'            # ou o vcpkg que vem com o VS (o script usa o do dev shell se faltar)
tools\build.ps1 -Target mod           # UE4SS.dll, dwmapi.dll e cb-mp.dll em build\mod (Game__Shipping__Win64)
tools\package-mod.ps1                 # monta dist\cbmp-mod.zip (layout 2)
node tools/check-mod-zip.mjs dist/cbmp-mod.zip --version 0.2.0 --protocol 1
```

- Os pacotes do vcpkg do mod ficam em **`C:/cbmp-vi-mod`** (manifesto `client/vcpkg.json`, triplet
  `x64-windows-static-md`). **Nunca** aponte o build do mod para `C:/cbmp-vi`: o vcpkg em modo manifesto removeria de lá
  os pacotes do build da raiz (o script recusa).
- `tools/build.ps1` escolhe o gerador do CMake pela versão do Visual Studio (major 18: "Visual Studio 18 2026";
  17.13 ou mais: "Visual Studio 17 2022"); `-Generator` ou `CBMP_CMAKE_GENERATOR` forçam outro.
- O zip do mod tem `cbmp-mod.json` (`layout 2`, `version`, `protocol`, `ue4ss`, `minLauncher`), `loader/dwmapi.dll`,
  `ue4ss/UE4SS.dll`, `ue4ss/UE4SS-settings.ini` (console, GUI, recarga a quente e dump desligados), `ue4ss/Mods/` com o
  `cb-mp` e `licenses/`. `check-mod-zip.mjs` confere tudo isso.
- Em CI: `.github/workflows/mod.yml` (push na `main`, ou manual) gera o artefato `cbmp-mod` com um zip. **Precisa do
  segredo `UE4SS_SUBMODULES_TOKEN`** (leitura do `UEPseudo`); sem ele o job termina verde, **sem** artefato e com um
  aviso, e o `release.yml` não assina nada. Publicação: `launcher/README.md` ("Publicar um release") e
  `docs/AMBIENTE-TESTE-ONLINE.md` (release de teste, `--mod-zip`).

## Contrato do cliente

Esta é a especificação do que o cliente (qualquer implementação, hoje o `core`) tem de fazer diante do servidor.
Fonte do formato: `proto/include/cbmp/messages.hpp`. Constantes em `core/include/cbmp/client/Limits.hpp`.

### Entrada e tickets

- O mod só age se o launcher acabou de pedir: acha `cbmp-run.json` (ponteiro) subindo no máximo 5 pastas a partir da
  pasta do `main.dll`; valida tamanho (≤ 4 KiB), `v == 1`, `runDir` absoluto (último segmento `run`), `launchId` de 32
  hex e idade (≤ 600 s; futuro ≤ 120 s); confere que `<runDir>\connect.json` existe. Qualquer falha: **inerte**
  (nada registrado, nenhum log, nada na rede).
- `connect.json` (≤ 16 KiB; lido e **apagado** de forma atômica pelo mod): `{host, port, ticket, serverId, contentDir,
  protocol, contentReadOnly: true, launchId}`. `host` é IP literal; `launchId` tem de ser o do ponteiro; `protocol`
  tem de ser `cbmp::kProtocolVersion`; `contentDir` é **somente leitura** (hard links do cache do launcher; o 5a não
  usa). O ticket nunca vem por linha de comando ou variável de ambiente, **nunca vai ao log** e é zerado na memória
  logo depois do `Hello`.
- O mod lê o `exp` do ticket (sem confiar nele) e desiste com `ticket_expirado` se faltarem menos de 10 s.
- Dois processos do jogo na abertura (o primeiro morre): época em seção nomeada `Local\cbmp-mod-epoch-v1` e dono em
  mutex `Local\cbmp-mod-owner-v1`. Resgata o arquivo só quem é o processo atual **e** (já está `InWorld` **ou** passou
  `kClaimDelay`: 45 s na época 1, 5 s depois). Quem deixa de ser o atual antes de resgatar fica inerte
  (`substituido epoca=<a>→<b>`).

### Conexão

1. `ConnectByIPAddress` com prazo de 10 s; `Hello{protocol, buildHash = sha256 do main.dll, ticket}` (confiável) em até 1 s.
2. `Welcome` em até 10 s: guarda `playerId`; `tickRateHz` fora de 5..60 → `servidor_incompativel`.
3. `Online` tem dois subestados: **AwaitWorld** (menu ou carregando: **nada é enviado**) e **Playing** (no mundo:
   `Input{seq, PlayerState}` a cada 50 ms, 20 Hz; `seq` começa em 1 e só cresce; tick atrasado não gera rajada).
4. `Disconnected(motivo)` é final: remove os bonecos, fecha a conexão, zera o ticket; o jogo segue single-player. **Não há
   reconexão**: o usuário fecha o jogo e clica Jogar de novo.

### Snapshots (canal não confiável, em partes)

- **Descarte**: tick menor ou igual ao último montado é descartado; um tick novo reinicia a montagem; `partCount` ≤ 8.
  Só com **todas** as partes o tick é aplicado e **confirmado** (`SnapshotAck{tick}`).
- **Histórico e baseline**: o estado de um tick = o do `baselineTick` (vazio se 0) + `applySnapshotEntity` de cada entidade
  recebida (sobre `PlayerState{}` se a entidade não estava na baseline): o mesmo cálculo do servidor. Anel de 32
  ticks. Se o `baselineTick` não está mais no anel, o tick é descartado **sem** ack (o servidor volta a mandar
  completo em até 1 s).
- **Ids desconhecidos**: um id ainda sem `EntityEnter` entra no histórico, mas **não** é desenhado.
- **`EntityEnter{id, estado, nome}` / `EntityLeave{id}`** (confiáveis; **não** carregam tick): `Enter` cria a entidade
  (nome saneado, ≤ 64 bytes); `Leave` remove a entidade e apaga o id de **todos** os registros do histórico (espelho do
  `forget` do servidor).
- Limites: mensagem ≤ 8 KiB, ≤ 512 por tick, ≤ 255 entidades; fila > 4096 ou > 20 erros de decodificação em 10 s →
  `servidor_invalido`.

### `Correction`

`Correction{seq, pos, vel}` (confiável; inclui a de spawn, `seq 0`), comparada com o pawn local:

- ≤ 100 cm: nada.
- ≤ 2000 cm: `TeleportLocal(pos)`, no máximo uma por segundo (as demais esperam; ao vencer a espera mede-se de novo e
  nunca se move mais de 20 m).
- \> 2000 cm ou em `AwaitWorld`: **não** move o jogador (não há teleporte seguro de longa distância no 5a); o próximo
  `Input` vai com `Apparate`. Uma segunda correção longe com o mesmo `seq` registra `posicao_recusada` e mostra "o
  servidor não aceitou sua posição" (uma vez).
- **Carência**: depois de uma correção do servidor (spawn, teleporte), o servidor ignora em silêncio o que o cliente
  mandar até 30 s (e reenvia a correção uma vez); o cliente **segue enviando** `Input` normalmente. Correção repetida
  com o mesmo `seq` é tratada igual.

### Eventos

`Event{nome, payload}` recebido: no 5a **todo** evento é contado e **descartado** (inclusive `cb:reply`); o mod não envia
`Event`. `cb:command` (payload = texto JSON) e `cb:reply` são do subprojeto 5c.

### `Reject` / `Kick`

O texto (saneado, ≤ 255 bytes) é guardado e o fim da conexão vira o motivo: `recusado: <texto>` ou `expulso: <texto>`.
Motivos possíveis: `sem_arquivo`, `arquivo_invalido`, `protocolo_diferente`, `ticket_expirado`, `sem_resposta`,
`recusado: <texto>`, `sem_welcome`, `expulso: <texto>`, `conexao_perdida`, `servidor_invalido`, `servidor_incompativel`,
`erro_interno`. Cada um tem uma frase na tela ("CB Multiplayer: desconectado — …").

### Estado local → `PlayerState`

- Posição em cm (centro da cápsula), yaw quantizado; velocidade da propriedade `Velocity`, limitada a ±32767 cm/s.
- Valor não finito (NaN, inf) ou fora de ±2 000 000 000 cm: a amostra é **pulada** (nada é enviado naquele período).
- Estado (provisório no 5a): `Falling` → `Fall`, `Swimming` → `Swim`, `Flying` → `Broom`; no chão: < 10 cm/s `Idle`, ≤ 380
  `Walk`, ≤ 1000 `Run`, acima `Broom`.
- Primeiro `Input` depois de entrar no mundo, ou salto de mais de 3000 cm entre dois envios: o `Input` vai com
  `Apparate`.
- No mundo (`InWorld`): pawn válido, mundo fora da lista de menus, `frontendHint` falso, mesmos mundo e pawn há ≥ 2 s e
  posição a mais de 100 cm da origem em XY.

### Interpolação e bonecos

Atraso de desenho de 100 ms (até 400 ms por entidade), extrapolação ≤ 250 ms, salto direto se > 500 cm ou `Apparate`,
relógio do servidor = mínimo numa janela de 10 s. Bonecos: só a até 400 m, teto `maxBonecos` (32; máximo 64); criação
nula repete 3 vezes; destruído pelo jogo recria após 1 s (até 5 por minuto por entidade); 3 erros seguidos no mesmo
boneco o descartam e 20 erros em 10 s desligam os bonecos (a conexão continua).

## Regras do repositório para este código

- O repositório é **público**: nada derivado do dump do jogo (cabeçalhos, trechos, listas) entra aqui; só nomes curtos
  de classes, funções e propriedades do jogo, e só dentro de `client/src/game`.
- Nenhum código do MafiaHub, arcane-mp ou HogwartsMP.
- Texto para o usuário em pt-BR; identificadores de código em inglês.
- Mod do jogo: `cb-mp.json` (opcional, em `Win64\cbmp\`) com `statusNaTela`, `neutralizacaoExtra`, `animacaoBoneco`
  (`"velocidade"`, `"ambulatory"`, `"off"`) e `maxBonecos` (0..64); ver `docs/TESTE-NO-JOGO-5A.md`, Anexo C.
