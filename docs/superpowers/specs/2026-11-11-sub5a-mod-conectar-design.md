# Subprojeto 5a — Mod no jogo: conectar e se ver — Design

Data: 2026-11-11
Status: decidido pelo arquiteto com autonomia total do dono; aguardando revisão da spec
Base: `2026-11-09-plataforma-design.md` (S1–S6), `2026-11-10-sub4-launcher-design.md`, `docs/PENDENCIAS.md` (seção
"Antes do cliente no jogo"), o spike em `client/` e o reconhecimento do jogo de 2026-11-11 (Steam build 26175237,
UE 4.27, UE4SS v3.0.1 commit `4fb9b615`). Os dados do reconhecimento ficam **fora do repositório**; aqui entram só
nomes curtos de classes, funções e propriedades.

## Objetivo

Dois jogadores se vendo andar no mesmo mundo, entrando pelo launcher com um clique em **Jogar**:

1. o launcher instala o pacote assinado do mod (UE4SS + `cb-mp`), grava `run\connect.json` e abre o jogo;
2. o mod lê e apaga o `connect.json`, conecta no `cb-server` com o ticket e envia o estado do jogador local a 20 Hz;
3. cada outro jogador visível vira um **boneco** (ator do jogo) que se move de forma suave (interpolação);
4. ao cair a conexão os bonecos somem e o jogo segue como single-player.

Fora do 5a (etapas seguintes, uma linha cada):

- **5b** animação e aparência fiéis (roupa, casa, varinha, estados de corpo). No 5a o boneco é um clone do bruxo do
  próprio jogador e pode deslizar sem animar.
- **5c** chat, comandos (`cb:command`/`cb:reply`) e Lua de cliente.
- **5d** UI em HTML dos resources.
- **5e** controle do mundo: hora, clima, missões, NPCs, spawn/teleporte decidido pelo servidor, entrar sem save.
- **5f** feitiços e interações entre jogadores.

Também fora: reconexão automática, nomes sobre a cabeça, anti-cheat de cliente (S6 além do hash do build), Linux,
carregador próprio no lugar do proxy do UE4SS (ver "Riscos").

## Decisões

| Tema | Decisão |
|---|---|
| Base de injeção | UE4SS v3.0.1 (`4fb9b615`), mod C++ `cb-mp`. Só `client/src/game/` conhece UE4SS/Unreal; o resto fala com `IGame` |
| Protocolo | **sem mudança**: `cbmp::kProtocolVersion = 1`, mesma `cbmp-proto`, mesmo GameNetworkingSockets do servidor e dos bots |
| Servidor | **sem mudança de código**. Só configuração do servidor de teste: `movement.maxSpeedCmS.apparate` alto e limites folgados (ver "Spawn e saltos") |
| Rede no mod | consultada no tick do jogo (game thread); o GNS mantém a conexão viva na thread dele durante telas de carregamento |
| Envio | `Input` a 20 Hz (`Welcome.tickRateHz` se estiver em 5..60), não confiável, como os bots |
| Interpolação | renderiza 100 ms no passado; extrapola no máximo 250 ms; erro > 500 cm ou estado `Apparate` → salto direto |
| Boneco | `SpawnActor` da classe do pawn local (`BP_Biped_Player_C`, provado no jogo), colisão desligada, sem controller |
| Dois processos | contador de época em memória compartilhada + mutex nomeado; quem conecta é o processo **mais novo e estável** |
| Ponteiro | `Win64\cbmp\cbmp-run.json`, gravado pelo launcher a cada Jogar; não tem segredo; aponta o `run\` do build que abriu o jogo |
| Pasta do mod | `Win64\cbmp\` é o contêiner fixo (ponteiro, `logs\`, `crash\`); o pacote fica em `Win64\cbmp\atual\` (troca atômica lá dentro) |
| Carregador | proxy `dwmapi.dll` do UE4SS em `Win64\` + `Win64\override.txt` com `cbmp\atual\ue4ss`. A pasta `ue4ss\` do usuário nunca é tocada |
| UE4SS de outro mod | nunca sobrescrito nem apagado; com confirmação do jogador vai para `Win64\cbmp-backup\` e volta em "Remover mod do jogo" |
| Feedback | log em `Win64\cbmp\logs\` e, com melhor esforço, uma linha de status pelo `UIManager.AddTextTickerNotification` |
| Versões | mod `0.2.0` (protocolo 1, tag `mod-p1-v0.2.0`); launcher `0.2.0` |
| Simular | `simulateLaunch` passa a `false` por padrão (migração única na 0.2.0); continua em Configurações → Avançado |

## Arquitetura

```
Launcher 0.2.0 (Tauri)                              Jogo: HogwartsLegacy.exe (Phoenix\Binaries\Win64)
  Jogar ─► mod 0.2.0 instalado e ativo?             dwmapi.dll (proxy UE4SS) ─► override.txt
        ─► <dados>\run\connect.json (ACL, ticket)     └► cbmp\atual\ue4ss\UE4SS.dll
        ─► Win64\cbmp\cbmp-run.json (sem segredo)          └► Mods\cb-mp\dlls\main.dll
        ─► abre o jogo                                        │
                                                     ┌────────┴──────────────────────────────────────┐
                                                     │ client/src        (Windows + UE4SS)           │
                                                     │  CbMpMod: ciclo de vida, tick, log, crash     │
                                                     │  platform/: WinProcessGate, WinConnectFs      │
                                                     │  game/Ue4ssGame : IGame   ◄── única porta     │
                                                     ├───────────────────────────────────────────────┤
                                                     │ client/core       (C++23 puro, sem UE4SS)     │
                                                     │  Session ─ ConnectFile ─ ProcessGate (porta)  │
                                                     │  NetClient ─ ITransport (GnsTransport)        │
                                                     │  SnapshotMirror ─ RemoteStore ─ Interp        │
                                                     │  LocalSampler ─ SendScheduler ─ WorldTracker  │
                                                     │  PuppetDirector ─ Limits ─ Log                │
                                                     └───────────────┬───────────────────────────────┘
                                                                     │ UDP (GNS), protocolo 1
                                                                 cb-server ◄──► outros jogadores
```

Regras de dependência: `client/core` depende só de `cbmp_codec` (bitstream + messages da `proto/`, sem libsodium),
GameNetworkingSockets e nlohmann-json; compila em Windows e Linux e entra no build nativo da raiz. `client/src`
depende de `core` e do UE4SS e só compila no build do mod. Código novo do cliente usa o namespace `cbmp::client` e
cabeçalhos em `<cbmp/client/...>` (a `proto/` já usa `cbmp::` e `<cbmp/...>`).

O `SpikeController`, o F8 e o `package/` do spike saem; `Transform`, `Log`, `Store`, `SafeCall`, `CrashDump` e
`FileHash` ficam.

## Componentes

### `IGame` estendido (`client/core/include/cbmp/client/IGame.hpp`)

Todos os métodos rodam no game thread. Falha interna vira `GameError` (o `SafeCall` com `/EHa` converte SEH); o
núcleo nunca vê ponteiros do motor.

```cpp
namespace cbmp::client {
enum class MoveMode : uint8_t { Ground, Falling, Swimming, Flying, Other };
struct LocalState { Vec3 pos; double yawDeg, pitchDeg; Vec3 vel; MoveMode mode; };   // cm, graus, cm/s
struct WorldSample { uint64_t worldId; std::string worldName; uint64_t pawnId; bool hasPawn; bool frontendHint; };
struct PuppetPose  { Vec3 pos; double yawDeg; Vec3 vel; uint8_t moveState; };
struct PuppetId    { uint32_t v{}; bool operator==(const PuppetId&) const = default; };
enum class PuppetResult { Ok, Gone, Failed };

class IGame {
public:
    virtual ~IGame() = default;
    virtual WorldSample SampleWorld() = 0;                       // barato; chamado a cada tick
    virtual std::optional<LocalState> ReadLocal() = 0;           // nullopt = sem pawn utilizável agora
    virtual bool TeleportLocal(const Vec3& pos) = 0;             // só correções curtas (≤ 20 m)
    virtual std::optional<PuppetId> SpawnPuppet(const PuppetPose& at) = 0;
    virtual PuppetResult MovePuppet(PuppetId id, const PuppetPose& to) = 0;   // Gone = o jogo destruiu o ator
    virtual void DestroyPuppet(PuppetId id) = 0;
    virtual void ForgetAllPuppets() = 0;                         // mundo trocou: os atores já morreram
    virtual bool PlayerSingletonIntact() = 0;                    // o jogo ainda aponta para o pawn local?
    virtual bool ShowStatus(std::string_view utf8) = 0;          // false = indisponível (segue só no log)
    virtual std::string Diagnose() = 0;
};
}
```

`worldId`/`pawnId` são identificadores opacos (índice + número de série do objeto), nunca ponteiros reutilizáveis.
`PuppetId` é um número do `Ue4ssGame`, que guarda por trás uma referência fraca ao ator.

### Módulos do núcleo (`client/core`)

| Módulo | Responsabilidade |
|---|---|
| `ConnectFile` | valida o ponteiro e o `connect.json` (funções puras sobre bytes); porta `IConnectFs` para "abrir exclusivo, ler e apagar" |
| `ProcessGate` (porta) | `epoch()`, `isCurrent()`, `tryOwn()`; implementação Win32 em `client/src/platform`, falsa nos testes |
| `Session` | máquina de estados da sessão; dona de todos os outros módulos; única entrada: `Tick(nowSec)` |
| `ITransport` / `GnsTransport` | conectar por IP, enviar (confiável ou não), receber, estado e motivo de fim; `FakeTransport` nos testes |
| `NetClient` | `Hello`, espera de `Welcome`, decodificação com limites, despacho das mensagens, contadores |
| `SnapshotMirror` | montagem de partes, histórico por tick (anel de 32), reconstrução por baseline, `SnapshotAck` |
| `RemoteStore` | entidades vivas (`EntityEnter`/`EntityLeave`), nome saneado, última amostra |
| `Interp` | relógio do servidor estimado, fila de amostras por entidade, pose no instante de desenho |
| `WorldTracker` | decide `Frontend` / `Loading` / `InWorld` a partir de `WorldSample` + `LocalState` |
| `LocalSampler` | `LocalState` → `cbmp::PlayerState` (quantização, NaN/inf, estado de movimento, detecção de salto) |
| `SendScheduler` | cadência de 20 Hz sem rajada de compensação |
| `PuppetDirector` | liga `RemoteStore`+`Interp` ao `IGame`: cria, move, destrói, teto, recriação com espera |
| `Limits` | todas as constantes de limite num só cabeçalho (seção "Segurança") |
| `Log` | o do spike + arquivo por processo, rotação e filtro de segredo |

## Inércia e o ponteiro do launcher

O mod só age se o launcher acabou de pedir. No construtor (thread do UE4SS, só leitura de arquivo):

1. acha `cbmp-run.json` subindo no máximo 5 pastas a partir da pasta do `main.dll` (fica em `Win64\cbmp\`);
2. valida: ≤ 4 KiB, JSON, `v == 1`, `runDir` absoluto, sem `\\` no começo, último segmento `run`, `launchId` de 32
   hex, `writtenAt` (Unix, s) com no máximo 600 s de idade e no máximo 120 s no futuro;
3. confere que `<runDir>\connect.json` existe (só consulta; não abre);
4. qualquer falha → **Inerte**: não registra o tick, não abre log, não inicia o GNS, não cria objetos nomeados, não
   grava nada. É o estado de quem abre o jogo pela Steam/Epic sem o launcher. Passando nos três, o processo fica
   **armado** (`Armed`): abre o log, entra na contagem de época e registra o tick em `on_unreal_init`.

`cbmp-run.json` (gravado pelo launcher por arquivo temporário + rename, em cada Jogar):

```json
{ "v": 1, "runDir": "C:\\Users\\x\\AppData\\Local\\CBMultiplayer-Teste\\run",
  "launchId": "9f3c…32 hex", "writtenAt": 1794438789, "launcher": "0.2.0" }
```

Isso resolve a pendência M-8: o mod não conhece o nome da pasta de dados; cada build (oficial, Teste local, Teste)
aponta a sua. O ticket continua só no `connect.json` (DACL do usuário). O `connect.json` ganha o campo `launchId`
(o mesmo do ponteiro): o mod recusa o arquivo se for diferente (ponteiro velho + arquivo de outro Jogar).

**O que continua carregado no single-player.** O proxy `dwmapi.dll` do UE4SS sempre carrega o `UE4SS.dll` (sem ele,
mostra erro e fecha o jogo), então o UE4SS e os ganchos internos dele ficam ativos mesmo com o `cb-mp` inerte. Quem
quiser o jogo 100% original usa **Remover mod do jogo** (Configurações); o próximo Jogar reinstala do pacote local
sem baixar de novo.

## Dois processos na abertura

Observado no jogo: o mod inicia duas vezes, com ~33 s entre as duas, os dois processos ticam ao mesmo tempo por ~13 s
e o **primeiro morre**. Um ticket vale uma vez; se o processo que vai morrer conectar, a sessão se perde.

- **Época**: seção de memória nomeada `Local\cbmp-mod-epoch-v1` (8 bytes, sem segredo). Cada processo armado faz
  `InterlockedIncrement` ao iniciar e guarda o valor; `isCurrent()` = o contador ainda é o meu. A seção some quando o
  último processo sai (reinício sem sobreposição recomeça em 1).
- **Dono**: mutex nomeado `Local\cbmp-mod-owner-v1`, tomado sem esperar por quem vai resgatar o arquivo e mantido até
  o processo terminar. No máximo um dono por sessão do Windows.
- **Quando resgatar** (conferido a cada tick, estado `Armed`): `isCurrent()` **e** (`WorldTracker == InWorld` **ou**
  passaram `kClaimDelay` desde o primeiro tick deste processo). `kClaimDelay` = **45 s** se a época é 1 (posso ser o
  que vai morrer) e **5 s** se é maior que 1 (já sou o sucessor).
- Processo que deixa de ser o atual antes de resgatar → `Superseded`: registra uma linha e fica inerte para sempre.
- **Resgate atômico** (`WinConnectFs`): `CreateFileW` com compartilhamento **zero**, `DELETE`,
  `FILE_FLAG_DELETE_ON_CLOSE` e `FILE_FLAG_OPEN_REPARSE_POINT`; confere arquivo comum, ≤ 16 KiB, dono = usuário do
  processo; lê; fecha (o arquivo some). Violação de compartilhamento → tenta de novo no próximo tick por até 5 s.
  Arquivo ausente → `Disconnected(sem_arquivo)`.
- **Log sem mistura**: um arquivo por processo, `Win64\cbmp\logs\cb-mp-<aaaammdd-hhmmss>-<pid>.log`; ficam os 10 mais
  novos. Todo log começa com pid, época e versão.

Prazos do launcher ajustados para caber nessa espera: `CONNECT_TTL` 60 → **240 s** e `TICKET_RENEW_AFTER_SECS` 240 →
**30 s** (o ticket gravado tem pelo menos 270 s de validade; o arquivo dura menos que o ticket). O mod lê o `exp` do
ticket (sem confiar nele) e desiste com `ticket_expirado` se faltarem menos de 10 s.

## Protocolo do cliente

Fonte: `proto/include/cbmp/messages.hpp`, `bots/src/bot.cpp` e o contrato de `docs/PENDENCIAS.md`.

### Estados da sessão

```
Inert ── ponteiro válido ──► Armed ── resgate ──► Connecting ── GNS conectado ──► Handshake ── Welcome ──► Online
                               │                      │                              │                       │
                          Superseded                  └────────── falha / Reject / Kick / fim ───────────────┤
                                                                                                             ▼
                                                                                        Disconnected(motivo) (final)
```

`Online` tem dois subestados: `AwaitWorld` (menu, carregando) e `Playing` (`WorldTracker == InWorld`).
`Disconnected` é final: remove todos os bonecos, fecha a conexão, zera o ticket; o jogo segue single-player.

### Passo a passo

1. **Connecting**: `host` tem de ser IP literal (o launcher resolve e valida antes de gravar); `ConnectByIPAddress`;
   prazo de 10 s.
2. **Handshake**: ao conectar, envia `Hello{protocol = kProtocolVersion, buildHash = sha256 do main.dll, ticket}`
   (confiável) em até 1 s (o servidor fecha em 5 s). Logo depois o ticket é sobrescrito com zeros e liberado. Antes
   de conectar, `connect.json.protocol != kProtocolVersion` → `protocolo_diferente` sem tocar na rede.
3. **Welcome** em até 10 s: guarda `playerId`; `tickRateHz` fora de 5..60 → `servidor_incompativel`.
4. **AwaitWorld**: não envia `Input`. Recebe e confirma snapshots normalmente; guarda a última `Correction`.
5. **Playing**: `Input{seq, PlayerState}` a cada 50 ms; `seq` começa em 1 e só cresce. Tick atrasado não gera rajada
   (próximo envio = agora + 50 ms).
6. **Snapshots** (não confiáveis, em partes):
   - tick menor ou igual ao último montado → descarta; tick novo reinicia a montagem; só com todas as partes o tick
     é aplicado e confirmado (`SnapshotAck{tick}`);
   - registro do tick = registro de `baselineTick` (vazio se 0) + `applySnapshotEntity` de cada entidade recebida
     (sobre `PlayerState{}` quando a entidade não está na baseline). Mesmo cálculo do servidor;
   - `baselineTick` que não está mais no anel → tick descartado sem ack (o servidor volta a mandar completo em ≤ 1 s);
   - id sem `EntityEnter` ainda: entra no histórico, **não** é desenhado;
   - `EntityEnter{id, estado, nome}` cria a entidade (amostra com o instante de chegada); `EntityLeave{id}` remove a
     entidade e apaga o id de **todos** os registros do histórico (espelho do `forget` do servidor). Nenhum dos dois
     carrega tick.
7. **Correction{seq, pos, vel}** (confiável; inclusive a de spawn, `seq 0`):
   - distância até o pawn local ≤ 100 cm → nada a fazer;
   - ≤ 2000 cm → `TeleportLocal(pos)` (no máximo 1 por segundo; as demais esperam);
   - \> 2000 cm ou `AwaitWorld` → **não** move o jogador (o 5a não tem teleporte seguro de longa distância); o
     próximo `Input` vai com estado `Apparate` (ressincronização). O servidor aceita se o limite `apparate` dele
     permitir; senão ignora em silêncio (correção do servidor, carência de 30 s) ou expulsa (anti-cheat);
   - o cliente segue enviando `Input` durante a carência; correção repetida com o mesmo `seq` é tratada igual.
8. **Event**: no 5a todo `Event` recebido é contado e descartado (inclusive `cb:reply`); o mod não envia `Event`.
9. **Reject / Kick**: guarda o texto (saneado, ≤ 255 bytes) e espera o fechamento; fim com código
   `App_Min+1` (recusado), `+2` (limites), `+3` (protocolo), `+4` (expulso / servidor encerrando) vira o motivo.

### Estado local → `PlayerState`

- Posição: `lround` da posição do ator (centro da cápsula) em cm; yaw com `quantizeYaw`; pitch `quantizePitch(0)`.
- Velocidade: propriedade `Velocity` do `CharacterMovement`, limitada a ±32767 cm/s.
- Qualquer valor não finito ou fora de ±2 000 000 000 cm → a amostra é pulada (nada é enviado naquele período).
- Estado (provisório no 5a; 5b refina com os estados do jogo): `Falling` → `Fall`; `Swimming` → `Swim`; `Flying` →
  `Broom`; chão: < 10 cm/s `Idle`, ≤ 380 `Walk`, ≤ 1000 `Run`, acima `Broom`.
- **Salto**: primeiro `Input` depois de entrar em `InWorld`, ou deslocamento > 3000 cm entre dois envios (viagem
  rápida, troca de área) → aquele `Input` vai com `Apparate`.

### `WorldTracker`

`InWorld` exige, ao mesmo tempo: pawn válido; `worldName` fora da lista de menus (`RootLevel`); `frontendHint`
falso (`UIManager.IsInFrontendLevel`, quando disponível); mesmos `worldId` e `pawnId` há ≥ 2 s; posição a mais de
100 cm da origem em XY (durante o carregamento o pawn fica em (0,0,z) caindo). Senão: `Frontend` (mundo de menu) ou
`Loading`. Sair de `InWorld` pausa o envio e solta os bonecos (`ForgetAllPuppets` se o mundo mudou; `DestroyPuppet`
se só o pawn mudou).

### Interpolação

- Tempo do servidor de uma amostra = `tick / tickRateHz`. Deslocamento de relógio = mínimo de
  `(chegada − tempoDoTick)` numa janela de 10 s; mudança > 2 s reinicia as filas.
- Instante de desenho = agora − deslocamento − **100 ms**. Por entidade, fila de até 32 amostras `(t, pos, yaw, vel)`.
- Entre duas amostras: posição linear, yaw pelo menor arco. Depois da última: extrapola com a velocidade por no
  máximo **250 ms** e para. Entidade sem amostra nova está parada ou igual (o servidor só manda o que mudou).
- Alvo a mais de **500 cm** da pose desenhada, ou amostra com `Apparate` → salto direto, sem deslizar.
- Entidades longe recebem menos atualizações (faixas do servidor): o atraso por entidade sobe até 400 ms conforme o
  intervalo observado.

### Spawn e saltos (sem mudar o servidor)

Com Hub, o servidor cria o jogador na origem e manda `Correction{seq 0}`; inputs longe disso são ignorados. O 5a não
teleporta o jogador para a origem (ficaria sob o mapa). Solução só de configuração, no `server.cfg` do servidor de
teste (`tools/teste-online/server.cfg.json`):

```json
"movement": { "maxSpeedCmS": { "idle": 50, "walk": 600, "run": 1500, "broom": 12000, "swim": 1500,
                               "fall": 8000, "apparate": 100000000 }, "tolerance": 0.2 }
```

Com `apparate` alto, o primeiro `Input` (estado `Apparate`, posição do save) fecha a janela da correção de spawn e
passa na validação (`world.cpp`, `onInput`): o jogador aparece onde está no jogo. O mesmo vale para viagem rápida.
Consequências aceitas no 5a: no servidor de teste o cliente pode se teleportar à vontade (pendência "Estados de
movimento confiáveis" continua aberta, dona 5e); enquanto está no menu o jogador conta como parado na origem do
mundo; servidor com a configuração padrão (`apparate: 0`) aceita a conexão mas nunca aceita a posição — o mod avisa
no log e na tela (tabela de erros).

## Bonecos

Nomes confirmados no reconhecimento (motor: `/Script/Engine`; jogo: `/Script/Phoenix`, `Ambulatory`, `Cognition`).

**Criar** (`SpawnPuppet`): `UWorld::SpawnActor(classe do pawn local, pos, yaw)`; em seguida, cada passo isolado em
`SafeCall` (falha de um passo registra uma vez e segue):

1. `Actor.SetActorEnableCollision(false)` — não empurra, não bloqueia, não dispara gatilhos;
2. conferir que `Pawn.Controller` é nulo; se não for, `Pawn.DetachFromControllerPendingDestroy`. Sem controller o
   `CharacterMovement` não roda física (`bRunPhysicsWithNoController = false` no jogo): o boneco não cai;
3. `Biped_Player.SetInvulnerable(true)`;
4. `CognitionStimuliSourceComponent`: `SetAutoRegisterAsSource(false)` e `UnregisterFromPerceptionSystem` — inimigos
   não percebem o boneco como jogador;
5. `Biped_Character.EnablePawnInteraction(false)`;
6. `PlayerSingletonIntact()`: chama `Biped_Player.GetTheBipedPlayer` e compara com o pawn local. Se o jogo passou a
   apontar para o boneco: destrói o boneco, confere de novo e **desliga os bonecos nesta sessão** (a conexão segue;
   o log diz `bonecos_desligados: singleton`).

Neutralização extra (desligada por padrão; chave `neutralizacaoExtra` do `cb-mp.json`, para experimento no teste
manual): `Deactivate` + `SetComponentTickEnabled(false)` nos componentes `CameraStackComponent`,
`InputWatcherComponent` e `BP_PlayerPerceptionPoint_C`, e `Base_Character.StartSettingTargetableByAiState(false, …)`.

**Mover** (`MovePuppet`, todo tick): `Actor.K2_SetActorLocationAndRotation(pos, (0, yaw, 0), bSweep=false,
bTeleport=true)`. Se a função não existir: `Actor.K2_TeleportTo`; se também não: `SceneComponent.
K2_SetWorldLocationAndRotation` no `RootComponent`. As `UFunction` são resolvidas por nome uma vez por classe e os
parâmetros são preenchidos por nome de propriedade (nenhum offset fixo no código).

**Animação (opcional, não bloqueia o 5a)**: chave `animacaoBoneco`: `"velocidade"` (padrão) grava a velocidade
interpolada na propriedade `Velocity` do `CharacterMovement` do boneco (escrita simples; se o AnimBP do jogo não a
usar, nada acontece); `"ambulatory"` chama também `Ambulatory_Character.SetWorldSpeedAndDirection` e `SetIsInIdle`
(experimento); `"off"` não faz nada. Boneco deslizando parado é resultado aceito.

**Destruir**: `Actor.K2_DestroyActor`. **Validade**: referência fraca conferida antes de cada uso; ator destruído
pelo jogo → `Gone`.

**Regras do `PuppetDirector`**: só em `Playing`; só entidades a até 400 m do jogador local; teto `maxBonecos`
(padrão 32, máximo 64), os mais próximos primeiro; `SpawnActor` nulo → nova tentativa em 1 s, 3 vezes; `Gone` →
recria depois de 1 s, no máximo 5 vezes por minuto por entidade, depois desiste dela; 3 `GameError` seguidos em
`MovePuppet` do mesmo boneco → destrói e trata como `Gone`; 20 `GameError` em 10 s no total → bonecos desligados na
sessão. O próprio jogador (`playerId`) nunca vira boneco.

**Status na tela**: `UIManager.AddTextTickerNotification(texto)` (função estática do jogo que alimenta o painel de
notificações do HUD), só em `InWorld` e só em mudança de estado: "CB Multiplayer: conectado (N por perto)",
"CB Multiplayer: desconectado — motivo". Primeira falha desliga o recurso na sessão; `statusNaTela: false` no
`cb-mp.json` desliga de vez. `PrintString`/mensagens de depuração do motor não servem (build Shipping).

`cb-mp.json` (opcional, em `Win64\cbmp\`, nunca vem do servidor): `statusNaTela` (bool), `neutralizacaoExtra`
(bool), `animacaoBoneco` (texto), `maxBonecos` (0..64). Arquivo ausente ou inválido → padrões.

## Pacote do mod e launcher

### Zip assinado (`cbmp-mod-p1-v0.2.0.zip`)

```
cbmp-mod.json                         {"layout":2,"version":"0.2.0","protocol":1,"ue4ss":"3.0.1+4fb9b615","minLauncher":"0.2.0"}
loader/dwmapi.dll                     proxy do UE4SS (vai para Win64\)
ue4ss/UE4SS.dll
ue4ss/UE4SS-settings.ini              o do reconhecimento, com ConsoleEnabled=0, GuiConsoleEnabled=0, GuiConsoleVisible=0,
                                      EnableHotReloadSystem=0 e [CrashDump] EnableDumping=0
ue4ss/Mods/mods.txt                   cb-mp : 1 / Keybinds : 0
ue4ss/Mods/cb-mp/enabled.txt
ue4ss/Mods/cb-mp/dlls/main.dll
licenses/UE4SS-LICENSE.txt            MIT
licenses/THIRD-PARTY-NOTICES.txt      GameNetworkingSockets, protobuf, abseil, OpenSSL, nlohmann-json
LEIA-ME.txt
```

A seção `[Hooks]` do UE4SS fica como no reconhecimento (configuração provada no jogo); enxugar os ganchos é tarefa
do 5b, com medição.

### O que o launcher grava

| Onde | O quê | Regra |
|---|---|---|
| `Win64\cbmp\atual\` | o zip inteiro | troca atômica dentro do contêiner (`atual.staging` → `atual`, anterior em `atual.old`); hashes em `mods\installed.json` como hoje |
| `Win64\cbmp\cbmp-run.json` | ponteiro | a cada Jogar |
| `Win64\cbmp\logs\`, `crash\`, `cb-mp.json` | do mod / do jogador | o launcher não toca |
| `Win64\dwmapi.dll` | cópia de `atual\loader\dwmapi.dll` | só se ausente, igual ao nosso (sha256 atual ou do histórico do registro) |
| `Win64\override.txt` | uma linha: `cbmp\atual\ue4ss` | só se ausente ou já igual |
| `Win64\cbmp-backup\` | arquivos do jogador movidos com confirmação | nunca apagados pelo launcher |

`installed.json` ganha `layout`, `loader {sha256,size}`, `loaderHistory` (até 8 hashes) e `backups`. O launcher
confere `cbmp-mod.json` (`layout == 2`, `version` e `protocol` iguais aos assinados, `minLauncher` ≤ versão do
launcher); divergência → `mod_invalid`. Migração do layout antigo: arquivos listados no registro antigo direto em
`cbmp\` são removidos antes da primeira instalação em `atual\`. Launcher antigo (≤ 0.1.3) que baixar o zip novo
extrai em `cbmp\` sem ativar nada: inofensivo; o updater leva para a 0.2.0.

**Ativação conferida em todo Jogar** (depois de `ensure`): hash do `Win64\dwmapi.dll` e conteúdo do `override.txt`.

**Conflito** (`mod_conflict`): `dwmapi.dll` ou `override.txt` que não são nossos. A UI explica e o Rust confirma num
diálogo nativo: "Encontramos outro carregador de mods (UE4SS) na pasta do jogo. Para jogar no CB Multiplayer é
preciso usar o do launcher. Seus arquivos não são apagados: ficam em `Phoenix\Binaries\Win64\cbmp-backup` e voltam
quando você remover o mod do CB Multiplayer nas Configurações. Enquanto isso, seus outros mods do UE4SS ficam
desligados." Aceitando, os dois arquivos são **movidos** (rename no mesmo volume; nome repetido ganha sufixo `.1`,
`.2`…). `Win64\xinput1_3.dll` junto de `Win64\UE4SS.dll` (UE4SS antigo por outro proxy) → `mod_conflict` sem
opção automática: o texto pede para o jogador remover; o launcher não mexe.

**Remover mod do jogo** (Configurações, com o jogo fechado): apaga `dwmapi.dll` e `override.txt` **se forem os
nossos**, devolve o que está em `cbmp-backup\` (só para destino vazio), apaga `cbmp\atual*` e o ponteiro, mantém
`cbmp\logs\`, mantém o pacote em `mods\package\` e a versão mínima no registro (o anti-downgrade não é zerado).
Configurações mostra o estado: não instalado / instalado X.Y.Z / conflito.

**Epic e pastas sem permissão** (não verificado numa Epic real): o launcher tenta sem elevação. Falha de permissão
ao criar `Win64\cbmp` ou gravar em `Win64\` → `mod_needs_admin` com o botão "Instalar com permissão de
administrador (uma vez)". O botão reabre o próprio launcher elevado (`ShellExecuteW`, verbo `runas`) com
`--mod-admin-setup`, que faz só isto e sai: valida a pasta do jogo (as mesmas regras de `validate_game_dir`, sem
pontos de reanálise no caminho, fora de `%SystemRoot%`); cria `Win64\cbmp` e dá **Modificar** só ao SID do usuário
que pediu; confere de novo a assinatura de `mods\package\cbmp-mod.zip` com a chave embutida e extrai dele só o
`loader/dwmapi.dll` para `Win64\`; grava `override.txt`; trata conflito só se o pedido trouxer a confirmação. Daí em
diante instalar, atualizar e gravar o ponteiro acontecem sem elevação dentro de `cbmp\`. Loader novo (hash diferente)
pede elevação de novo. UAC recusado → `mod_needs_admin` continua, com o caminho manual no texto. Abrir o jogo da
Epic direto pelo executável segue como pendência não verificada (`docs/PENDENCIAS.md`).

### Outras mudanças no launcher (0.2.0)

- `launch.rs`: ponteiro; `launchId` no `connect.json`; `CONNECT_TTL` 240 s; ao vencer o prazo o ponteiro é apagado
  (com ou sem `connect.json`). `play.rs`: `TICKET_RENEW_AFTER_SECS` 30; resolve o `host` e grava IP literal já validado
  pela regra de endereços do build.
- `game_running`: Jogar e Remover recusam se há `HogwartsLegacy.exe` rodando.
- `simulateLaunch`: padrão `false`; `settingsVersion: 2` desliga uma vez o valor salvo; opção em Avançado.
- Configurações: estado do mod, "Remover mod do jogo", "Abrir pasta de logs do mod".
- Erros novos (código → mensagem):

| Código | Mensagem |
|---|---|
| `mod_conflict` | Já existe outro carregador de mods (UE4SS) na pasta do jogo. |
| `mod_needs_admin` | O launcher não tem permissão para instalar o mod na pasta do jogo. |
| `mod_incompatible` | Este mod precisa de uma versão mais nova do launcher. Atualize e tente de novo. |
| `mod_activation_failed` | Não foi possível ativar o mod na pasta do jogo. |
| `game_running` | O Hogwarts Legacy está aberto. Feche o jogo e tente de novo. |

## Build e release

- **`client/`**: `client/core` e os testes entram no build nativo da raiz (alvos `cbmp_client_core`,
  `cbmp_client_tests` e `cbmp-headless`, este em `client/headless/`); os testes do spike são portados para Catch2. A `proto/` ganha o alvo
  `cbmp_codec` (só `bitstream.cpp` + `messages.cpp`; `cbmp_proto` passa a ligar nele). O build do mod continua um
  projeto à parte (`client/CMakeLists.txt`, configuração `Game__Shipping__Win64`) com `client/vcpkg.json`
  (gamenetworkingsockets + nlohmann-json, mesma baseline, triplet `x64-windows-static-md`) e
  `CMAKE_MAP_IMPORTED_CONFIG_GAME__SHIPPING__WIN64=Release`. `client/VERSION` (`0.2.0`) alimenta o DLL e o
  `cbmp-mod.json`.
- **`tools/package-mod.ps1`** (substitui `package-spike.ps1`) monta `dist/cbmp-mod.zip` no layout acima;
  `tools/check-mod-zip.mjs` confere o layout, o `cbmp-mod.json`, as licenças e que o ini está com console/GUI/dump
  desligados (roda no workflow, no `--build-releases` e no `--check`).
- **`.github/workflows/mod.yml`** (contrato do `launcher/README.md`): caminho exato; gatilhos `push` na `main`
  (caminhos `client/**`, `proto/**`, `tools/package-mod.ps1`, `tools/check-mod-zip.mjs`, o próprio arquivo) e
  `workflow_dispatch` **sem entradas**; checkout de `GITHUB_SHA` sem `ref:`; nunca `pull_request_target`. Passos
  (`windows-2025`): vcpkg no commit fixo; UE4SS no commit `4fb9b615…` com submódulos; build;
  `package-mod.ps1`; `check-mod-zip.mjs`; artefato **`cbmp-mod`** com exatamente um `.zip`; resumo com commit e
  sha256. O submódulo `UEPseudo` do UE4SS é privado (conta GitHub ligada à Epic): o workflow usa o segredo
  `UE4SS_SUBMODULES_TOKEN` (só leitura) e falha com mensagem clara se ele faltar. PRs são cobertos pelo `native.yml`
  (núcleo, sem UE4SS), que passa a observar `client/core/**`, `client/headless/**` e `tools/client-e2e.py`.
- **Release oficial**: `release.yml` `kind=mod protocol=1 version=0.2.0 mod_run_id=<execução do mod.yml na main>`
  (sem mudança no workflow).
- **Release de teste (chave dev)**: `tools/teste-online.mjs --build-releases --mod-zip <zip>` (do artefato do
  `mod.yml` via `gh run download`, ou do build local `tools/build.ps1 -Target mod` + `package-mod.ps1`);
  `MOD_VERSION = '0.2.0'`; `dev_sign_mod` ganha `--zip`; `mod-p1-v0.1.0` fica como foi publicado. Launcher `0.2.0`
  entra em `LAUNCHER_VERSIONS` e a `0.1.3` em `PUBLISHED`. `--deploy` leva o `server.cfg` com os limites de
  movimento; `--check` confere que `/v1/mod/latest?protocol=1` entrega a 0.2.0 e o layout do zip.

## Erros e falhas

| Situação | Comportamento | Jogador vê / log prova |
|---|---|---|
| Jogo aberto sem o launcher | Inerte | nada; nenhum arquivo novo |
| Ponteiro velho ou inválido, ou `connect.json` já ausente na abertura | Inerte | nada |
| Processo substituído por outro mais novo | `Superseded` | log: `substituido epoca=1→2` |
| `connect.json` sumiu entre armar e resgatar (venceu os 240 s, outro processo pegou) | `Disconnected(sem_arquivo)` | tela: "desconectado — entrada expirou; feche o jogo e clique Jogar"; log |
| `launchId` ou `protocol` diferente, JSON inválido | `Disconnected(arquivo_invalido / protocolo_diferente)` | idem, com o motivo |
| Ticket vencido antes do uso | `Disconnected(ticket_expirado)` | idem |
| Servidor inalcançável (10 s) | `Disconnected(sem_resposta)` | "desconectado — servidor não respondeu" |
| `Reject` (ticket inválido/usado, banido, cheio, protocolo) | `Disconnected(recusado: texto)` | "desconectado — recusado: …" |
| Sem `Welcome` em 10 s | `Disconnected(sem_welcome)` | idem |
| `Kick` / servidor encerrando / "conectado em outro lugar" | `Disconnected(expulso: texto)` | "desconectado — …" |
| Queda da rede (GNS dá timeout) | `Disconnected(conexao_perdida)` | "desconectado — conexão perdida" |
| Mensagens inválidas do servidor (> 20 em 10 s) ou fila > 4096 | `Disconnected(servidor_invalido)` | log com contadores |
| Servidor não aceita a posição (2ª `Correction` longe com o mesmo `seq`) | segue conectado, sem mover o jogador | tela (uma vez): "o servidor não aceitou sua posição"; log `posicao_recusada` |
| Tela de carregamento / troca de área / viagem rápida | `AwaitWorld`; bonecos soltos; ao voltar, `Input` com `Apparate` e bonecos recriados | log `mundo: Overland → …` |
| Pawn recriado (RootLevel → Overland) | igual ao anterior | log |
| Boneco destruído pelo jogo | recria com espera; desiste após 5 por minuto | log `boneco id=… recriado/desistiu` |
| Falha repetida ao mover/criar bonecos, ou singleton trocado | bonecos desligados na sessão; conexão mantida | log `bonecos_desligados: motivo` |
| Exceção no tick | contida; resumo a cada 10 s; 100 em 60 s → `Disconnected(erro_interno)` | log |
| Jogo fechando | fecha a conexão sem esperar; se não der tempo o servidor remove por timeout (~10 s) | log `encerrando` (se houver tempo) |
| Crash do jogo | minidump em `cbmp\crash\` (ticket já zerado) | arquivo `.dmp` + última linha do log |

Sem reconexão no 5a (ticket de uso único e curto): **para voltar, feche o jogo e clique Jogar de novo**. Desconexão
ocorrida no menu é mostrada uma vez ao entrar no mundo.

## Segurança

- **Servidor não confiável**: toda mensagem passa por `cbmp::decode` (lança em qualquer coisa fora do formato) e
  pelos limites do `Limits`: mensagem recebida ≤ 8 KiB (maior é descartada e contada); no máximo 512 mensagens
  processadas por tick; fila pendente > 4096 ou > 20 erros de decodificação em 10 s → desconecta; ≤ 255 entidades no
  `RemoteStore` e por registro; anel de 32 ticks; `partCount` ≤ 8; nomes ≤ 64 bytes, só para log, com
  `escapeLogText`; textos de `Reject`/`Kick` ≤ 255 bytes, saneados antes de ir para log ou tela.
- **Números**: posições são `int32` do protocolo; nada não finito chega ao motor (conferido antes de cada chamada de
  `IGame`); bonecos só a até 400 m do jogador; no máximo 64 atores; teleporte do jogador local só ≤ 20 m e ≤ 1/s.
- **Nada do servidor vira caminho ou código**: no 5a `Event` é descartado, `contentDir` não é lido, nenhum arquivo é
  aberto por nome vindo da rede.
- **Ticket**: só no `connect.json` (DACL do usuário) e, depois do resgate, só na memória até o `Hello`; zerado em
  seguida; nunca em log, linha de comando, variável de ambiente, ponteiro, memória compartilhada ou minidump (o
  dump passa a `MiniDumpNormal`, sem heap; o dump do UE4SS fica desligado). O log tem um filtro que recusa qualquer
  linha contendo o ticket enquanto ele existir.
- **Arquivos**: o mod grava só em `Win64\cbmp\logs\` e `Win64\cbmp\crash\` (o UE4SS grava o próprio `UE4SS.log`
  em `cbmp\atual\ue4ss\`); lê o ponteiro, o `cb-mp.json` e o
  `connect.json`; apaga só o `connect.json`. `runDir` do ponteiro é validado e o arquivo tem de ser comum, pequeno e
  do próprio usuário.
- **Contenção**: toda chamada ao jogo passa por `SafeCall`; o tick inteiro está em `try/catch(...)`; falhas
  repetidas desligam o recurso (bonecos, status) em vez de insistir.
- **Código**: o pacote assinado é a única fonte de código; o launcher confere assinatura, hashes e ativação a cada
  Jogar; o modo elevado reconfere a assinatura. `Hello.buildHash` leva o sha256 do `main.dll` (S6; o servidor ainda
  não recusa por ele).
- **Licenças e origem**: UE4SS (MIT) com licença e avisos no pacote; nada de MafiaHub, arcane-mp ou HogwartsMP;
  Dumper-7 só como ferramenta externa, se um dia for usado; dados derivados do jogo nunca entram no repositório
  (`.gitignore` + regra de revisão: só nomes curtos em código e spec).
- **Usuário**: o launcher não apaga nem sobrescreve arquivo que não é dele; conflito só anda com confirmação.

## Testes

**Automáticos (sem o jogo)**

- `cbmp_client_tests` (Catch2, Windows e Linux/ASAN) com `FakeGame`, `FakeTransport`, `FakeGate`, relógio falso:
  - `ConnectFile`: ponteiro e `connect.json` válidos e cada recusa (tamanho, `launchId`, protocolo, IP, caminho);
  - `Session`: cada transição e cada linha da tabela de erros; ticket zerado depois do `Hello`; nada enviado em
    `AwaitWorld`; regra de resgate (45 s / 5 s / `InWorld` / substituído);
  - `SnapshotMirror`: partes fora de ordem, tick antigo, baseline ausente, `EntityLeave` limpando o histórico, id
    desconhecido; **teste espelho** contra `makeSnapshotEntity`/`splitSnapshot` com perda de 30% (estado do cliente
    = estado do servidor a cada tick confirmado) — fecha a pendência "Teste espelho fim a fim";
  - `Interp`: atraso de 100 ms, extrapolação limitada, salto, yaw pelo menor arco, relógio com jitter;
  - `LocalSampler`: NaN/inf, saturação, mapeamento de estado, salto → `Apparate`;
  - `PuppetDirector`: teto, raio, `Gone`, espera e desistência, singleton trocado, mundo trocado;
  - `Limits`: mensagens grandes, enxurrada, erros de decodificação; log nunca contém o ticket.
- **Integração `client-e2e`** (`tools/client-e2e.py`, no `native.yml`, Linux e Windows): sobe o `cb-server` real em
  modo dev com a mesma configuração de movimento do servidor de teste (spawn na origem + `apparate` alto) e dois
  `cbmp-headless` (núcleo real + `GnsTransport` + `ScriptedGame`; tickets pelo gerador dos bots). A anda 20 m em
  linha reta a 300 cm/s; passa se B criou o boneco de A, se a trajetória do boneco fica a ≤ 60 cm da de A atrasada
  em 100–300 ms, sem passo > 40 cm entre quadros de 16 ms depois do primeiro, e se o boneco some em ≤ 15 s quando A fecha. Repetido com
  perda falsa de 20% (configuração do GNS). Um terceiro caso derruba o servidor e confere `conexao_perdida`.
- **Launcher** (`cargo test` + Vitest): layout v2 e `cbmp-mod.json`; ativação (ausente, nosso, histórico, estranho);
  conflito com e sem confirmação; remoção devolvendo backup e sem tocar em arquivo estranho; migração do layout
  antigo; ponteiro e `launchId`; TTLs; `game_running`; migração do `simulateLaunch`; validação dos argumentos do
  modo elevado (sem elevar); `config_guard` com as constantes novas.
- **Guardas**: `check-mod-zip.mjs` com zips bons e ruins; teste que falha se algo de `client/core` incluir UE4SS ou
  `Windows.h`; teste do `mod.yml` (caminho, gatilhos, sem entradas, sem `ref:`).

**Manual no jogo** (uma rodada, dois testadores, servidor de teste online; roteiro em `docs/TESTE-NO-JOGO-5A.md`)

1. Os dois: abrir o launcher (atualiza para 0.2.0), Configurações → pasta do jogo certa, Jogar no servidor de teste.
   Se aparecer conflito ou pedido de administrador, seguir a tela e anotar.
2. Carregar um save (não começar jogo novo) e ir até o pátio de Hogsmeade. Quem demorar mais de 4 min até o menu
   fecha o jogo e clica Jogar de novo.
3. Conferir a mensagem "CB Multiplayer: conectado". Chegar perto um do outro: cada um vê um bruxo extra.
4. Um anda 30 s em volta do outro, corre, pula, para; trocam. Anotar: desliza? treme? atrasa quanto?
5. Um usa viagem rápida para outro ponto e volta a pé/voando; o boneco some e reaparece?
6. Um fecha o jogo: o boneco some para o outro em ~15 s? O outro continua jogando normal por 2 min.
7. O que ficou reabre pelo launcher e os dois repetem o passo 3.
8. Abrir o jogo **pela Steam/Epic, sem o launcher**: joga normal, sem mensagem, sem log novo em `cbmp\logs`.
9. Configurações → Remover mod do jogo; conferir que `dwmapi.dll` saiu de `Win64` e o jogo abre normal.
10. Enviar `Win64\cbmp\logs\`, `Win64\cbmp\crash\` (se houver) e a pasta de logs do launcher.

O que os logs têm de provar: dois arquivos de log por abertura (um `substituido`, um `dono`), com os tempos entre
início, primeiro tick e resgate; `Hello` → `Welcome playerId=…`; `mundo: … InWorld`; `EntityEnter id=…` e
`boneco id=… criado`; contadores a cada 10 s (snapshots, acks, correções, bonecos, erros de jogo = 0);
`singleton=ok`; no passo 5, `Input Apparate` e nenhuma expulsão; no passo 6, `EntityLeave` e `boneco destruído`;
no passo 8, nenhum arquivo. No servidor: `journalctl` sem `movimento inválido`.

## Critério de pronto

1. `native.yml` verde com `cbmp_client_tests` e `client-e2e` (Linux e Windows); `launcher.yml` verde.
2. `mod.yml` gera o artefato `cbmp-mod` com um zip que passa no `check-mod-zip.mjs`.
3. `mod-p1-v0.2.0` e o launcher `0.2.0` publicados no ambiente de teste (chave dev); `teste-online.mjs --check`
   verde; servidor de teste com a configuração de movimento nova.
4. Numa máquina sem o jogo: o launcher instala em uma pasta de jogo falsa, ativa, grava o ponteiro, detecta
   conflito, remove e devolve o backup (teste de integração do launcher).
5. `docs/TESTE-NO-JOGO-5A.md`, `launcher/README.md`, `docs/AMBIENTE-TESTE-ONLINE.md`, `docs/COMO-FUNCIONA.md` e
   `docs/PENDENCIAS.md` atualizados (itens fechados saem; os novos entram).
6. Rodada manual feita depois, em lote: passos 3, 4, 6 e 8 aprovados = 5a validado. Os demais passos viram
   pendência registrada se falharem, sem reabrir o 5a.

## Riscos e o que só o jogo responde

| Dúvida | Como o desenho degrada |
|---|---|
| Qual dos dois processos sobrevive e quanto tempo passa entre eles em outras máquinas | regra da época + 45 s/5 s; se errar, a sessão não começa e o log dos dois processos mostra os tempos para ajustar `kClaimDelay`; jogar de novo resolve na maioria dos casos |
| Clonar a classe do jogador mexe em sistemas do jogo (singleton, HUD, câmera, percepção) | conferência do singleton a cada criação; bonecos desligados na sessão com a conexão mantida; `neutralizacaoExtra` para experimentar; classe de NPC como boneco fica para o 5b |
| Custo de N clones (36 componentes cada) | teto de 32; só a ≤ 400 m; medir quadros no teste; 5b troca por classe leve |
| `K2_SetActorLocationAndRotation` todo tick com colisão desligada briga com algum sistema (tecido, áudio, ODC) | duas alternativas de movimento; erros repetidos desligam os bonecos |
| `AddTextTickerNotification` aceita texto livre? | falha desliga só o status; `statusNaTela: false`; log é a prova |
| Critério de `InWorld` (outros mundos de menu/intro, criação de personagem) | lista de mundos de menu + dica do `UIManager` + estabilidade + posição; se errar para "nunca", não envia posição (log `mundo` mostra o nome para corrigir) |
| Limites de velocidade reais (vassoura, montaria, queda) | limites folgados no servidor de teste; correção curta só reposiciona ≤ 20 m; cada `Correction` vai para o log com a distância para calibrar |
| UE4SS sempre carregado no single-player; atualização do jogo pode quebrar o UE4SS | "Remover mod do jogo"; carregador próprio que só arma pelo launcher fica registrado em `PENDENCIAS` (dono 5b) |
| Epic: permissão em `Program Files`, abrir pelo executável, UE4SS gravando o próprio log em pasta só leitura | elevação única ou erro claro; nada disso verificado; primeira Epic real decide |
| Mais de 4 min até o primeiro tick (compilação de shaders) | `sem_arquivo` com instrução de jogar de novo; renovação contínua do ticket pelo launcher fica como pendência |
| CI sem acesso ao `UEPseudo` | `mod.yml` falha claro; release de teste sai do build local na máquina de desenvolvimento |
| Jogador parado na origem enquanto está no menu; cliente pode declarar `Apparate` no servidor de teste | aceito no 5a; 5e traz spawn pelo gamemode e teleporte autorizado |

## Fica para depois

5b: animação/aparência, classe de boneco leve, estados de movimento reais, enxugar ganchos do UE4SS, carregador
próprio. 5c: chat/comandos/Lua de cliente (`cb:command`, `cb:reply`, uso do `contentDir`). 5d: UI HTML. 5e: mundo,
spawn pelo servidor, teleporte longo seguro, entrar sem save, limites de movimento calibrados. 5f: feitiços e
interações. Fora das etapas: reconexão, renovação contínua do ticket, lista de builds aceitos (S6), carência do
ticket medida com dados reais (pendência M-1).
