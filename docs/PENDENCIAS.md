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
  - DNS em threads de resolução próprias (desanexadas), com o prazo do pedido: a thread do pool não fica presa e
    cancelar/parar o resource a solta na hora. Mas `getaddrinfo` não é cancelável: a resolução presa continua até o
    resolvedor do SO desistir (glibc: até ~30 s) e conta num teto de 16 no pool e 2 por resource (geração); acima
    disso o pedido falha na hora ("resolvedor DNS ocupado"). Um resource que se reinicia várias vezes com DNS travado
    pode encher o teto global por esse tempo.
  - `httplib::Client::stop()` não interrompe um `connect` em andamento (o socket só existe depois): cada endereço tem
    no máximo 3 s de connect, então parar um resource devolve a thread em ≤ 3 s. IPv4 antes de IPv6 (sem Happy
    Eyeballs).
  - Um resource ocupa no máximo 2 das 4 threads ao mesmo tempo (`maxInFlightPerOwner`); a fila é em rodízio entre
    resources.
  - TLS no Windows: a verificação do Schannel (`CPPHTTPLIB_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE`, ligada por
    padrão no httplib 0.65) está DESLIGADA por pedido (`enable_windows_certificate_verification(false)`): ela fazia o
    CryptoAPI buscar as URLs AIA/CRL/OCSP do certificado do servidor (SSRF por fora da checagem de endereços;
    comprovado por teste). A cadeia é verificada pelo OpenSSL contra a cópia das lojas ROOT+CA do Windows feita a
    cada pedido. Consequências: raízes que o Windows só baixaria sob demanda (não presentes na loja local) e
    servidores que não mandam o intermediário falham; não há checagem de revogação (nem no Linux).
  - Logs e erros devolvidos ao Lua só levam `scheme://host[:porta]` (nunca caminho/query: tokens de webhook).
  - Cabeçalhos da resposta: o httplib lê até 100 linhas de 8 KiB (~800 KiB por pedido no pior caso; ≤ ~3,2 MiB com
    4 threads) antes de o pool conferir o teto de 64 KiB (recusa depois de ler). Baixar isso exigiria
    `CPPHTTPLIB_HEADER_MAX_*` globais (o resto do servidor usa o mesmo header).
  - Teto de 4 MiB por resource conta corpo do pedido + resposta recebida até a resposta sair do pool; a resposta já
    entregue fica na caixa de entrada até o próximo onTick (limitada por `maxHttpPending` = 64 por resource).
  - Cada pedido cria um `SSL_CTX` e reimporta a loja do Windows (CPU por pedido; sem cache).
  - CAs: Linux usa os caminhos padrão do OpenSSL → a distribuição Linux precisa de `ca-certificates`.
    `HttpOutOptions::caFile` existe, mas não há chave no `server.cfg`. O caminho das CAs do sistema só é exercitado
    pelo teste de AIA (recusa); não há teste de sucesso contra uma CA do sistema (sem rede externa nos testes).
- Login de admin via Hub (Task 7 + fix 1), resíduos e decisões:
  - GET `server-admin` só confirma; o ticket sai no POST com Origin = Hub e token CSRF. Um clique a mais no fluxo
    legítimo. A página de confirmação usa `Referrer-Policy: same-origin` (com `no-referrer` o navegador manda
    `Origin: null` no POST e todo login falharia); o resto da rota, inclusive o 303, segue `no-referrer`. Não há
    teste com navegador real no repo (só `inject`); o comportamento foi reproduzido no Chromium na revisão.
  - Chave do HMAC do CSRF é aleatória por processo: restart do Hub, ou várias instâncias atrás de um balanceador,
    fazem uma confirmação aberta falhar com 403 (o usuário refaz em 1 clique). Mesma premissa de instância única
    dos limitadores; com várias instâncias, derivar a chave de um segredo de config.
  - `adminUrl` IPv6 literal: a gramática `host-source` da CSP não aceita `[..]`, então a página de confirmação usa
    `form-action 'self' https:` (ou `http:` em dev loopback) só nesse caso. O destino continua fixado pelo Hub; a
    CSP é defesa extra.
  - Ticket de admin é emitido para qualquer usuário logado (inclusive quem não é dono): quem decide é o servidor
    (`admin.admins`). Ban global não bloqueia o login de admin.
  - `adminUrl` inválida, ou loopback/IP privado/`localhost`/`*.localhost` sem `HUB_ALLOW_PRIVATE_SERVERS=1`, é
    ignorada (o servidor segue listado; `adminUrlIgnored` na resposta; aviso no log do Hub e do cb-server). Nomes DNS
    não são resolvidos: um nome público que aponte para 127.0.0.1 passa (só o fragmento vai ao destino que o dono
    escolheu). IPv4 mapeado em IPv6 é sempre tratado como não público.
  - Sem IDN: rótulos `xn--` são recusados nos dois lados (evita homógrafos na confirmação e divergência de
    validação de punycode entre C++ e o parser WHATWG). IPv4 só decimal estrito (`127.1`, `0x7f…`, `0177…`
    recusados).
  - Limites (10 tickets/min por usuário, 30 req/min por IP com IPv6 por /64) em memória do processo: janela
    deslizante aproximada, até 50 000 chaves por janela (acima disso chaves novas levam 429); zeram ao reiniciar e não
    valem entre várias instâncias.
  - Auditoria em logger filho com nível `info` fixo: aparece mesmo com `HUB_LOG_LEVEL=warn/error`; some só com
    `silent`.
  - O Hub não sonda o painel admin (não confere que responde em `/auth/callback`).
  - ~~**Para a Task 8 (painel no cb-server):** `state` em cookie `SameSite=Strict`, conferido no login; `admin.port`/
    `bind`/`admins` no `ServerConfig`; jti de admin com cache próprio.~~ Resolvido na Task 8 (state de 32 bytes em
    cookie HttpOnly Path=/auth 10 min, comparado em tempo constante com o do corpo e de uso único também no servidor;
    jti guardado até exp + 60 s, teto de 10 000; config validada).
- Painel admin (Task 8), resíduos e decisões:
  - Sessões, states, jti e limites de taxa só em memória do processo: restart do cb-server desloga todo mundo.
  - Limites de login (fix 1 da revisão): só falhas gastam (por IP e global); ticket + state válidos entram sempre;
    sem orçamento o devToken nem é comparado; `/auth/start` com limite próprio por IP (60/min) e até 4096 states
    pendentes (o mais antigo sai). Atrás de proxy, sem `admin.trustedProxies`, os limites por IP valem para o proxy
    inteiro.
  - O state só prova que *algum* navegador começou o fluxo: um ticket roubado pode ser pareado com o state do próprio
    atacante. Fechar isso exige o Hub amarrar o hash do state no ticket (claim `nonce`) — mudança no Hub/contrato.
  - Ação do painel e comando de script para o mesmo jogador no mesmo tick: a coalescência do canal prioritário mantém
    o último teleporte (o do script vence).
  - Ban pelo painel: o kick por did sai na hora; a gravação vai pela thread de scripts e o painel espera até 3 s pelo
    resultado (200 gravado / 503 falhou / 202 em andamento). Se o host parar no meio, o jogador já foi expulso mas o
    ban não fica gravado (a auditoria registra `failed`).
  - Pool do painel: 8 threads, fila de 32, keep-alive 2 s. O httplib segura uma thread por conexão: exposto direto,
    poucas conexões lentas travam o painel (o jogo não é afetado). Documentado: só atrás de proxy que bufferiza.
  - Filtro de logs: entre a checagem de symlink e a abertura do arquivo há uma janela (TOCTOU) que exige escrita
    local em `logsDir`; aceito.
  - O proxy precisa repassar o `Host` original (`proxy_set_header Host $host`): o painel recusa (421) Host fora da
    lista (origem da `publicUrl`; nomes de loopback só com bind loopback). Bind curinga sem `publicUrl` = 421 em tudo
    (aviso no log).
  - CSRF: Origin do painel + `X-CB-Admin: 1` + JSON (sem token por sessão). O JS manda `referrerPolicy:
    'same-origin'` nos fetch (com `no-referrer` o navegador poderia mandar `Origin: null`). No Chromium o Origin veio
    certo nos dois casos (verificação manual).
  - Filtro de linhas de log com a senha do banco: substring da senha crua e da forma escapada em JSON; senha com
    menos de 4 caracteres desliga o filtro (aviso no log). Só `audit` e `anticheat` (o `server.log` não é exposto).
  - Comando de console de resource que não devolve texto (ou demora > 2 s) responde `pending`; a resposta tardia se
    perde (fica só no log/auditoria do resource).
  - ~~Console aceita comandos sem `admin = true`.~~ Resolvido no fix 1: o console do painel recusa comandos que não
    são de admin.
  - Sem teste automatizado com navegador real (Playwright etc.); o fluxo completo foi verificado à mão no Chromium do
    app (relatório da Task 8).
  - Sem `/metrics` para o HTTP (`HttpOutPool::metrics()` existe; falta expor).
  - Redirecionamento para outro host/porta descarta os cabeçalhos do script e recusa 307/308 com corpo; 301/302 de
    POST e 303 viram GET sem corpo. `Expect: 100-continue` automático do httplib para corpos ≥ 1 KiB.
  - `permissions.http`: maiúsculas, IP, curinga e não ASCII recusados no `resource.json` (antes maiúsculas eram
    convertidas). Porta diferente de 443 só com `"host:porta"`.
  - `cb.http` dentro de `cb.db.transaction` é recusado (a transação seguraria a conexão do banco).
  - Pool do banco (Task 5, M10): continua com fila FIFO única. Recomendação: quando valer a pena, usar o mesmo desenho
    do `HttpOutPool` (fila por resource em rodízio + teto de conexões por resource). Prioridade baixa: cada resource
    já tem teto de 1024 pedidos / 16 MiB no banco.
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
- Limites explícitos do HTTP do jogo (`/info`, `/cb/*`, `/metrics`: `set_payload_max_length`, timeouts, pool). O
  painel admin (Task 8) e o HTTP de saída dos scripts (`cb.http`, Task 6) já têm os seus.
- Teste manual de **512 bots em 4 vCPUs** (`docs/LOAD-TEST.md`).
- Hub: migrar para zod 4; purge e observabilidade já feitos; revisar erros em inglês do Fastify.
- Domínio, hospedagem do Hub e repositório/releases públicos.

## Mod do jogo (spike)

- `dist/cb-mp-spike.zip` aguardando teste em PC com Hogwarts Legacy.
