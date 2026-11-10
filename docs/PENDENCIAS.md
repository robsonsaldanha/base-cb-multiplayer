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
- ~~Comando de chat que cede (consulta assíncrona da Task 5) não responde: só retorno síncrono vira `cb:reply`.~~
  Resolvido na Task 5: o comando que espera o banco responde com o texto que devolver no fim.
- Banco (Task 5): bans com MySQL só são lidos do banco na subida (carga com novas tentativas); bans gravados por
  outro servidor no mesmo banco só valem depois de reiniciar. Gravações de ban com o banco fora ficam em fila na
  memória (teto 100 000) e se perdem se o processo cair antes de o banco voltar.
- Banco (Task 5): `cb.db` dentro de corrotinas do próprio script (`coroutine.wrap`) cede para o `resume` do script;
  quando o resultado chega, o host retoma a corrotina (o "resume" do script recebe só a cessão). Documentar em
  `RESOURCES.md`; resultado ≤ 10 000 linhas / 8 MiB; CALL com conjuntos de resultado não é suportado.
- Distribuição do `cb-server` precisa levar `libmariadb.so*`/`libmariadb.dll` (LGPL, ligada dinamicamente; no Linux
  pelo triplet `vcpkg-triplets/x64-linux.cmake`) **e a pasta `plugins/libmariadb/` ao lado do executável** (o build
  já a copia; sem ela, MySQL 8 com `caching_sha2_password` e contas `ed25519` não conectam).
- Banco (Task 5, fix 1), documentar em `RESOURCES.md`/config:
  - tempo esgotado em `exec`/`insert`/COMMIT diz "resultado incerto" (o comando pode ter sido aplicado);
  - corrotina filha (`coroutine.wrap`) dentro de `cb.db.transaction` usa outra conexão (fora da transação) e não é
    barrada pela regra de não aninhar;
  - colunas com o mesmo nome se sobrescrevem na linha (use alias);
  - `db.pool: 1` + transação segura a única conexão (aviso no log); fila do pool é FIFO única, sem justiça entre
    resources (cada resource limitado a 1024 pedidos e 16 MiB pendentes);
  - `db.tls: "off"` só desliga a verificação; não impede o servidor de negociar TLS se exigir;
  - MySQL (não MariaDB): `max_execution_time` só vale para SELECT; o resto depende do timeout de leitura do cliente;
  - subida com o banco fora: bans do banco não valem até a carga (log CRÍTICO após 5 falhas; `/metrics.db.bansLoaded`).
- HTTP de saída (Task 6, `cb.http`), resíduos e decisões:
  - `getaddrinfo` não é cancelável: DNS lento segura uma thread do pool (4) até o resolvedor do SO desistir. O script
    recebe "tempo esgotado" pelo prazo do lado do script (fila 10 s + execução 10 s + 1 s); no encerramento a thread
    presa é desanexada (log) e termina sozinha.
  - `httplib::Client::stop()` não interrompe um `connect` em andamento (o socket só existe depois): endereço que não
    responde segura a thread até o connection timeout (restante do prazo; no máximo 3 s se houver outro endereço na
    reserva). IPv4 é tentado antes do IPv6 (sem Happy Eyeballs).
  - Cabeçalhos da resposta: o httplib lê até 100 linhas de 8 KiB (~800 KiB por pedido no pior caso) antes de o pool
    conferir o teto de 64 KiB (recusa depois de ler). Baixar isso exigiria `CPPHTTPLIB_HEADER_MAX_*` globais (o resto
    do servidor usa o mesmo header).
  - Teto de 4 MiB por resource conta corpo do pedido + resposta recebida até a resposta sair do pool; a resposta já
    entregue fica na caixa de entrada até o próximo onTick (limitada por `maxHttpPending` = 64 por resource).
  - CAs: repositório do sistema (Windows ROOT+CA; Linux caminhos padrão do OpenSSL → a distribuição Linux precisa de
    `ca-certificates`). `HttpOutOptions::caFile` existe, mas não há chave no `server.cfg`.
  - Sem `/metrics` para o HTTP (`HttpOutPool::metrics()` existe; falta expor).
  - Redirecionamento para outro host/porta descarta os cabeçalhos do script (credenciais); 301/302 de POST e 303 viram
    GET sem corpo. `Expect: 100-continue` automático do httplib para corpos ≥ 1 KiB.
  - `permissions.http`: maiúsculas, IP, curinga e não ASCII agora são recusados no `resource.json` (antes maiúsculas
    eram convertidas). Porta diferente de 443 só com `"host:porta"`.
  - `cb.http` dentro de `cb.db.transaction` é recusado (a transação seguraria a conexão do banco).
  - Pool do banco (Task 5, M10): continua com fila FIFO única. Recomendação: quando valer a pena, usar o mesmo rodízio
    por dono do `HttpOutPool` (fila por resource; a vez de outro dono vem antes do 2º pedido de quem acabou de ser
    atendido). Prioridade baixa: cada resource já tem teto de 1024 pedidos / 16 MiB no banco.
  - TLS só foi exercitado contra servidor local com certificado gerado no teste (`caFile`); o caminho das CAs do
    sistema não tem teste automatizado (sem rede externa nos testes).
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
- Limites explícitos do HTTP do servidor (`/metrics`, painel: `set_payload_max_length`, timeouts, pool). O HTTP de
  saída dos scripts (`cb.http`) já tem os seus (Task 6).
- Teste manual de **512 bots em 4 vCPUs** (`docs/LOAD-TEST.md`).
- Hub: migrar para zod 4; purge e observabilidade já feitos; revisar erros em inglês do Fastify.
- Domínio, hospedagem do Hub e repositório/releases públicos.

## Mod do jogo (spike)

- `dist/cb-mp-spike.zip` aguardando teste em PC com Hogwarts Legacy.
