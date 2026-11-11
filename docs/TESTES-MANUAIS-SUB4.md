# Testes manuais do launcher (subprojeto 4)

Roteiro para rodar **uma vez**, nesta máquina, no fim do subprojeto 4. Tudo é local: Hub de teste em
`http://127.0.0.1:8787`, `cb-server` com o resource `hello-client` na porta 30500 e o launcher
**"CB Multiplayer (Teste local)"**. Nada vai para produção e nenhuma conta real é usada (o login padrão é uma conta
fictícia do Hub de teste).

Leva uns 30 minutos (mais uns 10 se for preciso gerar os builds de teste no passo 2).

Cada passo diz o que fazer, o que deve acontecer (**Esperado**) e o que mandar se der errado (**Se falhar**). Em
qualquer falha, mande também:

- o log do launcher: `%LOCALAPPDATA%\CBMultiplayer-TesteLocal\logs\launcher.log` (cole na barra de endereço do
  Explorador de Arquivos);
- os logs do ambiente: `%TEMP%\cbmp-dev-env\logs\hub.log` e `%TEMP%\cbmp-dev-env\logs\cb-server.log`;
- a saída do terminal onde o `dev-env` está rodando e um print da tela do launcher.

Os logs não têm tokens nem tickets (são filtrados), então podem ser mandados como estão.

> **Antes de trocar a chave de assinatura dev pela de produção**: este roteiro usa releases de teste assinados com a
> chave dev. Depois da troca (`launcher/README.md`, "Chaves") o update e o mod de teste deixam de valer.

## 0. Pré-requisitos

1. **Node 24** (`node --version` mostra `v24.x` ou maior) e as dependências instaladas. Num terminal (PowerShell ou
   Git Bash) na raiz do repositório:

   ```
   npm ci
   ```

2. **Rust** (`cargo --version`) e as ferramentas de build do Visual Studio: necessários só se o passo 2 precisar
   gerar os builds de teste.
3. **`cb-server` compilado** em `build-native\windows-release\server\Release\cb-server.exe`. Se não existir, compile
   no "Developer PowerShell for VS" (instruções completas em `server/README.md`):

   ```
   cmake --preset windows-release -DVCPKG_INSTALLED_DIR=C:/cbmp-vi
   cmake --build --preset windows-release --target cb-server
   ```

4. **Chave dev** em `%USERPROFILE%\.cbmp-devkeys\update.key` (já existe nesta máquina). Ela nunca vai para o
   repositório.
5. **Só um launcher instalado.** Se o "CB Multiplayer" oficial estiver instalado, desinstale-o antes (Configurações
   do Windows → Aplicativos → Aplicativos instalados). Os dois registram o link `cbmp://` do login, e o login cairia
   no que foi instalado por último.
6. Feche qualquer `dev-env` anterior: `node tools/dev-env.mjs --stop`.

**Esperado**: `--stop` termina sem erro (pode dizer "nada rodando").

## 1. Testes automáticos (opcional, 5 min)

```
cd launcher\src-tauri
cargo test --test e2e -- --nocapture
cd ..\..
```

**Esperado**: `test hub_e_cb_server_reais_login_dev_e_play_simulado ... ok`. Ele sobe Hub e `cb-server` de verdade
em portas livres, faz o login de dev, roda o Jogar em modo simular e confere a pasta montada.
Se aparecer `e2e PULADO: cb-server não encontrado`, volte ao item 3 dos pré-requisitos.

**Se falhar**: mande a saída inteira do comando (ela já inclui o fim dos logs do Hub e do `cb-server`).

## 2. Gerar os builds de teste

```
node tools/dev-env.mjs --build-releases
```

**Esperado**:

- Na primeira vez (ou se o código do launcher mudou), dois builds release de alguns minutos cada. Se já estiverem
  prontos e o código não mudou, ele diz `já existe e o código do launcher não mudou` e termina em segundos.
- Termina com `[dev-env] releases em ...\cbmp-dev-env\releases`, e lá existem:
  - `launcher-v0.1.0\CB Multiplayer (Teste local)_0.1.0_x64-setup.exe` (e o `.sig`);
  - `launcher-v0.1.1\CB Multiplayer (Teste local)_0.1.1_x64-setup.exe` (e o `.sig`);
  - `mod-p1-v0.1.0\cbmp-mod-p1-v0.1.0.zip` (e `.sig`, `.sha256`).
- As linhas `launcher-v0.1.x: pronto (... version:0.1.x)` mostram a versão assinada.

**Se falhar**: mande a saída do terminal (do fim para cima até o primeiro `error`).

## 3. Subir o ambiente

Num terminal que vai ficar aberto durante todo o teste:

```
node tools/dev-env.mjs --run
```

**Esperado** (em até 1 minuto):

```
 [OK  ] Hub http://127.0.0.1:8787 lista "Castelo Bruxo (teste)" em destaque e online (srv_...)
 [OK  ] update 0.1.0 → 0.1.1
 [OK  ] mod de teste: 0.1.0 (protocolo 1)
...
[dev-env] rodando. Ctrl+C encerra Hub e cb-server.
```

Os três itens com `OK`. Deixe o terminal aberto.

**Se falhar**: `FALTA` em algum item, ou erro como `a porta 8787 está em uso` (rode `node tools/dev-env.mjs --stop`
e tente de novo). Mande a saída do terminal e os dois logs de `%TEMP%\cbmp-dev-env\logs`.

## 4. Instalar o launcher 0.1.0

1. Abra `%TEMP%\cbmp-dev-env\releases\launcher-v0.1.0` no Explorador e execute
   `CB Multiplayer (Teste local)_0.1.0_x64-setup.exe`.
2. O SmartScreen avisa "O Windows protegeu o computador" (o instalador não tem assinatura Authenticode): clique em
   **Mais informações** → **Executar assim mesmo**.
3. Siga o instalador (em português, sem pedir administrador) e abra o launcher no fim.

**Esperado**:

- O instalador termina sem erro; o menu Iniciar tem **CB Multiplayer (Teste local)**.
- A janela se chama "CB Multiplayer (Teste local)" e mostra a tela de entrada com **Entrar com Discord**.
- A pasta `%LOCALAPPDATA%\CBMultiplayer-TesteLocal` foi criada, com `logs\launcher.log` contendo `versão 0.1.0`.
- Nada foi criado em `%LOCALAPPDATA%\CBMultiplayer` (essa é a pasta do launcher oficial).

**Se falhar**: print da mensagem de erro do instalador ou da janela, e o `launcher.log` se existir.

## 5. Login

1. Clique em **Entrar com Discord**. O launcher mostra "Aguardando o navegador…" e o navegador padrão abre uma página
   do Hub de teste: **"Hub de teste: entrar sem Discord"**, com Discord ID `111111111111111111` e Nome `Dev Local`.
2. Clique em **Entrar como dev**.
3. O navegador pergunta se pode abrir o aplicativo do link `cbmp://` (CB Multiplayer (Teste local)): **permita**
   (pode marcar "sempre permitir" para este site).

**Esperado**:

- O launcher sai da tela de entrada e mostra a lista de servidores; o perfil (canto da janela) mostra **Dev Local** e
  o botão **Sair**.
- `launcher.log` tem `login iniciado no navegador` e `login concluído`, e nenhum código, token ou ticket (procure por
  `at_`, `rt_`, `code=`: se aparecerem, o valor está trocado por `***`).
- Fechar e abrir o launcher de novo: continua logado (a sessão fica no Gerenciador de Credenciais do Windows, entrada
  com `CBMultiplayer (Teste local)`).

**Se falhar**: o que a página do navegador mostrou (print), se o navegador perguntou sobre o link `cbmp://`, e o
`launcher.log`. Se o login foi parar em outro programa, confira o item 5 dos pré-requisitos.

### 5b. (Opcional) Login com o app Discord de teste

Só se você quiser testar o Discord de verdade, com um **app de teste** do Discord Developer Portal (nunca o de
produção):

1. No app de teste, em OAuth2 → Redirects, cadastre `http://127.0.0.1:8787/v1/auth/discord/callback`.
2. Pare o ambiente (Ctrl+C no terminal do passo 3). No **mesmo** terminal, defina as variáveis só para ele (nunca em
   arquivo, nunca no repositório) e suba de novo:

   PowerShell:
   ```
   $env:DISCORD_CLIENT_ID = "<client id do app de teste>"
   $env:DISCORD_CLIENT_SECRET = "<client secret do app de teste>"
   node tools/dev-env.mjs --run
   ```

3. No launcher, **Sair** e **Entrar com Discord** de novo.

**Esperado**: o navegador vai para o Discord (autorizar o app de teste) e volta ao launcher logado com o seu nome do
Discord e avatar.

Depois, feche esse terminal (as variáveis somem com ele), abra outro e suba o ambiente de novo com
`node tools/dev-env.mjs --run` para continuar com a conta de dev.

## 6. Lista: destaque, filtros, favoritos, ping

1. Em **Servidores**, no topo, o cartão **★ Destaque** mostra **Castelo Bruxo (teste)** com a descrição
   "Servidor local de teste (tools/dev-env.mjs): hello-client.", jogadores `0/32` e um botão **Jogar**.
2. A lista abaixo tem o mesmo servidor, com a tag `teste` e o idioma `pt-BR`.
3. **Ping**: em poucos segundos a coluna Ping mostra um número em `ms` (normalmente `0 ms` a `5 ms`) no lugar de `…`.
4. **Filtros**:
   - Digite `xyz` em **Buscar servidor**: a lista fica vazia. Apague: o servidor volta.
   - Digite `castelo`: o servidor aparece.
   - Clique no chip `teste`: continua aparecendo; clique de novo para desligar.
   - Em **Todos os idiomas**, escolha `pt-BR`: aparece. Volte para "Todos os idiomas".
   - Marque **Com vagas**: aparece (0/32). Desmarque.
5. **Favoritos**: clique em **☆** ao lado do servidor: vira **★**. Em **Favoritos** (menu lateral) o servidor aparece.
   Clique em **★** de novo: some de Favoritos. Favorite de novo para o passo 9.
6. **Atualizar lista**: clique; a lista recarrega sem erro.

**Esperado**: tudo como descrito; nenhum aviso de erro.

**Se falhar**: print da tela e o `launcher.log`. Para o ping, diga o que a coluna mostra (`…` ou `sem resposta`).

## 7. Jogar (modo simular) até "Pronto para abrir"

1. Em **Configurações**, confira que **Modo simular** está marcado (padrão). Em "Pasta do jogo" pode estar "Não
   encontrada": tudo bem, no modo simular o jogo não é necessário.
2. Volte a **Servidores** e clique em **Jogar** no cartão de destaque.

**Esperado**:

- O botão vira "Entrando…" e a barra de download aparece e some rápido (o conteúdo tem poucos KB).
- Aparece o painel **"Pronto para abrir — Castelo Bruxo (teste)"** com: Endereço `127.0.0.1`, Porta `30500`,
  Servidor `srv_...` (o mesmo id da saída do passo 3), Protocolo `1` e a Pasta do conteúdo
  `C:\Users\<você>\AppData\Local\CBMultiplayer-TesteLocal\servers\srv_...`.
- Em **Recentes**, o servidor aparece.

**Se falhar**: print do painel de erro (título e detalhe) e o `launcher.log` (procure `jogar em ... falhou: <código>`).

## 8. Conferir a pasta montada, o arquivo de conexão e o log

1. Abra no Explorador a **Pasta do conteúdo** mostrada no passo 7.
   **Esperado**: uma pasta `hello-client` com exatamente estes arquivos, iguais aos de
   `resources.example\hello-client` do repositório:
   - `client\main.lua`
   - `files\logo.png`
   - `ui\index.html`, `ui\app.js`, `ui\style.css`

   (Sem `resource.json` e sem `server\`: só o conteúdo de cliente é baixado.) Os arquivos de
   `%LOCALAPPDATA%\CBMultiplayer-TesteLocal\cache` são as cópias verificadas (nomes = sha256).
2. Abra `%LOCALAPPDATA%\CBMultiplayer-TesteLocal\run` **em até 60 segundos** depois do Jogar.
   **Esperado**: existe `connect.json` (não o abra num editor que trave o arquivo; o Bloco de Notas serve) com
   `host`, `port`, `ticket`, `serverId`, `contentDir`, `protocol` e `"contentReadOnly": true`. Depois de 60 s o
   arquivo some sozinho (o launcher apaga o ticket que o mod não leu).
3. Abra `logs\launcher.log`.
   **Esperado**: linhas com data/hora (UTC) e nível (`INFO`/`WARN`), sem nenhum token, ticket ou código (procure
   por `at_`, `rt_`, `ticket`, `code=`; se aparecerem, o valor está trocado por `***`).

**Se falhar**: diga qual arquivo falta/sobra (ou print da pasta) e mande o `launcher.log`.

## 9. Atualização automática 0.1.0 → 0.1.1

1. Feche e abra o launcher (a procura de atualização roda ao abrir).
   **Esperado**: no menu lateral aparece **Atualização 0.1.1 disponível** (e `launcher.log` tem
   `atualização disponível: Some("0.1.1")`).
2. Clique nele (ou em **Configurações** → **Procurar atualização**). Aparece "Versão 0.1.1 disponível." com a nota
   "Versão de teste 0.1.1 do launcher (Teste local), gerada por tools/dev-env.mjs."
3. Clique em **Instalar atualização**.
   **Esperado**: "Baixando e conferindo a atualização… o launcher será reiniciado."; o instalador roda sozinho (janela
   de progresso, sem perguntas) e o launcher reabre.
4. Em **Configurações** → **Versão**: **v0.1.1** (também no rodapé do menu). **Procurar atualização** →
   "Você já está na versão mais recente."
5. Continua logado como Dev Local, e os favoritos/recentes do passo 6/7 continuam lá.

**Se falhar**: print da mensagem e o `launcher.log` (procure `atualização`, `assinatura`). O `hub.log` mostra se o
download `/v1/dl/launcher-v0.1.1/...` chegou ao Hub.

## 10. (Opcional, só com o Hogwarts Legacy instalado) Mod de teste

Só se o jogo estiver instalado. Este passo **abre o jogo** (o mod de teste não faz nada; só um LEIA-ME):

1. **Configurações**: confira a **Pasta do jogo** (achada na Steam/Epic, ou **Escolher pasta do jogo**) e
   **desmarque** Modo simular.
2. **Jogar** no servidor de teste.
   **Esperado**: a etapa do mod baixa `cbmp-mod-p1-v0.1.0.zip` do Hub de teste, confere a assinatura e instala em
   `<pasta do jogo>\Phoenix\Binaries\Win64\cbmp\` (só `LEIA-ME.txt`); `launcher.log` tem
   `mod 0.1.0 (protocolo 1) instalado`; o jogo abre (sem multiplayer: o mod real é do subprojeto 5). Feche o jogo.
3. **Marque de novo** Modo simular e apague a pasta `<pasta do jogo>\Phoenix\Binaries\Win64\cbmp`.

**Se falhar**: o erro mostrado e o `launcher.log`.

## 11. Limpar cache

1. **Configurações** → **Cache** → **Limpar cache**. Aparece o aviso para fechar o jogo; clique em **Limpar agora**.
   **Esperado**: "Cache limpo."; as pastas `%LOCALAPPDATA%\CBMultiplayer-TesteLocal\cache` e `servers` somem (ou
   ficam vazias).
2. **Jogar** de novo (modo simular).
   **Esperado**: baixa tudo de novo e chega a "Pronto para abrir" como no passo 7.

**Se falhar**: print e `launcher.log`.

## 12. Sair, desinstalar e encerrar

1. No launcher, **Sair**. **Esperado**: volta à tela de entrada; no Gerenciador de Credenciais do Windows
   (Credenciais do Windows → Credenciais genéricas) não há mais entrada com `CBMultiplayer (Teste local)`.
2. Feche o launcher e desinstale: Configurações do Windows → Aplicativos → Aplicativos instalados →
   **CB Multiplayer (Teste local)** → Desinstalar.
   **Esperado**: some do menu Iniciar e da lista de aplicativos.
3. Apague `%LOCALAPPDATA%\CBMultiplayer-TesteLocal` (o desinstalador não apaga a pasta de dados nem os logs).
4. No terminal do `dev-env`, **Ctrl+C**. **Esperado**: `[dev-env] encerrando…` e o terminal volta ao prompt.
   Depois rode `node tools/dev-env.mjs --stop` (**Esperado**: `nada rodando (sem pids.json)`).
5. (Opcional) Apague `%TEMP%\cbmp-dev-env` para descartar dados do Hub de teste e os builds de teste (o próximo
   `--build-releases` refaz os builds).

**Se falhar**: se o Ctrl+C não encerrar, rode `node tools/dev-env.mjs --stop` e mande a saída.

## Resultado

Mande uma linha por passo: `OK`, `FALHOU` (com o que foi pedido em "Se falhar") ou `PULEI`. Os passos 1, 5b e 10 são
opcionais.
