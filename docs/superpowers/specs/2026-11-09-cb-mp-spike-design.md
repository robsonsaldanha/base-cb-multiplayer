# CB-MP — Spike de injeção (Subprojeto 1) — Design

Data: 2026-11-09
Status: aprovado em conversa, aguardando revisão da spec

## Contexto

O Castelo Bruxo RP hoje roda sobre o HogwartsMP/arcane-mp (MafiaHub Framework). A licença da MafiaHub proíbe
reaproveitar o código deles e proíbe launcher próprio sem autorização escrita. Decisão: construir multiplayer
próprio do zero ("CB-MP"), sem copiar nem portar código da MafiaHub/arcane-mp.

Restrições:
- Alvo: Hogwarts Legacy (Unreal Engine 4), versões **Steam e Epic**.
- Desenvolvimento: Claude escreve o código (C++/TS); Robson testa.
- O jogo **não está instalado** no PC de desenvolvimento. Build aqui (Visual Studio 2026), teste em outro PC.

## Projeto completo (decomposição)

| # | Subprojeto | Entrega |
|---|------------|---------|
| 1 | Spike de injeção (esta spec) | mod lê posição do jogador e cria um clone |
| 2 | Netcode + sync | 2 jogadores se vendo e andando |
| 3 | Servidor + scripting | `cb-server` com runtime Node/TS para gamemodes |
| 4 | Launcher | detecta jogo, atualiza, instala, conecta |
| 5 | Migração de gamemodes | reescrever `hp-*`/`hmp-*`/`cb-*` na API própria |

Cada subprojeto tem spec e plano próprios.

## Arquitetura geral (alvo final)

```
PC do jogador                                   VPS
CB Launcher (Tauri) ──HTTPS──► API/Masterlist (versões, servidores)
   │ instala UE4SS + cb-mp, abre o jogo
   ▼
Hogwarts Legacy
 └ UE4SS (MIT)
    └ cb-mp.dll  ──UDP (GameNetworkingSockets)──► cb-server (C++ + runtime Node/TS)
       ├ game/  camada que conhece UE4SS/Unreal
       ├ net/   cliente de rede
       └ sync/  replicação de jogadores
```

Regras:
- **Só `game/` conhece UE4SS/Unreal.** `net/` e `sync/` usam apenas a interface `IGame` (nossa). Trocar a base
  (ex.: DLL própria com MinHook + SDK gerado por Dumper-7) deve afetar só `game/`.
- Protocolo binário versionado em `shared/`, usado por cliente e servidor.
- Monorepo em `Launcher_livre/`: `client/`, `server/`, `shared/`, `launcher/`, `docs/`. Build C++: CMake + vcpkg.

Dependências permitidas (licenças conferidas em 2026-11-08):
- RE-UE4SS — MIT (base de injeção/reflexão).
- GameNetworkingSockets — BSD-3 (rede, subprojeto 2).
- ENet — MIT (alternativa de rede).
- MinHook — BSD-2 (só se sair do UE4SS).
- Dumper-7 — sem licença: **usar só como ferramenta** para gerar SDK; não copiar código.
- Proibido: qualquer código MafiaHub Framework / arcane-mp / HogwartsMP.

## Escopo do spike

Dentro:
- `client/` como mod C++ do UE4SS (`cb-mp.dll`).
- `client/src/game/`: interface `IGame` + implementação `Ue4ssGame`.
- Interface mínima:
  - `std::optional<PlayerHandle> GetLocalPlayer()`
  - `Transform GetTransform(PlayerHandle)` (posição + rotação)
  - `std::optional<PlayerHandle> SpawnClone(const Transform&)` — cria um personagem parado usando a mesma
    classe/aparência do jogador local
  - `void Destroy(PlayerHandle)`
- Logger em arquivo `cb-mp.log` (ao lado do mod), com timestamp e nível.
- Tecla **F8**: cria clone 2 m à frente do jogador; segunda vez remove.
- Pacote `cb-mp-spike.zip`: UE4SS + `cb-mp.dll` + `INSTALAR.bat` + `DESINSTALAR.bat` + `LEIA-ME.txt`.

Fora (YAGNI): rede, servidor, launcher, animação/sincronização, outros jogadores, UI.

## Critério de sucesso

No PC de teste, com o zip instalado, ao carregar um save:
1. `cb-mp.log` registra loja (Steam/Epic), hash do executável, versão do UE4SS.
2. Registra posição/rotação do jogador **1x por segundo**, e os valores mudam ao andar.
3. F8 faz aparecer um clone parado do bruxo; F8 de novo remove; o jogo não crasha.
4. Funciona nas duas lojas (Steam e Epic) — pelo menos uma testada no spike, a outra registrada como pendente
   se não houver cópia disponível.

## Tratamento de erros

- Toda chamada de `game/` protegida (SEH + try/catch); falha vira linha `ERRO` no log, não crash.
- Se o jogador local não for encontrado em 30 s após carregar o save: log `ERRO: player não encontrado`
  com detalhes (classes procuradas, objetos encontrados) e o mod se desativa.
- Handler de crash grava minidump `cb-mp-<data>.dmp` junto ao log.
- Instalação: `INSTALAR.bat` faz backup de qualquer arquivo que sobrescreve; `DESINSTALAR.bat` restaura.

## Testes

- Aqui (sem jogo): build do `cb-mp.dll` em Release x64 deve passar sem warnings novos; testes unitários do
  logger e de utilitários puros (formatação de transform, parsing de config).
- No PC de teste (Robson): roteiro em `LEIA-ME.txt` — instalar, carregar save, andar 10 s, F8, F8, sair; enviar
  `cb-mp.log` (e `.dmp` se houver).
- O log é a principal ferramenta de diagnóstico; deve ser verboso no spike.

## Riscos

- Patch do jogo quebra o UE4SS → aguardar/atualizar UE4SS; nosso código isolado em `game/`.
- Spawn de personagem pode exigir mais que `SpawnActor` (componentes de aparência) → spike aceita clone com
  aparência padrão se a cópia da aparência falhar, registrando no log.
- Termos de uso da WB e marca "Hogwarts": não usar "Hogwarts" no nome do produto. Não é parecer jurídico.
