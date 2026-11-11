# Teste online do CB Multiplayer (guia do testador)

Obrigado por testar! Você só vai instalar um programa: o launcher **CB Multiplayer (Teste)**. Não precisa instalar
Node, servidor nem nada técnico. O teste leva uns 20 minutos.

**O que este teste cobre**: instalar o launcher, entrar com o seu Discord, ver o servidor de teste na lista, apertar
**Jogar** em **modo simular** (o launcher baixa e confere o conteúdo do servidor, mas **não abre o jogo** e **não mexe
na pasta do jogo**) e a atualização automática do launcher.

**O que ainda NÃO dá para testar**: jogar de verdade com outras pessoas. O mod do jogo ainda é um arquivo de exemplo;
abrir o Hogwarts Legacy pelo launcher não conecta a nada. Por isso **deixe sempre o Modo simular ligado**.

Precisa de: Windows 10 ou 11 (64 bits), conta no Discord, internet. Ter o Hogwarts Legacy instalado ajuda no passo 6,
mas não é obrigatório.

---

## 1. Baixar e instalar

1. Baixe o instalador: **https://cbmultiplayer.cntoficial.com.br/CB-Multiplayer-Teste-setup.exe**
2. Dê dois cliques no arquivo baixado.
3. **SmartScreen**: o instalador ainda **não tem assinatura digital** (o certificado é pago e ainda não existe), então
   o Windows pode mostrar **"O Windows protegeu o computador"**. Clique em **Mais informações** → **Executar assim
   mesmo**. Isso é esperado. (As **atualizações** têm assinatura própria, conferida pelo launcher.)
4. Siga o instalador (em português, **sem pedir administrador**) e deixe marcado para abrir o launcher no fim.

**Esperado**: o menu Iniciar tem **CB Multiplayer (Teste)**; a janela se chama "CB Multiplayer (Teste)" e mostra
**Entrar com Discord**.

## 2. Atualização automática

Assim que abre, o launcher procura atualização. A versão atual é a **0.1.3** (detecção do jogo na Epic com
confirmação). O instalador do passo 1 já é a 0.1.3; quem instalou antes (0.1.0, 0.1.1 ou 0.1.2) recebe a 0.1.3 por
aqui.

1. Se você está numa versão anterior: no menu lateral aparece **Atualização 0.1.3 disponível**. Clique nele (ou
   **Configurações** → **Procurar atualização**).
2. Aparece "Versão 0.1.3 disponível." com a nota "Versão de teste 0.1.3 do launcher CB Multiplayer (Teste)".
3. Clique em **Instalar atualização**.

**Esperado**: "Baixando e conferindo a atualização…"; o instalador roda sozinho (sem perguntas e sem SmartScreen) e o
launcher reabre. Em **Configurações** → **Versão** aparece **v0.1.3**, e **Procurar atualização** diz "Você já está
na versão mais recente." Se você instalou direto a 0.1.3, só confira a versão e o "Você já está na versão mais
recente."

## 3. Entrar com o Discord

1. Clique em **Entrar com Discord**. O launcher mostra "Aguardando o navegador…" e o seu navegador abre a página do
   Discord pedindo para autorizar o app de teste do CB Multiplayer (ele só lê o seu nome e avatar).
2. Clique em **Autorizar**.
3. O navegador pergunta se pode abrir o link `cbmp://` (CB Multiplayer (Teste)): **permita**.

**Esperado**: o launcher mostra a lista de servidores com o seu nome do Discord e o botão **Sair**. Feche e abra o
launcher: continua logado.

Se o navegador mostrar um texto técnico com `login_unavailable` e "login com Discord ainda não configurado neste
Hub", o login ainda não foi ligado: avise e
pare aqui.

## 4. Lista de servidores

1. No topo, o cartão **★ Destaque** mostra **CB Multiplayer – Servidor de teste**, com jogadores `0/32` e o botão
   **Jogar**.
2. Em poucos segundos a coluna **Ping** mostra um número em `ms`.
3. Teste a busca (`teste` mostra o servidor, `xyz` esconde), o chip `teste`, o filtro de idioma `pt-BR`, o **☆**
   de favorito (vira **★** e aparece em **Favoritos**) e **Atualizar lista**.

## 5. Jogar (modo simular)

1. Em **Configurações**, confira que **Modo simular** está **marcado** (é o padrão). **Não desmarque.**
2. Em **Servidores**, clique em **Jogar** no cartão de destaque.

**Esperado**:

- O botão vira "Entrando…", uma barra de download aparece e some rápido (são poucos KB vindos do servidor de teste).
- Aparece o painel **"Pronto para abrir — CB Multiplayer – Servidor de teste"** com "Modo simular: o conteúdo foi
  baixado e conferido, mas o jogo não foi aberto", Endereço `143.95.209.35`, Porta `30500` e a Pasta do conteúdo
  `C:\Users\<você>\AppData\Local\CBMultiplayer-Teste\servers\srv_...`.
- Abra essa pasta: tem `hello-client` com `client\main.lua`, `files\logo.png`, `ui\index.html`, `ui\app.js` e
  `ui\style.css`.
- O servidor aparece em **Recentes**.

## 6. Pasta do jogo (só se você tem o Hogwarts Legacy)

Em **Configurações**, a **Pasta do jogo** deve aparecer sozinha (Steam ou Epic). O launcher só **lê** o registro e os
arquivos da Steam/Epic; não grava nada na pasta do jogo.

**Epic Games**: a Epic deixa a pasta do jogo (e os arquivos dela que dizem onde o jogo está) alteráveis por outras
contas do computador. Por isso o launcher não usa essa pasta sem você confirmar, e não diz de onde ela veio. Em
**Configurações** → **Pasta do jogo** aparece:

> O launcher encontrou uma pasta que parece ser do Hogwarts Legacy:
> `C:\Program Files\Epic Games\HogwartsLegacy`
> O launcher não conseguiu verificar quem controla esta pasta: outra conta deste computador pode tê-la criado ou
> alterado. Use esta pasta só se foi nela que você instalou o jogo; se não tiver certeza, use "Escolher pasta do jogo".

1. Confira se o caminho é a pasta que você escolheu ao instalar o jogo pela Epic (no aplicativo da Epic, a opção
   **Gerenciar** do jogo na **Biblioteca** costuma mostrar a pasta). Se for, clique em **Usar esta pasta**.
2. O Windows mostra a janela **"Pasta do Hogwarts Legacy"** com o mesmo caminho e o mesmo aviso. Clique em **Usar esta
   pasta** (**Cancelar** não muda nada e a sugestão continua lá).

**Esperado**: a **Pasta do jogo** passa a mostrar o caminho e a sugestão some. A mesma sugestão aparece se você
apertar **Jogar** com o Modo simular desligado antes de confirmar.

Se aparecer "Mais de uma pasta parece ser do Hogwarts Legacy…", o launcher achou mais de uma indicação e não sugere
nenhuma: use **Escolher pasta do jogo** e anote isso no resultado. Se aparecer "Não encontrada" e nenhuma sugestão,
use **Escolher pasta do jogo** e anote que a detecção falhou (o log diz qual verificação falhou).

## 7. Resultados

Copie a tabela, marque **OK** ou **FALHOU** e mande junto com o log (passo 8):

| Passo | Resultado | Observação |
|---|---|---|
| 1. Instalar (SmartScreen, instalador, janela) | | |
| 2. Atualização para 0.1.3 (ou já instalado na 0.1.3) | | |
| 3. Login com Discord (e continua logado ao reabrir) | | |
| 4. Lista, destaque, ping, filtros, favoritos | | |
| 5. Jogar (modo simular): painel "Pronto para abrir" e pasta `hello-client` | | |
| 6. Pasta do jogo encontrada (Steam sozinha / Epic com "Usar esta pasta"?) | | |

Diga também: versão do Windows, se é Steam ou Epic (ou sem o jogo), e o que achou estranho ou confuso.

## 8. Como mandar o log

1. Aperte **Win + R**, cole `%LOCALAPPDATA%\CBMultiplayer-Teste\logs` e aperte Enter.
2. Mande o arquivo **`launcher.log`** (e prints das telas com erro) para quem te convidou para o teste.

O log não tem senha, token nem código de login (o launcher troca esses valores por `***`). Ele tem o seu ID e nome do
Discord e o caminho das pastas no seu PC.

## 9. Desinstalar (quando o teste acabar)

1. **Configurações do Windows** → **Aplicativos** → **Aplicativos instalados** → **CB Multiplayer (Teste)** →
   **Desinstalar**.
2. Apague a pasta `%LOCALAPPDATA%\CBMultiplayer-Teste` (Win + R, cole o caminho, apague a pasta).
3. Opcional: **Gerenciador de Credenciais** do Windows → **Credenciais do Windows** → apague a que tem `CBMultiplayer (Teste)` no nome.
4. Opcional: no Discord, **Configurações de usuário** → **Aplicativos autorizados** → remova o app de teste do CB
   Multiplayer.

Nada foi gravado na pasta do jogo (com o Modo simular ligado), então não há nada para desfazer nela.
