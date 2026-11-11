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

Assim que abre, o launcher procura atualização. Este teste começa na versão **0.1.0** de propósito: existe a **0.1.1**.

1. No menu lateral aparece **Atualização 0.1.1 disponível**. Clique nele (ou **Configurações** → **Procurar
   atualização**).
2. Aparece "Versão 0.1.1 disponível." com a nota "Versão de teste 0.1.1 do launcher CB Multiplayer (Teste)".
3. Clique em **Instalar atualização**.

**Esperado**: "Baixando e conferindo a atualização…"; o instalador roda sozinho (sem perguntas e sem SmartScreen) e o
launcher reabre. Em **Configurações** → **Versão** aparece **v0.1.1**, e **Procurar atualização** diz "Você já está
na versão mais recente."

## 3. Entrar com o Discord

1. Clique em **Entrar com Discord**. O launcher mostra "Aguardando o navegador…" e o seu navegador abre a página do
   Discord pedindo para autorizar o app de teste do CB Multiplayer (ele só lê o seu nome e avatar).
2. Clique em **Autorizar**.
3. O navegador pergunta se pode abrir o link `cbmp://` (CB Multiplayer (Teste)): **permita**.

**Esperado**: o launcher mostra a lista de servidores com o seu nome do Discord e o botão **Sair**. Feche e abra o
launcher: continua logado.

Se a página disser **"login com Discord ainda não configurado neste Hub"**, o login ainda não foi ligado: avise e
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
arquivos da Steam/Epic; não grava nada na pasta do jogo. Se aparecer "Não encontrada", use **Escolher pasta do jogo**
e anote no resultado que a detecção falhou.

## 7. Resultados

Copie a tabela, marque **OK** ou **FALHOU** e mande junto com o log (passo 8):

| Passo | Resultado | Observação |
|---|---|---|
| 1. Instalar (SmartScreen, instalador, janela) | | |
| 2. Atualização 0.1.0 → 0.1.1 | | |
| 3. Login com Discord (e continua logado ao reabrir) | | |
| 4. Lista, destaque, ping, filtros, favoritos | | |
| 5. Jogar (modo simular): painel "Pronto para abrir" e pasta `hello-client` | | |
| 6. Pasta do jogo encontrada sozinha (Steam/Epic?) | | |

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
