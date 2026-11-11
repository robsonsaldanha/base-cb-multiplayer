# Testes do launcher em casa (subprojeto 4)

Roteiro para testar o launcher **"CB Multiplayer (Teste local)"** num PC com Windows 11 que **não** tem nada do
projeto (sem repositório, Rust, Visual Studio ou chave de assinatura), mas **tem o Hogwarts Legacy instalado** (Steam
ou Epic). Tudo o que precisa de compilador já vem pronto no **kit** (`cbmp-kit-teste-sub4.zip`): o Hub de teste, o
`cb-server` com o resource `hello-client`, os instaladores 0.1.0 e 0.1.1 e o mod de teste, todos assinados com a
chave de desenvolvimento.

> Este arquivo é o `LEIA-ME.md` que vem dentro do kit. Na máquina de desenvolvimento o roteiro é outro:
> `docs/TESTES-MANUAIS-SUB4.md`. O kit é montado lá com `node tools/make-test-kit.mjs`.

Tudo roda **só neste PC**: o Hub de teste fica em `http://127.0.0.1:8787` (não aceita conexões de fora), o
`cb-server` na porta 30500, e o login padrão é uma conta fictícia ("Dev Local"). Nada vai para produção, nenhuma
conta real é usada e nada é publicado.

Tempo: uns 15 minutos para preparar e uns 45 para os testes (o passo 12, com o jogo, é o mais demorado).

**Como ler cada passo**: o que fazer, o que deve acontecer (**Esperado**) e o que mandar se der errado (**Se
falhar**). No fim há uma **tabela de resultados** para preencher e mandar de volta.

**O que mandar em qualquer falha** (além do que o passo pedir):

- o log do launcher: `%LOCALAPPDATA%\CBMultiplayer-TesteLocal\logs\launcher.log` (cole esse caminho na barra de
  endereço do Explorador de Arquivos e aperte Enter);
- os logs do ambiente: `C:\cbmp-kit\cbmp-kit-teste-sub4\dados\logs\hub.log` e `cb-server.log`;
- um print da janela preta do ambiente e da tela do launcher (Win+Shift+S tira print de um pedaço da tela).

Os logs não têm tokens, tickets nem senhas (o launcher e o Hub filtram), então podem ser mandados como estão.

---

## 1. O que instalar (uma vez)

### 1.1 Node.js 24 (obrigatório)

O Hub de teste roda no Node.js. **Use a versão 24** (o kit foi montado e testado com ela).

1. Abra <https://nodejs.org/dist/latest-v24.x/> e baixe o arquivo **`node-v24.<número>-x64.msi`** (o que termina em
   `-x64.msi`).
   - Não use o botão grande da página inicial do nodejs.org se ele oferecer outra versão (26 ou mais nova).
2. Execute o `.msi` e aceite os padrões (Next, Next…, Install). **Não** precisa marcar "Tools for Native Modules"
   (a caixa que instala Chocolatey/Visual Studio): o kit já traz tudo compilado.
3. Feche e abra de novo qualquer janela de terminal. Abra o **PowerShell** (menu Iniciar → digite `PowerShell`) e
   rode:

   ```
   node --version
   npm --version
   ```

**Esperado**: `v24.` seguido de números (ex.: `v24.18.0`) e um número de versão do npm (ex.: `11.x`).

**Se falhar**: `node` não reconhecido → reinicie o PC e tente de novo. Outra versão (ex.: `v22` ou `v26`) → o kit
avisa ao subir; com `v22` ou mais nova costuma funcionar, mas para o teste valer instale a 24.

### 1.2 O que NÃO precisa instalar

- **Git, Rust, Visual Studio, vcpkg, chave de assinatura**: não. O kit nunca compila nada e nunca procura a chave.
- **Visual C++ Redistributable**: não. As DLLs do runtime do Visual C++ que o `cb-server` usa vão junto dele no kit
  (`bin\cb-server\`). Só se a janela do ambiente disser "provável falta do Microsoft Visual C++ Redistributable",
  instale o x64 de <https://aka.ms/vs/17/release/vc_redist.x64.exe> (é da Microsoft) e tente de novo.
- **WebView2** (o "navegador" que desenha a janela do launcher): já vem no Windows 11. Para conferir: Configurações do
  Windows → Aplicativos → Aplicativos instalados → procure **Microsoft Edge WebView2 Runtime**. Se não estiver lá, o
  instalador do launcher baixa e instala sozinho (precisa de internet nessa hora).

### 1.3 Antes de começar

- Se o **"CB Multiplayer" oficial** estiver instalado (não deve estar), desinstale-o: os dois registram o link
  `cbmp://` do login e o login cairia no instalado por último.
- Tenha uns **300 MB livres** no disco C:.
- Saiba se o seu Hogwarts Legacy é da **Steam** ou da **Epic** (vai ser anotado no resultado).

---

## 2. Pegar e extrair o kit

O kit é um arquivo só: **`cbmp-kit-teste-sub4.zip`** (uns 30 MB), com um `cbmp-kit-teste-sub4.zip.sha256` ao lado.
Na pasta do OneDrive ele fica em `OneDrive\CasteloBruxoRP\Launcher_livre\dist\` (ou chega num pendrive).

1. **OneDrive com "arquivos sob demanda"**: se o ícone do zip no Explorador for uma **nuvem**, ele ainda não está no
   PC. Clique com o botão direito no zip → **Sempre manter neste dispositivo** e espere o ícone virar um **check
   verde**. Extrair um arquivo que ainda está baixando dá erro ou um kit pela metade.
2. **Copie** o zip para fora do OneDrive, por exemplo para `C:\cbmp-kit\` (crie a pasta). Assim o OneDrive não fica
   sincronizando os milhares de arquivos extraídos nem os logs.
3. (Recomendado) Confira se o zip chegou inteiro. No PowerShell:

   ```
   Get-FileHash C:\cbmp-kit\cbmp-kit-teste-sub4.zip
   ```

   **Esperado**: o `Hash` mostrado é igual (maiúsculas/minúsculas não importam) ao texto do arquivo
   `cbmp-kit-teste-sub4.zip.sha256` (abra com o Bloco de Notas).
4. Se o zip veio por **download do navegador** (não pelo OneDrive sincronizado nem pendrive): botão direito no zip →
   **Propriedades** → marque **Desbloquear** → OK. Sem isso o Windows marca todos os arquivos extraídos como "vindos
   da internet" e pede confirmação a cada um.
5. Botão direito no zip → **Extrair Tudo…**. Na janela que abre, o campo "Os arquivos serão extraídos para esta
   pasta" já vem preenchido pelo Windows com **`C:\cbmp-kit\cbmp-kit-teste-sub4`**. **Apague o final
   `\cbmp-kit-teste-sub4`** para o campo ficar **exatamente `C:\cbmp-kit`** (o zip já traz a pasta
   `cbmp-kit-teste-sub4` dentro dele; se você deixar como o Windows sugere, ela fica repetida). Clique
   **Extrair**.

**Esperado**: existe a pasta **`C:\cbmp-kit\cbmp-kit-teste-sub4`** e, **direto dentro dela**, `LEIA-ME.md` (este
roteiro), `INICIAR.cmd`, `PARAR.cmd`, `CONFERIR.cmd`, `kit.json` e as pastas `bin`, `hub`, `node_modules`,
`releases`, `tools` etc. Para conferir no PowerShell:

```
Test-Path C:\cbmp-kit\cbmp-kit-teste-sub4\INICIAR.cmd
```

**Esperado**: `True`. Todos os caminhos deste roteiro partem de `C:\cbmp-kit\cbmp-kit-teste-sub4`.

Use um caminho **curto, sem espaços e sem acentos** (como `C:\cbmp-kit`). Evite a Área de Trabalho, Downloads
(caminhos longos) e pastas dentro do OneDrive.

**Se falhar**:

- O comando mostrou `False`, e existe `C:\cbmp-kit\cbmp-kit-teste-sub4\cbmp-kit-teste-sub4` (a pasta repetida: o
  final do caminho não foi apagado no item 5) → apague a pasta `C:\cbmp-kit\cbmp-kit-teste-sub4` inteira e refaça o
  item 5, apagando o final `\cbmp-kit-teste-sub4` do campo. (O kit até funcionaria na pasta repetida, mas os caminhos
  do roteiro ficariam todos errados.)
- Erro de "caminho muito longo" ou "arquivo não encontrado" ao extrair → confira o item 1 (arquivo inteiro no PC) e
  extraia em `C:\cbmp-kit`. Mande o print do erro.

---

## 3. Configurar

Não há nada a configurar para o teste padrão: o launcher de teste procura o Hub em `127.0.0.1:8787`, o ambiente
cadastra sozinho o servidor "Castelo Bruxo (teste)" e o login é a conta fictícia de dev.

Opcional, só para o passo 6b (login com o Discord de verdade): um **app de teste** no Discord Developer Portal (nunca
o de produção). Os dados dele são digitados só na janela do terminal, nunca gravados em arquivo (veja o passo 6b).

---

## 4. Subir o ambiente

1. Em `C:\cbmp-kit\cbmp-kit-teste-sub4`, dê dois cliques em **`INICIAR.cmd`**. Abre uma janela preta: **deixe-a
   aberta durante todo o teste** (fechá-la desliga o Hub e o servidor).
2. Se aparecer **"O Firewall do Windows Defender bloqueou alguns recursos deste aplicativo"** para `cb-server.exe`
   (o Hub, `node.exe`, só escuta dentro do PC e não deve gerar esse aviso): clique **Cancelar** (recomendado). O teste todo acontece dentro do próprio PC, que o firewall
   não bloqueia; "Cancelar" só impede que outros PCs da rede alcancem a porta 30500 do `cb-server`.

**Esperado** (em até 1 minuto), na janela preta:

```
[dev-env] kit conferido: 3000+ arquivos ok (commit ..., gerado em ...)
[dev-env] subindo o Hub em http://127.0.0.1:8787 (log: C:\cbmp-kit\cbmp-kit-teste-sub4\dados\logs\hub.log)
[dev-env] servidor cadastrado: srv_...
[dev-env] subindo o cb-server na porta 30500 (log: ...\dados\logs\cb-server.log)
...
 [OK  ] Hub http://127.0.0.1:8787 lista "Castelo Bruxo (teste)" em destaque e online (srv_...)
 [OK  ] update 0.1.0 → 0.1.1
 [OK  ] mod de teste: 0.1.0 (protocolo 1)
...
[dev-env] rodando. Ctrl+C encerra Hub e cb-server.
```

Os três itens com `OK`. Anote o `srv_...` (vai aparecer de novo no passo 9).

3. **Conferência automática**: com a janela do ambiente aberta, dê dois cliques em **`CONFERIR.cmd`**. Ele faz pelo
   HTTP o que o launcher faria (lista, update, mod, conteúdo do servidor) e confere as assinaturas.

**Esperado**: seis linhas `[OK    ]` e `Tudo certo: 6 conferências OK.` Feche essa segunda janela (a do ambiente
continua aberta).

**Se falhar**:

- `o kit está incompleto ou corrompido` → o zip não foi extraído inteiro: apague `C:\cbmp-kit\cbmp-kit-teste-sub4`
  e refaça o passo 2.
- `a porta 8787 está em uso` → já há um ambiente aberto: feche a outra janela ou dê dois cliques em `PARAR.cmd`, e
  tente de novo.
- `o cb-server parou (código ..., DLL não encontrada)` → instale o Visual C++ Redistributable (passo 1.2).
- `FALTA` em algum item, ou `[FALHOU]` no `CONFERIR.cmd` → mande o print das duas janelas e os dois logs de
  `dados\logs`.

---

## 5. Instalar o launcher 0.1.0

1. Abra `C:\cbmp-kit\cbmp-kit-teste-sub4\releases\launcher-v0.1.0` e dê dois cliques em
   **`CB Multiplayer (Teste local)_0.1.0_x64-setup.exe`**.
2. **SmartScreen**: o instalador **não tem assinatura digital Authenticode** (isso exige um certificado pago, que
   ainda não existe), então o Windows pode mostrar **"O Windows protegeu o computador"**. Clique em **Mais
   informações** → **Executar assim mesmo**. Isso é esperado neste teste. (A integridade das **atualizações** não
   depende disso: elas têm assinatura própria, conferida pelo launcher.)
3. Siga o instalador (em português, **sem pedir administrador**) e deixe marcado para abrir o launcher no fim.

**Esperado**:

- O instalador termina sem erro; o menu Iniciar tem **CB Multiplayer (Teste local)**.
- A janela se chama "CB Multiplayer (Teste local)" e mostra a tela de entrada com **Entrar com Discord**.
- Existe a pasta `%LOCALAPPDATA%\CBMultiplayer-TesteLocal`, com `logs\launcher.log` contendo `versão 0.1.0`.
- **Não** existe `%LOCALAPPDATA%\CBMultiplayer` (essa é a pasta do launcher oficial).

**Se falhar**: print da mensagem do instalador ou da janela, e o `launcher.log` se existir. Se a janela abrir em
branco, diga se o WebView2 aparece nos aplicativos instalados (passo 1.2).

---

## 6. Login

1. Clique em **Entrar com Discord**. O launcher mostra "Aguardando o navegador…" e o navegador padrão abre a página
   **"Hub de teste: entrar sem Discord"**, com Discord ID `111111111111111111` e Nome `Dev Local`.
2. Clique em **Entrar como dev**.
3. O navegador pergunta se pode abrir o link `cbmp://` (CB Multiplayer (Teste local)): **permita** (pode marcar
   "sempre permitir" para este site).

**Esperado**:

- O launcher mostra a lista de servidores; o perfil mostra **Dev Local** e o botão **Sair**.
- `launcher.log` tem `login iniciado no navegador` e `login concluído`, e nenhum código, token ou ticket (procure
  `at_`, `rt_`, `code=`: se aparecerem, o valor está trocado por `***`).
- Feche o launcher e abra de novo: continua logado.

**Se falhar**: print do que o navegador mostrou, se ele perguntou sobre o link `cbmp://`, e o `launcher.log`.

### 6b. (Opcional) Login com um app Discord de teste

Só se você quiser testar o Discord de verdade, com um **app de teste** (nunca o de produção):

1. No app de teste (Discord Developer Portal → OAuth2 → Redirects), cadastre
   `http://127.0.0.1:8787/v1/auth/discord/callback`.
2. Na janela do ambiente aperte **Ctrl+C** e responda `S` em "Deseja finalizar o arquivo em lotes". Abra o
   **PowerShell** e cole estas linhas, uma por vez. As duas do meio **perguntam** o Client ID e o Client Secret do
   app de teste: digite (ou cole) cada um quando pedir e aperte Enter. Assim os valores ficam só nesta janela: o
   secret nem aparece na tela, e nenhum dos dois vai para o histórico do PowerShell (que é gravado em arquivo; por
   isso **não** escreva o secret direto num comando).

   ```
   cd C:\cbmp-kit\cbmp-kit-teste-sub4
   $env:DISCORD_CLIENT_ID = Read-Host 'Client ID do app de teste'
   $env:DISCORD_CLIENT_SECRET = [Net.NetworkCredential]::new('', (Read-Host -AsSecureString 'Client Secret do app de teste')).Password
   node tools\dev-env.mjs --run
   ```

3. No launcher, **Sair** e **Entrar com Discord** de novo.

**Esperado**: o navegador vai ao Discord (autorizar o app de teste) e volta ao launcher logado com o seu nome e
avatar do Discord.

Depois: Ctrl+C nesse PowerShell, **feche-o** (as variáveis somem com ele) e suba o ambiente de novo pelo
`INICIAR.cmd`; no launcher, **Sair** e entre de novo como dev para continuar.

---

## 7. Lista: destaque, filtros, favoritos, ping

1. Em **Servidores**, no topo, o cartão **★ Destaque** mostra **Castelo Bruxo (teste)** com a descrição
   "Servidor local de teste (tools/dev-env.mjs): hello-client.", jogadores `0/32` e o botão **Jogar**.
2. A lista abaixo tem o mesmo servidor, com a tag `teste` e o idioma `pt-BR`.
3. **Ping**: em poucos segundos a coluna Ping mostra um número em `ms` (normalmente `0 ms` a `5 ms`).
4. **Filtros**:
   - `xyz` em **Buscar servidor**: lista vazia. Apague: o servidor volta.
   - `castelo`: aparece.
   - Chip `teste`: continua aparecendo; clique de novo para desligar.
   - **Todos os idiomas** → `pt-BR`: aparece. Volte para "Todos os idiomas".
   - **Com vagas**: aparece. Desmarque.
5. **Favoritos**: **☆** ao lado do servidor vira **★**; em **Favoritos** (menu lateral) ele aparece. **★** de novo:
   some. Favorite de novo (usado no passo 11).
6. **Atualizar lista**: recarrega sem erro.

**Esperado**: tudo como descrito, sem aviso de erro.

**Se falhar**: print e `launcher.log`. Para o ping, diga o que a coluna mostra (`…` ou `sem resposta`).

---

## 8. Pasta do jogo (detecção automática, sem mexer no jogo)

Ao abrir, o launcher procura o Hogwarts Legacy **só lendo** o registro do Windows e os arquivos de configuração da
Steam (`libraryfolders.vdf`, `appmanifest_990080.acf`) e da Epic (`C:\ProgramData\Epic\EpicGamesLauncher\Data\Manifests`).
Uma pasta só é aceita se tiver `HogwartsLegacy.exe` e a subpasta `Phoenix\Binaries\Win64`. Este passo **não grava
nada na pasta do jogo**.

1. No launcher, abra **Configurações**. Se a "Pasta do jogo" mostrar "Não encontrada", vá a **Servidores** e volte
   a **Configurações** (a busca roda logo depois de abrir o launcher).
2. Anote o caminho mostrado em **Pasta do jogo**. Exemplos comuns:
   - Steam: `C:\Program Files (x86)\Steam\steamapps\common\Hogwarts Legacy` (ou outra biblioteca da Steam);
   - Epic: `C:\Program Files\Epic Games\HogwartsLegacy`.
3. Teste o seletor: **Escolher pasta do jogo** → escolha uma pasta **qualquer que não seja do jogo** (ex.:
   `C:\cbmp-kit`).
   **Esperado**: aviso "A pasta escolhida não é do Hogwarts Legacy." e o caminho anterior continua.
4. **Escolher pasta do jogo** → escolha a pasta do jogo do item 2 (a que tem `HogwartsLegacy.exe`).
   **Esperado**: o caminho aparece de novo, sem aviso.

**Esperado no geral**: a pasta é achada sozinha (Steam ou Epic) e é a mesma que a Steam/Epic mostra (Steam: botão
direito no jogo → Gerenciar → Procurar arquivos locais; Epic: Biblioteca → … → Gerenciar → ícone de pasta).

**Se falhar**: diga se é Steam ou Epic, qual é a pasta real do jogo, o que a tela mostra e mande o `launcher.log`
(procure `Hogwarts Legacy não encontrado automaticamente` ou `pasta do jogo`). Se não achou sozinho mas o seletor
funcionou, marque o passo como **FALHOU (detecção)** e continue: o resto usa a pasta escolhida.

---

## 9. Jogar (modo simular) até "Pronto para abrir"

1. Em **Configurações**, confira que **Modo simular** está **marcado** (padrão). Com ele marcado o launcher **não
   abre o jogo e não grava nada na pasta do jogo**.
2. Em **Servidores**, clique em **Jogar** no cartão de destaque.

**Esperado**:

- O botão vira "Entrando…" e a barra de download aparece e some rápido (são poucos KB).
- Aparece o painel **"Pronto para abrir — Castelo Bruxo (teste)"**, com "Modo simular: o conteúdo foi baixado e
  conferido, mas o jogo não foi aberto", Endereço `127.0.0.1`, Porta `30500`, Servidor `srv_...` (o mesmo do passo
  4), Protocolo `1` e a Pasta do conteúdo
  `C:\Users\<você>\AppData\Local\CBMultiplayer-TesteLocal\servers\srv_...`.
- Em **Recentes**, o servidor aparece.

**Se falhar**: print do painel de erro e o `launcher.log` (procure `jogar em ... falhou: <código>`).

---

## 10. Conferir a pasta montada, o arquivo de conexão e o log

1. Abra no Explorador a **Pasta do conteúdo** do passo 9.
   **Esperado**: uma pasta `hello-client` com exatamente: `client\main.lua`, `files\logo.png`, `ui\index.html`,
   `ui\app.js`, `ui\style.css` (iguais aos de `C:\cbmp-kit\cbmp-kit-teste-sub4\resources.example\hello-client`,
   **sem** `resource.json` e sem `server\`).
2. Abra `%LOCALAPPDATA%\CBMultiplayer-TesteLocal\run` **em até 60 segundos** depois do Jogar (se passou, clique
   Jogar de novo).
   **Esperado**: existe `connect.json` (abra com o Bloco de Notas) com `host`, `port`, `ticket`, `serverId`,
   `contentDir`, `protocol` e `"contentReadOnly": true`. Depois de 60 s o arquivo some sozinho.
3. Abra `logs\launcher.log`.
   **Esperado**: linhas com data/hora (UTC) e nível (`INFO`/`WARN`), sem token, ticket ou código (procure `at_`,
   `rt_`, `ticket`, `code=`: se aparecerem, o valor está trocado por `***`).

**Se falhar**: qual arquivo falta/sobra (ou print da pasta) e o `launcher.log`.

---

## 11. Atualização automática 0.1.0 → 0.1.1

1. Feche e abra o launcher.
   **Esperado**: no menu lateral aparece **Atualização 0.1.1 disponível** (`launcher.log`:
   `atualização disponível: Some("0.1.1")`).
2. Clique nele (ou **Configurações** → **Procurar atualização**): "Versão 0.1.1 disponível." com a nota "Versão de
   teste 0.1.1 do launcher (Teste local), gerada por tools/dev-env.mjs."
3. **Instalar atualização**.
   **Esperado**: "Baixando e conferindo a atualização… o launcher será reiniciado."; o instalador roda sozinho
   (janela de progresso, sem perguntas) e o launcher reabre. O SmartScreen **não** deve aparecer aqui.
4. **Configurações** → **Versão**: **v0.1.1**. **Procurar atualização** → "Você já está na versão mais recente."
5. Continua logado como Dev Local; favoritos e recentes continuam lá; a Pasta do jogo continua a mesma.

**Se falhar**: print e `launcher.log` (procure `atualização`, `assinatura`). O `hub.log` mostra se o download
`/v1/dl/launcher-v0.1.1/...` chegou ao Hub.

---

## 12. Mod de teste e abertura do jogo (modo simular DESLIGADO)

Este é o único passo que **grava na pasta do jogo e abre o Hogwarts Legacy**. Leia tudo antes.

### O que o launcher faz com o modo simular desligado (exatamente)

Ao clicar **Jogar**, na ordem:

1. Baixa e confere o conteúdo do servidor, como no passo 9 (só em `%LOCALAPPDATA%\CBMultiplayer-TesteLocal`).
2. **Mod**: pergunta ao Hub de teste qual é o mod do protocolo 1, baixa `cbmp-mod-p1-v0.1.0.zip` para
   `%LOCALAPPDATA%\CBMultiplayer-TesteLocal\mods\download`, confere tamanho, sha256 e a **assinatura** (mesma chave das
   atualizações) e só então instala na pasta do jogo:
   - extrai em `<pasta do jogo>\Phoenix\Binaries\Win64\cbmp.staging`, relê e confere cada arquivo;
   - renomeia `cbmp.staging` para **`<pasta do jogo>\Phoenix\Binaries\Win64\cbmp`**. Se já existisse uma pasta
     `cbmp` ali, ela seria renomeada para `cbmp.old` e **apagada** (por isso o item "Antes" abaixo);
   - o mod de teste é **um único arquivo de texto**, `cbmp\LEIA-ME.txt`, sem nenhum código: o jogo não carrega nem
     percebe essa pasta.
   - Guarda uma cópia do pacote e o registro em `%LOCALAPPDATA%\CBMultiplayer-TesteLocal\mods\` (`installed.json`,
     `package\`).
3. Grava `run\connect.json` (em `%LOCALAPPDATA%`, apagado em 60 s).
4. **Abre `<pasta do jogo>\HogwartsLegacy.exe` diretamente**, sem argumentos (nada de ticket na linha de comando),
   como se você desse dois cliques nele. O launcher não espera o jogo nem fica de olho nele.

**Fora a pasta `cbmp`, nada na pasta do jogo é criado, alterado ou apagado**: nenhum arquivo do jogo é modificado.
Tudo é **reversível** apagando a pasta `cbmp` (veja "Desfazer" abaixo). O desinstalador do launcher **não** apaga essa
pasta, e "Verificar integridade dos arquivos" da Steam/Epic também **não** (ela só conserta arquivos do próprio jogo).

O que **não** dá para testar ainda: o **multiplayer de verdade**. O mod real (que lê o `connect.json`, conecta no
servidor e carrega o `hello-client` dentro do jogo) é do **subprojeto 5** e ainda não existe. O jogo vai abrir
normal, single-player, sem nenhuma mudança.

### Antes

1. Feche o Hogwarts Legacy se estiver aberto. Steam ou Epic podem ficar abertas (ajuda: o jogo pode precisar delas).
2. Confira que **não existe** uma pasta `cbmp` (nem `cbmp.old`/`cbmp.staging`) em
   `<pasta do jogo>\Phoenix\Binaries\Win64`. Abra o **PowerShell** e cole as duas linhas abaixo, trocando **só** o
   que está entre aspas na primeira pela **Pasta do jogo** anotada no passo 8 (a pasta que tem
   `HogwartsLegacy.exe`; o resto do caminho a segunda linha completa sozinha). Deixe essa janela aberta: ela é usada de
   novo em "Desfazer".

   ```
   $jogo = "C:\Program Files (x86)\Steam\steamapps\common\Hogwarts Legacy"
   Get-ChildItem -LiteralPath (Join-Path $jogo 'Phoenix\Binaries\Win64') -Filter cbmp*
   ```

   **Esperado**: não mostra nada. Se mostrar algo, **pare** este passo, marque **PULEI** e conte o que apareceu. Se
   aparecer um erro em vermelho dizendo que o caminho não existe, a pasta da primeira linha está errada: confira com a
   do passo 8.

### Fazer

3. **Configurações**: confira a **Pasta do jogo** (passo 8) e **desmarque** **Modo simular**.
4. **Servidores** → **Jogar** no servidor de teste.

**Esperado**:

- As etapas passam por "Conferindo o mod…"; o `launcher.log` tem `mod 0.1.0 (protocolo 1) instalado`.
- Existe `<pasta do jogo>\Phoenix\Binaries\Win64\cbmp\LEIA-ME.txt` e nada mais nessa pasta `cbmp`; **não** sobram
  `cbmp.staging` nem `cbmp.old`.
- O launcher mostra **"Jogo aberto. Bom jogo!"** e o Hogwarts Legacy abre (pode abrir pela Steam/Epic, aparecer a tela
  da conta WB Games etc., como sempre). Sem multiplayer: é o jogo normal. **Feche o jogo** depois que chegar ao menu.
- Clique **Jogar** de novo (com o jogo fechado): **Esperado**: não baixa o mod de novo (já instalado e intacto) e abre
  o jogo outra vez. Feche o jogo.

Anote no resultado: Steam ou Epic, se o jogo abriu, e qualquer mensagem da Steam/Epic/jogo.

**Se falhar**:

- `Sem permissão para gravar na pasta.` (comum na **Epic**, em `C:\Program Files\Epic Games`, que só o
  administrador pode alterar): **não** rode o launcher como administrador. Anote como **FALHOU (permissão)**, mande o
  `launcher.log` e vá para "Desfazer". É um resultado importante para o subprojeto 5.
- O jogo não abre ou a Steam/Epic mostra erro: print da mensagem, `launcher.log` (procure `jogo aberto` ou
  `Não foi possível abrir o jogo`).
- `O mod baixado não tem assinatura válida` ou `O pacote do mod é inválido`: mande o `launcher.log` e o `hub.log`.

### Desfazer (sempre, mesmo se deu certo)

5. Feche o jogo. Em **Configurações**, **marque de novo** **Modo simular**.
6. Apague **só** a pasta `cbmp` (e `cbmp.staging`/`cbmp.old`, se existirem) de dentro de
   `<pasta do jogo>\Phoenix\Binaries\Win64`. **Jeito recomendado, pelo Explorador**: abra a Pasta do jogo, entre em
   `Phoenix` → `Binaries` → `Win64`, clique **uma vez** na pasta chamada exatamente **`cbmp`** e aperte
   **Delete**. Não apague nada além dela: o resto de `Win64` é do jogo.

   Jeito alternativo, no PowerShell do item 2 (se fechou a janela, rode de novo a linha `$jogo = ...` do item 2). O
   comando só apaga entradas com esses três nomes exatos; com a pasta errada ele simplesmente não acha nada:

   ```
   Get-ChildItem -LiteralPath (Join-Path $jogo 'Phoenix\Binaries\Win64') -Filter cbmp* | Where-Object Name -in 'cbmp','cbmp.staging','cbmp.old' | Remove-Item -Recurse -Force
   ```

7. Confira com o mesmo comando `Get-ChildItem ... -Filter cbmp*` do item 2: **Esperado**: não mostra nada. A pasta
   do jogo está como antes do teste.

(Se depois disso você clicar **Jogar** com o modo simular **desligado**, o launcher instala a pasta `cbmp` de novo:
deixe o modo simular marcado.)

---

## 13. Limpar cache

1. **Configurações** → **Cache** → **Limpar cache** → aviso para fechar o jogo → **Limpar agora**.
   **Esperado**: "Cache limpo."; `%LOCALAPPDATA%\CBMultiplayer-TesteLocal\cache` e `servers` somem ou ficam vazias.
   (A limpeza do cache **não** mexe na pasta do jogo.)
2. **Jogar** de novo (modo simular **marcado**).
   **Esperado**: baixa tudo de novo e chega a "Pronto para abrir" como no passo 9.

**Se falhar**: print e `launcher.log`.

---

## 14. Sair, desinstalar, encerrar e limpar tudo

1. No launcher, **Sair**. **Esperado**: volta à tela de entrada; em Painel de Controle → **Gerenciador de
   Credenciais** → Credenciais do Windows → Credenciais genéricas não há entrada com `CBMultiplayer (Teste local)`.
2. Feche o launcher. Configurações do Windows → Aplicativos → Aplicativos instalados → **CB Multiplayer (Teste
   local)** → **Desinstalar**. **Esperado**: some do menu Iniciar e da lista.
3. Apague a pasta de dados `%LOCALAPPDATA%\CBMultiplayer-TesteLocal` (o desinstalador não apaga dados nem logs;
   **mande os logs antes**, se algo falhou).
4. Na janela do ambiente, **Ctrl+C** e `S`. **Esperado**: `[dev-env] encerrando…`. Depois dê dois cliques em
   **`PARAR.cmd`**: **Esperado**: `nada rodando (sem pids.json)`.
5. **Pasta do jogo**: confira de novo que não existe `<pasta do jogo>\Phoenix\Binaries\Win64\cbmp` (passo 12,
   "Desfazer"). Nada mais do jogo foi tocado.
6. (Opcional) No navegador, se você marcou "sempre permitir" o link `cbmp://` para `127.0.0.1:8787`, pode remover
   nas configurações de sites do navegador.
7. (Opcional) Apague `C:\cbmp-kit` inteiro (kit, dados do Hub de teste e logs). Para desinstalar o Node.js:
   Configurações do Windows → Aplicativos → **Node.js** → Desinstalar.

**Se falhar**: se o Ctrl+C não encerrar, use `PARAR.cmd` e mande o que ele mostrou.

---

## 15. O que este teste não cobre

- **Multiplayer dentro do jogo** e o **mod real** (subprojeto 5): o mod de teste é só um `LEIA-ME.txt`.
- **Hub de produção, login com o Discord de produção, atualizações e mods publicados no GitHub**: aqui tudo é o Hub
  local de teste, com a chave de **desenvolvimento**.
- **Assinatura Authenticode** do instalador (por isso o SmartScreen avisa) e o instalador **oficial** "CB
  Multiplayer": este é o build "Teste local", que só conversa com `127.0.0.1:8787` e usa outra pasta de dados.
- **Outros jogadores / outro PC**: o Hub de teste só aceita conexões deste PC e anuncia o servidor como
  `127.0.0.1`, então nenhum outro PC consegue entrar (o `cb-server` escuta em todas as interfaces na porta 30500,
  mas sem o Hub não há como conseguir um ticket; deixando o firewall bloqueado, como no passo 4, nem a porta fica
  acessível de fora).

---

## 16. Resultado (preencha e mande de volta)

Copie a tabela, preencha **Resultado** com `OK`, `FALHOU` ou `PULEI` e **Observação** com o que achar útil (mensagem
de erro, print, o que era diferente do Esperado). Mande junto os logs pedidos nos passos que falharam.

Dados do PC: Windows 11 versão ______ (Configurações → Sistema → Sobre) · `node --version`: ______ ·
Hogwarts Legacy na: Steam / Epic · Pasta do jogo: ____________________

| Passo | O que | Resultado | Observação |
|---|---|---|---|
| 1 | Node 24 instalado (`node --version`) | | |
| 2 | Kit copiado, hash confere, extraído em `C:\cbmp-kit` | | |
| 4 | Ambiente subiu com 3 `OK` | | |
| 4 | `CONFERIR.cmd`: 6 conferências OK | | |
| 5 | Instalador 0.1.0 (SmartScreen: apareceu? sim/não) | | |
| 5 | Pasta `CBMultiplayer-TesteLocal` criada; `CBMultiplayer` não | | |
| 6 | Login como Dev Local; continua logado ao reabrir | | |
| 6b | (Opcional) Login com app Discord de teste | | |
| 7 | Destaque, filtros, favoritos, ping, atualizar lista | | |
| 8 | Pasta do jogo achada sozinha (Steam/Epic) | | |
| 8 | Seletor recusa pasta errada e aceita a do jogo | | |
| 9 | Jogar (simular) → "Pronto para abrir" | | |
| 10 | Pasta montada com os 5 arquivos; `connect.json` some em 60 s; log limpo | | |
| 11 | Update 0.1.0 → 0.1.1 (sem SmartScreen), continua logado | | |
| 12 | Antes: não havia `cbmp*` na pasta do jogo | | |
| 12 | Mod de teste instalado (`cbmp\LEIA-ME.txt`, sem sobras) | | |
| 12 | Jogo abriu ("Jogo aberto. Bom jogo!"); 2º Jogar não rebaixou o mod | | |
| 12 | Desfeito: `cbmp` apagada, modo simular marcado | | |
| 13 | Limpar cache e baixar de novo | | |
| 14 | Sair (cofre limpo), desinstalar, encerrar ambiente | | |

Os passos 6b e 12 podem ser `PULEI` se você preferir não fazer; o resto é o teste principal.
