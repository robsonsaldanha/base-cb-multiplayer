# Pendências conhecidas

Itens levantados nas revisões que não bloquearam o merge, com o momento em que precisam ser resolvidos e o
subprojeto (ou área) sugerido como dono. Itens resolvidos saem desta lista (o histórico fica no git e nos relatórios
das tasks).

## Antes do cliente no jogo (subprojeto 5)

- **Contrato do cliente por escrito**: snapshots não confiáveis em partes; descartar tick ≤ último montado; ignorar ids
  desconhecidos; `EntityEnter/Leave` não carregam tick; reconstruir estado a partir do histórico por tick; confirmar só
  ticks completos; adotar `Correction` (inclusive a de spawn); carência de 30 s após correções do servidor. Incluir
  os eventos do motor: `cb:command` (payload = texto JSON) e `cb:reply`.
- **Teste espelho fim a fim**: cliente simulado aplicando baselines/partes com perda, comparado ao estado do servidor
  (os bots atuais confirmam ticks sem aplicar baseline).
- **Estados de movimento confiáveis**: hoje o cliente declara o estado (ex.: `Broom`, `Fall`) e com isso amplia o limite
  de velocidade; velocidade do input é repassada sem validação. Definir quando cada estado é permitido.
- Velocidades por estado são provisórias; calibrar com dados do jogo.
- Quantização com NaN/inf no lado do cliente.
- **Spawn vindo do gamemode**: `addPlayer` sempre na origem fora do modo dev; o gamemode (Lua) deveria decidir o
  spawn (hoje só dá para teleportar depois com `cb.players.setPosition`).
- **Teto com kick para cliente que nunca alcança uma correção do servidor** (spawn/teleporte): hoje a Correction é
  reenviada uma vez e os inputs seguem ignorados. Decidir junto com o cliente real.
- **Política de teleporte**: `setState` limpa `correctionTimes`; decidir se mantém o histórico.
- **Comandos `admin = true` pelo chat** usam `admin.admins` (lida na subida). Falta testar com o chat do cliente real e
  decidir se admins de jogo devem ter uma lista separada da do painel. Dono: sub5.
- **Mod real e a entrega pelo launcher** (sub4 entregou só o canal): o launcher instala o mod por `protocol` a partir
  do Hub (`mod-pN-vX.Y.Z`, assinado). Falta o mod em si e um build que publique o zip como artefato `cbmp-mod` (um
  único `.zip`) para o `release.yml` (`kind=mod`, `mod_run_id`) assinar e publicar. Esse build tem de ser
  `.github/workflows/mod.yml` e o artefato usado tem de vir de um `push`/`workflow_dispatch` na `main` concluído com
  sucesso, com o commit na `main`: o `release.yml` recusa qualquer outra execução (`MOD_WORKFLOW`, `launcher/README.md`).
  O `mod.yml` faz checkout exatamente de `GITHUB_SHA` (sem `ref:`), não tem entradas de ref, branch ou fonte e não usa
  `pull_request_target`. Enquanto não houver mod com
  `protocol` compatível, o launcher fica em "Modo simular".
- **Contrato do `connect.json` para o mod** (escrito pelo launcher em `<dados>\run\connect.json`, `launcher/README.md`):
  `{host, port, ticket, serverId, contentDir, protocol, contentReadOnly: true}`, UTF-8, DACL só do usuário. O mod lê,
  **apaga** o arquivo e usa o ticket uma vez (TTL de 300 s no Hub); o launcher apaga sozinho depois de 60 s se ainda
  existir e remove sobras ao abrir. Nunca procurar o ticket em linha de comando ou variável de ambiente. `contentDir`
  é **somente leitura** (hard links do cache verificado, compartilhados entre servidores): o mod não pode gravar nada
  ali. Conferir `protocol` com o do mod e recusar se diferente.
- **Pasta de dados por build** (revisão 9a, M-8): o launcher oficial usa `%LOCALAPPDATA%\CBMultiplayer` e o build
  "Teste local" usa `%LOCALAPPDATA%\CBMultiplayer-TesteLocal` (`run\` e `mods\` inclusive); o build "Teste" (teste
  online) usa `%LOCALAPPDATA%\CBMultiplayer-Teste`. O mod não pode fixar o nome da pasta: receber o caminho do `run\`
  do launcher (ex.: arquivo ao lado do mod instalado) ou procurar nos três.
- **Margem de renovação do ticket** (revisão 7, M-1): o launcher renova o ticket com mais de 240 s (TTL 300 − 60;
  `play::TICKET_RENEW_AFTER_SECS`) antes de gravar o `connect.json`, mas o tempo entre abrir o jogo e o mod resgatar o
  ticket não é medido. Medir com o mod real e ajustar a margem (ou renovar pelo mod).
- **Mod na pasta do jogo sem administrador** (kit de teste em casa): o launcher roda sem elevação e grava o mod em
  `<jogo>\Phoenix\Binaries\Win64\cbmp`. Na Steam a biblioteca costuma permitir; na **Epic** (`C:\Program Files\Epic
  Games`) o usuário comum em geral não pode gravar e o `play` para em `permission_denied`. Decidir no subprojeto 5
  (ex.: elevação só para instalar o mod, pasta do mod fora do jogo com o loader apontando para ela). O resultado do
  passo 12 de `docs/TESTES-EM-CASA-SUB4.md` diz o que acontece numa instalação real.
- **Abrir `HogwartsLegacy.exe` direto** (sem argumentos): na Steam o jogo costuma se reabrir pela Steam; na Epic pode
  exigir os parâmetros de autenticação que o launcher da Epic passa. Conferir no subprojeto 5 (passo 12 do roteiro
  em casa).

## Servidor: resíduos do subprojeto 3 (Lua / MySQL / admin)

Decisões registradas e limites aceitos; o comportamento atual está documentado em `docs/RESOURCES.md` e
`server/README.md`. Dono sugerido: manutenção do servidor, salvo indicação.

- **Exceção dentro de `world.tick`** derruba o servidor (sem try/catch no laço). Já protegidos: codificação de
  mensagem (`NetServer::send`, `encodeErrors`), validação dos eventos de script na ponte, `applyCommands`.
- **`cb.resource` com export travado**: o prazo da vigia do chamador também corre, então chamador e alvo levam o
  aborto. Separar os prazos exigiria mudar o `Resource`.
- **Thread única de scripts**: um resource pesado atrasa os outros. Há tempo de CPU por resource no host para guiar
  uma futura divisão em várias threads.
- **`cb.events.emit` sem garantia de entrega**: `true` = enfileirado; cota por jogador de `scripting.eventOutKBps`
  (8 KiB/s) com rajada de 32 KiB e 48 B por mensagem; teto de 8192 entregas por tick. Gamemode que precise de mais
  banda tem de subir a cota e conferir o orçamento de 64 kbps da spec.
- **Canal prioritário** (`kick`/`setPosition`/kick por did): coalescido por jogador, teto de 65 536 itens / 8 MiB com
  log CRÍTICO. Uma ação do painel e um comando de script no mesmo tick: vale o último teleporte.
- **Foto dos jogadores por tick**: buffers reaproveitados (até 4), mas copia as strings de cada jogador por tick.
- **Banco (`cb.db`)**:
  - bans do MySQL só lidos na subida; bans gravados por outro servidor no mesmo banco só valem depois de reiniciar;
    gravações com o banco fora ficam em fila na memória (até 100 000) e se perdem se o processo cair;
  - subida com o banco fora: bans do banco não valem até a primeira carga (log CRÍTICO após 5 falhas). Opção futura:
    recusar entradas até carregar, atrás de uma chave de config;
  - fila do pool FIFO única, sem justiça entre resources (cada um limitado a 1024 pedidos / 16 MiB). Quando valer a
    pena, usar o desenho do `HttpOutPool` (fila por resource em rodízio + teto de conexões por resource);
  - corrotina filha dentro de `cb.db.transaction` usa outra conexão (fora da transação);
  - sem opção de CA própria para `db.tls = "verify"` (servidores com certificado interno usam `required`);
  - MySQL: `max_execution_time` só vale para `SELECT`; `CALL` com conjuntos de resultado não é suportado;
  - `mysql_close`/`mysql_thread_end` em thread desanexada depois do `exit` (só no encerramento com banco travado);
  - a integração com MariaDB/MySQL 8 roda só no CI (Linux); nunca contra um banco local.
- **HTTP de saída (`cb.http`)**:
  - `getaddrinfo` não é cancelável: uma resolução presa continua até o resolvedor do SO desistir (glibc: ~30 s) e conta
    num teto de 16 no pool e 2 por resource; acima disso o pedido falha na hora;
  - `httplib::Client::stop()` não interrompe um `connect` em andamento (≤ 3 s por endereço). IPv4 antes de IPv6, sem
    Happy Eyeballs;
  - Windows: verificação do Schannel desligada (ela buscaria AIA/CRL/OCSP na rede, fora da checagem de endereços).
    Servidores que não mandam o intermediário e raízes que o Windows só baixaria sob demanda falham; sem checagem de
    revogação (nem no Linux);
  - o httplib lê até ~800 KiB de cabeçalhos antes de o pool conferir o teto de 64 KiB (memória limitada, ~3,2 MiB no
    pior caso);
  - cada pedido cria um `SSL_CTX` e reimporta a loja do Windows (CPU por pedido);
  - `HttpOutOptions::caFile` sem chave no `server.cfg`; sem teste de sucesso contra CA do sistema (testes sem rede);
  - sem métricas do HTTP em `/metrics` (`HttpOutPool::metrics()` existe).
- **Login de admin via Hub**:
  - chave do HMAC do CSRF aleatória por processo: restart do Hub ou várias instâncias fazem uma confirmação aberta
    falhar com 403 (refaz em 1 clique). Com várias instâncias, derivar a chave de um segredo de config. Dono: Hub;
  - limites do Hub em memória do processo (não valem entre instâncias); o Hub não sonda o painel; nomes DNS da
    `adminUrl` não são resolvidos (um nome público que aponte para 127.0.0.1 passa). Dono: Hub.
- **Painel admin**:
  - sessões, states, jti e limites só em memória: restart do `cb-server` desloga todo mundo; uma instância só;
  - atrás de proxy sem `admin.trustedProxies`, os limites por IP valem para o proxy inteiro (aviso no log);
  - pool de 8 threads que segura uma por conexão: só exponha atrás de proxy que bufferiza;
  - ban pelo painel com o host de scripts parando no meio: o kick sai, o ban pode não ficar gravado (auditoria
    `failed`);
  - comando de console que demora mais de 2 s responde `pending` e a resposta tardia se perde;
  - filtro de linhas com a senha do banco por substring (senha < 4 caracteres desliga o filtro, com aviso); TOCTOU
    entre a checagem de symlink e a abertura do log (exige escrita local em `logsDir`);
  - o resumo de logins recusados da janela em curso se perde se o processo morrer sem `stop`;
  - sem teste automatizado com navegador real (fluxo verificado à mão no Chromium).
- `kid`/`name` vindos do Hub vão para o log sem remover caracteres de controle.
- **`cb.players.ban` grava `bans.json` com fsync na thread de scripts, dentro da janela do vigia** (revisão 9a, M-7):
  num disco lento a gravação durável (arquivo + pasta) pode passar do `abortMs` (5 s) e o handler do resource é
  abortado no meio (o ban fica gravado ou não, conforme o ponto do aborto). Os testes usam vigia folgada
  (`ApiEnv::roomyWatchdog`). Corrigir movendo a gravação para fora da thread de scripts (fila + confirmação), ou
  descontando o tempo de disco do vigia.

## Antes de abrir ao público

- **Ambiente de teste online** (`docs/AMBIENTE-TESTE-ONLINE.md`): Hub em modo `HUB_STAGING=1` com releases locais
  assinadas com a chave **dev**. Antes da troca para o par de produção, decidir como o launcher de teste continua
  aceitando releases de teste (par de teste próprio ou pública dev só no perfil `online-test`). O servidor de teste
  pertence a um usuário de sistema (Discord ID `0`) e não aparece no painel de ninguém. O e2e do launcher não roda
  contra esse Hub (exige dev-login); `node tools/teste-online.mjs --check` cobre o caminho sem login.

- **Launcher (sub4)**:
  - **Trocar a chave de assinatura dev** (key id `BDCB478091E91629`) pela de produção antes do primeiro release real
    (`launcher/README.md`, "Chaves"); o `release.yml` recusa assinar com a dev. Efeito (revisão 9a, M-6): o build
    "Teste local" embute a mesma `keys/update.pub`, então os releases de teste do `tools/dev-env.mjs` assinados com
    a dev deixam de valer. Rodar `docs/TESTES-MANUAIS-SUB4.md` (ou `docs/TESTES-EM-CASA-SUB4.md`, com o kit) antes da troca, ou decidir então embutir a pública dev
    só no perfil `local-test`.
  - **Endereço real do Hub**: `https://hub.cbmp.invalid` é placeholder (`settings::DEFAULT_HUB_URL` e endpoint do
    updater no `tauri.conf.json`).
  - **Assinatura Authenticode** do instalador e do executável: exige certificado de code signing. Sem ela o SmartScreen
    avisa na primeira execução ("Mais informações" → "Executar assim mesmo"); a integridade dos updates é garantida
    pelo minisign de qualquer forma.
  - **`cbmp://` registrado pelos dois builds** (revisão 9a, M-2): com o oficial e o "Teste local" instalados, o login
    cai no que foi instalado por último. Hoje basta manter só um instalado. Separar exigiria outro esquema no build de
    teste e o Hub aceitar os dois no `launcherLink` (e na CSP do formulário de dev).
  - **Linux/macOS**: o launcher é só Windows x64 (o código Rust é portável onde não custa nada, mas não é testado fora
    do Windows; localizador do jogo, ACL do `connect.json` e cofre são específicos do Windows).
  - Rollback do launcher para uma versão antiga assinada continua impossível só enquanto `requireSignedVersion` ficar
    ligado no `tauri.conf.json` (o comentário confiável traz a versão; o `config_guard` exige a opção).
  - "Limpar cache" com o jogo aberto: a UI avisa para fechar o jogo, mas o launcher não acompanha o processo do jogo
    depois de abrir; no Windows a remoção de um arquivo em uso falha (não corrompe).
  - Conteúdo e ping ignoram o proxy do sistema (`.no_proxy()`, para a regra de endereços valer): redes que só saem por
    proxy não pingam nem baixam conteúdo (o jogo também não conectaria). Documentado em `launcher/README.md`.
  - Instalação do mod com disco cheio: a troca é atômica (staging + rename), mas não há teste automatizado de disco
    cheio.
  - **Ping: leque de portas por host** (revisão 8, R1-1): cada id da lista vira um ping, então vários servidores no
    mesmo host com portas diferentes geram vários pacotes para ele (`ping.rs`, `Pinger::ping`). Limitado (≤ 200 ids,
    cache de 25 s, 16 simultâneos); agrupar por host ou limitar portas por host.
  - **NAT64 e 6to4 não são desembrulhados** em `content::ip_allowed` (revisão 8, R1-2): `64:ff9b::/96` e `2002::/16`
    podem levar a um IPv4 privado. Exige Hub comprometido e rede NAT64; tratar os dois prefixos como o IPv4 embutido.
  - **Erro de leitura do índice apaga o cache inteiro** (revisão 6, R1-3a): `Index::get` trata índice ilegível como
    vazio e o `startup` remove os arquivos que não estão nele. É re-baixável, mas é uma remoção em massa silenciosa;
    distinguir "não existe" de "não deu para ler" e não varrer no segundo caso.
  - **Montagem por cópia sem fsync** (revisão 6, R1-2): no fallback sem hard link (`content.rs`, `mount`) os arquivos
    copiados não passam por `sync_all` antes de gravar o registro da montagem; queda de energia pode deixar arquivo
    truncado marcado como montado (o arquivo do cache continua verificado).
  - **Registro do mod gravado depois da troca** (revisão 7, R1-m1): `modpkg.rs` troca a pasta do mod e só então grava
    o registro; um crash entre os dois, somado a um Hub comprometido, permite reinstalar outra versão assinada.
    Gravar o registro (versão mínima) antes da troca.
  - **Confiança da pasta da Epic só na descoberta, e só na folha** (revisão 7, R1-m3): `game.rs` (`SystemTrust`) confere
    dono/ACL do caminho quando acha o jogo, não de novo ao abrir, e não confere as pastas acima nem pontos de
    reanálise. Só atacante local; conferir a cadeia e repetir antes de abrir.
  - **Epic com manifest/pasta graváveis por outras contas** (teste online, launcher 0.1.2): numa instalação real a
    detecção da Epic falhava sempre (3 avisos "dono ou permissões não confiáveis" no log do testador). Agora o que
    falha na verificação vira **sugestão** (`game::Detection::Candidate`): nunca usada sozinha; a UI mostra "Encontramos
    o Hogwarts Legacy em … (Epic Games). Usar esta pasta?" e o Rust procura de novo, só aceita a pasta que ele mesmo
    achou, confirma num diálogo nativo e grava como o seletor (`commands::adopt_detected`). Depois disso a pasta vale
    como escolhida à mão: `launch`/`modpkg` não conferem dono/ACL (nem para pasta manual), então outra conta do PC que
    possa gravar na pasta da Epic ainda pode trocar o executável ou o mod depois da confirmação. Falta confirmar numa
    Epic real quais verificações falham (o log 0.1.2 diz: manifest, pasta do jogo, executável, Binaries). A Steam não
    tem verificação de dono/ACL nenhuma (biblioteca fora de `Program Files` costuma ser gravável por Usuários).
  - **Anti-downgrade do mod lembra só o último protocolo** (revisão 7, R1-m4): o piso de versão assinada fica no
    registro de um protocolo só (`modpkg.rs`, `installed`); alternar de protocolo zera o piso do anterior. Exige Hub
    comprometido; guardar o piso por protocolo.
  - **Validade do access token pelo `Date` do Hub sem limite** (revisão 5, N1): `hub::Tokens::access_lifetime` usa o
    `Date` da resposta sem teto; um `Date` muito errado faz o launcher renovar cedo ou tarde demais. Corrige-se sozinho
    pelo 401 + refresh; limitar ao `expires_in` anunciado.
  - Erro `protocol_mismatch` na tabela da UI e em `blocksPlay` (`ui/src/lib/errors.ts`), mas o Rust nunca o emite
    (protocolo incompatível sai como `manifest_mismatch`/`mod_unavailable`): remover ou criar a checagem no launcher.
  - **Gravações de configuração da UI sem fila** (`ui/src/lib/settings.ts`): cada uma lê, altera e grava o store;
    `pushRecent` (depois do Jogar) concorrendo com `toggleFavorite` pode perder uma das duas. Encadear numa fila.
  - O User-Agent (`hub.rs`, `content.rs`) usa `CARGO_PKG_VERSION` (a base do `Cargo.toml`, hoje `0.1.2`), enquanto o
    `release.yml` e os scripts de teste só trocam a versão do Tauri: os builds 0.1.0/0.1.1 de teste se anunciavam com a
    base da época. Usar `app.package_info().version`.
- **Hub: release mantido com ids de asset antigos** (revisão 3, Mn2): quando um release é excluído por `.sig`/`.sha256`
  ilegível, `hub/src/github.ts` mantém a entrada anterior (não regride a versão), mas os ids de asset dela podem já ter
  sido trocados no GitHub; o `/v1/dl` então falha para esse release até ele voltar a ser legível. Só disponibilidade.

- **Pacote de distribuição do `cb-server`**: levar `libmariadb.so*`/`libmariadb.dll` (LGPL, ligada dinamicamente) com
  o aviso de licença, a pasta `plugins/libmariadb/` ao lado do executável (sem ela, MySQL 8 com
  `caching_sha2_password` e contas `ed25519` não conectam) e, no Linux, depender de `ca-certificates`. Conferir o
  caminho de carga da `.so` (RPATH/`LD_LIBRARY_PATH`) no pacote.
  Levar também `THIRD-PARTY-NOTICES.md` (raiz do repositório) com as licenças de todas as dependências vcpkg ligadas
  ao `cb-server` (OpenSSL Apache-2.0 com LICENSE/NOTICE, Lua, sol2, spdlog/fmt, nlohmann-json, cpp-httplib, libsodium,
  GameNetworkingSockets/protobuf/abseil, brotli/zlib) e os arquivos `copyright` de `<vcpkg_installed>/<triplet>/share/*/`
  do build que gerou o pacote; conferir a lista contra `vcpkg list` a cada atualização da baseline.
- Limite de taxa de **novas conexões por IP** (hoje só há limite de pendentes).
- Teto de saída por cliente (`net.maxOutKbpsPerClient` é lido mas não aplicado).
- Limites explícitos do HTTP do jogo (`/info`, `/cb/*`, `/metrics`: `set_payload_max_length`, timeouts, pool). O
  painel admin e o HTTP de saída dos scripts já têm os seus.
- Teste manual de **512 bots em 4 vCPUs** (`docs/LOAD-TEST.md`).
- Hub: migrar para zod 4; revisar erros em inglês do Fastify.
- Domínio, hospedagem do Hub e repositório/releases públicos.

## Mod do jogo (spike)

- Teste do spike (F8 clone) **e reconhecimento do jogo** juntos no kit `dist/cbmp-kit-reconhecimento.zip`
  (`node tools/make-recon-kit.mjs`; roteiro `docs/RECONHECIMENTO-JOGO.md`), aguardando a visita ao PC de casa. Ele
  substitui o `dist/cb-mp-spike.zip` (mesmo UE4SS e mesmo `cb-mp.dll`). Só no jogo dá para confirmar: que o UE4SS
  carrega nesta versão do jogo; o critério de "jogador no mundo" do `cb-recon` (Character estável 45 s num mundo sem
  nome de menu); quanto tempo cada dumper leva e se algum derruba o jogo; e os nomes de propriedades lidos no
  relatório (`SkeletalMesh`, `AnimClass`, `CharacterMovement` etc. são os do UE 4.27 padrão).
- **Dados do jogo são privados**: o zip do COLETAR (`cbmp-recon-*.zip`) fica em `OneDrive\CasteloBruxoRP\cbmp-dumps\`,
  fora do repositório. Nada derivado dele (dumps, cabeçalhos, relatório, listas de nomes copiadas em massa) entra no
  repositório público; o `.gitignore` bloqueia os nomes conhecidos, mas a regra vale para qualquer formato. O que o
  subprojeto 5 precisar do SDK fica fora do repo ou é reescrito à mão só com o mínimo necessário.
