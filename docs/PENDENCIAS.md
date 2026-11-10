# Pendências conhecidas

Itens levantados nas revisões que não bloquearam o merge, com o momento em que precisam ser resolvidos.

## Antes do launcher (subprojeto 4)

- **HTTP do `cb-server` só em IPv4** (`server/src/main.cpp`, bind `0.0.0.0`), enquanto o jogo (GNS) aceita IPv6 e o
  Hub pode anunciar `host` IPv6. Fazer bind dual-stack (`::` com `IPV6_V6ONLY=0`) ou nos dois endereços.
- **Nome do resource** (`resource.json` → `name`) sem validação de charset; o launcher deve sanitizar de qualquer forma,
  mas validar no servidor com a mesma regra dos caminhos.
- Pasta de exemplo de resources versionada (`server/resources.example/`) para o launcher testar download.

## Antes do cliente no jogo (subprojeto 5)

- **Contrato do cliente por escrito**: snapshots não confiáveis em partes; descartar tick ≤ último montado; ignorar ids
  desconhecidos; `EntityEnter/Leave` não carregam tick; reconstruir estado a partir do histórico por tick; confirmar só
  ticks completos; adotar `Correction` (inclusive a de spawn); carência de 30 s após correções do servidor.
- **Teste espelho fim a fim**: cliente simulado aplicando baselines/partes com perda, comparado ao estado do servidor
  (os bots atuais confirmam ticks sem aplicar baseline).
- **Estados de movimento confiáveis**: hoje o cliente declara o estado (ex.: `Broom`, `Fall`) e com isso amplia o limite
  de velocidade; velocidade do input é repassada sem validação. Definir quando cada estado é permitido.
- Velocidades por estado são provisórias; calibrar com dados do jogo.
- Quantização com NaN/inf no lado do cliente.

## Subprojeto 3 (Lua / MySQL / admin)

- ~~`World` sem gancho para rotear eventos para Lua e sem API para enviar `Event` servidor → cliente.~~ Resolvido
  na Task 4 (`WorldHooks`, `queueEvent`/`broadcastEvent`, `ScriptBridge`).
- `addPlayer` sempre na origem fora do modo dev: spawn deve vir do gamemode.
- Lua deve rodar na thread da simulação ou por fila (`World`/`Authenticator` não são thread-safe).
- Exceção dentro de `world.tick` derruba o servidor (sem try/catch no loop). **Parcial (Task 4, fix 1):** mensagem
  não codificável é descartada e contada em `NetServer::send` (`encodeErrors`); a ponte valida nome/payload dos
  eventos de script; `applyCommands` tem try/catch. O `world.tick` em si continua sem try/catch.
- Chamada `cb.resource` a um export que trava: o prazo da vigia do chamador também corre, então chamador e alvo levam
  uma falta (os dois reiniciam com back-off). Documentar em `RESOURCES.md`; separar os prazos exigiria mudar o `Resource`.
- Foto dos jogadores por tick: buffers reaproveitados quando a thread de scripts já soltou a foto antiga (até 4); ainda
  copia as strings de cada jogador por tick (`assign`, sem nova alocação no caso comum).
- Comando de chat que cede (consulta assíncrona da Task 5) não responde: só retorno síncrono vira `cb:reply`.
- Eventos servidor → cliente de scripts: cota por jogador de `scripting.eventOutKBps` (padrão 8 KiB/s = 64 kbps) com
  rajada `eventOutBurstKB` (32 KiB) e 48 B de overhead por mensagem; teto de 8192 eventos de saída por tick. Um
  gamemode que precise de mais banda por jogador tem de subir a cota (e conferir o orçamento de 64 kbps da spec).
- `kick`/`setPosition`/kick por did dos scripts vão por um canal prioritário do host (nunca atrás dos emits); teto de
  segurança de 65 536 com log CRÍTICO.
- Política de teleporte (`setState` limpa `correctionTimes`; decidir se mantém o histórico).
- Teto com kick para cliente que nunca alcança correção do servidor.
- ~~Evento `echo` é de teste e fica ativo em produção: tornar só-dev.~~ Resolvido na Task 4: o eco C++ saiu; só existe
  no resource de exemplo `resources.example/echo` (fora da pasta de resources de produção).
- `kid`/`name` vindos do Hub vão para o log sem remover caracteres de controle.

## Antes de abrir ao público

- Limite de taxa de **novas conexões por IP** (hoje só há limite de pendentes).
- Teto de saída por cliente (`net.maxOutKbpsPerClient` é lido mas não aplicado).
- Limites explícitos do HTTP (`set_payload_max_length`, timeouts, pool).
- Teste manual de **512 bots em 4 vCPUs** (`docs/LOAD-TEST.md`).
- Hub: migrar para zod 4; purge e observabilidade já feitos; revisar erros em inglês do Fastify.
- Domínio, hospedagem do Hub e repositório/releases públicos.

## Mod do jogo (spike)

- `dist/cb-mp-spike.zip` aguardando teste em PC com Hogwarts Legacy.
