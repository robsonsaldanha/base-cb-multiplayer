# Teste no jogo do CB Multiplayer 0.2.0 (mod conecta e mostra os outros jogadores)

Roteiro para **duas pessoas**, cada uma no seu PC, com o **Hogwarts Legacy** instalado (Steam ou Epic) e um **save já
existente**. É a primeira vez que o mod roda de verdade dentro do jogo: o objetivo é ver os dois jogadores se
enxergando como bruxos extras e coletar os logs que provam (ou desmentem) as suposições que só o jogo responde.

Tempo: uns 20 minutos de preparo e **uns 60 a 90 minutos** de teste. Conversem por voz (Discord) durante o teste.

> **Estado deste roteiro**: a rodada ainda **não foi feita**. Este arquivo descreve o que fazer; os resultados entram
> na tabela do fim e viram pendência em `docs/PENDENCIAS.md` quando algo falhar. O mod 0.2.0 não pôde ser rodado
> no jogo antes disso (as máquinas de desenvolvimento não têm o jogo).

Convenções: **Pessoa A** e **Pessoa B** são os dois testadores. **Win64** é a pasta
`<pasta do jogo>\Phoenix\Binaries\Win64` (o launcher mostra a `<pasta do jogo>` em **Configurações** → **Pasta do
jogo**). Em **Steam**, também dá para chegar lá com botão direito no jogo → **Gerenciar** → **Procurar arquivos
locais**.

- [1. O que muda no seu PC](#1-o-que-muda-no-seu-pc)
- [2. Antes de começar](#2-antes-de-começar)
- [3. Os 10 passos](#3-os-10-passos)
- [4. Quando algo falha: o que mandar](#4-quando-algo-falha-o-que-mandar)
- [5. Remover o mod e voltar ao jogo original](#5-remover-o-mod-e-voltar-ao-jogo-original)
- [6. Tabela de resultados](#6-tabela-de-resultados)
- [7. Critério de aprovação](#7-critério-de-aprovação)
- [Anexo A. O que só o jogo responde](#anexo-a-o-que-só-o-jogo-responde)
- [Anexo B. Conferências avançadas (opcionais)](#anexo-b-conferências-avançadas-opcionais)
- [Anexo C. Ligar as opções do `cb-mp.json`](#anexo-c-ligar-as-opções-do-cb-mpjson)
- [Anexo D. Para quem conduz o teste](#anexo-d-para-quem-conduz-o-teste)

---

## 1. O que muda no seu PC

Nenhum arquivo original do jogo é alterado. O launcher **acrescenta** estes itens em **Win64** (e só eles):

| Onde (dentro de Win64) | O que é |
|---|---|
| `cbmp\atual\` | o pacote do mod 0.2.0 (inclui o UE4SS 3.0.1 e o `cb-mp`) |
| `cbmp\cbmp-run.json` | bilhete do launcher para o mod (sem senha nem ticket) |
| `cbmp\logs\`, `cbmp\crash\` | logs do mod e relatórios de falha (`.dmp`), criados pelo mod quando ele roda |
| `cbmp\cb-mp.json` | opcional: só existe se **você** criar (Anexo C) |
| `dwmapi.dll` | o carregador do UE4SS (cópia de `cbmp\atual\loader\dwmapi.dll`) |
| `override.txt` | uma linha: `cbmp\atual\ue4ss` |
| `cbmp-backup\` | só se você já tinha outro `dwmapi.dll` ou `override.txt` e confirmou o aviso: eles ficam guardados aqui e voltam quando o mod é removido |

A pasta `Win64\ue4ss` (de outro mod do UE4SS que você tenha) **nunca** é tocada pelo launcher. O launcher também
**nunca pede permissão de administrador**: se a pasta do jogo não aceita gravação do seu usuário, ele para com um
aviso (passo 1).

Detalhe importante: com o mod instalado, o UE4SS é carregado **toda vez** que o jogo abre, mesmo sem o launcher. Sem
o launcher, o `cb-mp` fica parado (não conecta, não cria log), mas o UE4SS está lá. Para ter o jogo 100% como
antes, remova o mod (seção 5).

## 2. Antes de começar

**Instalar ou atualizar** (as duas pessoas):

1. Launcher **CB Multiplayer (Teste) 0.2.0**.
   - Já tem o launcher de teste instalado (0.1.x): abra-o, ele oferece a atualização sozinho (menu lateral
     **Atualização 0.2.0 disponível**, ou **Configurações** → **Procurar atualização** → **Instalar atualização**).
   - Não tem: baixe **https://cbmultiplayer.cntoficial.com.br/CB-Multiplayer-Teste-setup.exe**. O instalador não tem
     assinatura digital: no SmartScreen use **Mais informações** → **Executar assim mesmo** (detalhes em
     [`TESTE-ONLINE.md`](TESTE-ONLINE.md)).
   - Confirme em **Configurações** → **Versão**: deve aparecer **v0.2.0**.
2. O **mod 0.2.0** não é instalado à mão: o launcher baixa (uns 12 MB), confere a assinatura e instala sozinho na
   primeira vez que você aperta **Jogar** com o **Modo simular desligado**. Nada a fazer agora.
3. Em **Configurações** → **Avançado**, **desmarque** "Modo simular: prepara tudo e não abre o jogo". (No 0.2.0 o
   padrão já é desmarcado, mas confira: com ele ligado o jogo **não abre**.)
4. Em **Configurações** → **Pasta do jogo**, confira se é a pasta certa do Hogwarts Legacy. Se não achou sozinho, use
   **Escolher pasta do jogo**. Na Epic, pode aparecer a sugestão "Usar esta pasta" (veja `TESTE-ONLINE.md`, passo 6).

**Pré-requisitos**: Windows 10 ou 11 (64 bits); conta no Discord; internet; jogo **fechado**; um **save existente**
(não comece jogo novo); Steam ou Epic abertas como de costume; a mesma hora marcada entre as duas pessoas.

**Antes de abrir o jogo pela primeira vez**: abra a pasta **Win64**, veja se já existe `cbmp\logs` e anote a data do
arquivo mais novo (ou "não existe"). Isso serve para o passo 8.

**Antivírus**: se o Windows Defender ou outro antivírus apontar `dwmapi.dll` ou `UE4SS.dll`, **anote** o aviso (e
mande um print). Não desligue a proteção do computador sem avisar quem conduz o teste.

**Se o jogo mostrar a janela "UE4SS Error" e fechar** (por exemplo "Failed to load UE4SS.dll", quando o antivírus
apagou ou pôs em quarentena o `UE4SS.dll`; acontece também abrindo pela Steam/Epic): aperte **Jogar** de novo no
launcher (ele confere os arquivos do mod e reinstala o que falta) ou use **Remover mod do jogo** (seção 5). Se o
jogo **continuar sem abrir**, feche o launcher e apague à mão, em **Win64** (`<pasta do jogo>\Phoenix\Binaries\Win64`),
os arquivos `dwmapi.dll` e `override.txt`; se existir a pasta `cbmp-backup`, devolva os arquivos dela para Win64
(seção 5). Anote o que aconteceu e avise quem conduz o teste.

## 3. Os 10 passos

Para cada passo: **Fazer**, **Esperado** e **Anotar**. Marque o resultado na tabela da seção 6.

**Guardar os arquivos** (quando um passo pedir, sempre com o jogo **fechado**): crie na **Área de Trabalho** a pasta
`cbmp-teste-5a` e, dentro dela, uma pasta com o nome que o passo indicar (por exemplo `1-passo-6`). **Copie** para
ela (copiar, não mover):

- a pasta `Win64\cbmp\logs` inteira (logs do mod);
- a pasta `Win64\cbmp\crash`, se existir (relatórios de falha `.dmp`);
- o arquivo `Win64\cbmp\atual\ue4ss\UE4SS.log`;
- a pasta `%LOCALAPPDATA%\CBMultiplayer-Teste\logs` (log do launcher; **Win + R**, cole o caminho, **Enter**).

Por que copiar na hora: cada vez que o jogo abre, o UE4SS **recomeça** o `UE4SS.log` do zero; o mod guarda só os
**10 logs mais novos** (dois por abertura, ou seja, umas 5 aberturas) e apaga os mais antigos; e **Remover mod do
jogo** (passo 9) apaga `cbmp\atual`, com o `UE4SS.log` dentro. **Regra**: se precisar fechar o jogo e apertar
**Jogar** de novo fora do roteiro (entrada expirou, "desconectado", travou), guarde os arquivos **antes** de abrir de
novo, numa pasta `extra-1`, `extra-2`, … e anote o porquê.

### Passo 1. Abrir o launcher e apertar Jogar (as duas pessoas)

**Fazer**

1. Abra o **CB Multiplayer (Teste)**. Entre com o Discord se pedir.
2. Confira **Configurações** → **Versão** (**v0.2.0**) e **Pasta do jogo**.
3. Em **Servidores**, aperte **Jogar** no cartão **CB Multiplayer – Servidor de teste**.
4. Se aparecer uma tela de **conflito** (outro carregador de mods do UE4SS encontrado), leia e siga a tela: o texto diz
   que os seus arquivos **não são apagados** e ficam em `Phoenix\Binaries\Win64\cbmp-backup`. Aceitar
   (**Usar o carregador do CB Multiplayer**) move os seus `dwmapi.dll` e `override.txt` para essa pasta; eles
   voltam quando o mod for removido. **Anote** que o conflito apareceu.
5. Se aparecer o erro **"O launcher não tem permissão para gravar na pasta do jogo (...)"** (pasta protegida): **não
   insista**. Anote o texto completo e a pasta mostrada, e pare: a solução desta versão é instalar o jogo numa
   pasta em que o seu usuário grave (por exemplo, outra biblioteca da Steam). O launcher não muda nada na pasta quando
   isso acontece.
6. Se aparecer "O Hogwarts Legacy está aberto. Feche o jogo e tente de novo.", feche o jogo (e o que sobrou dele no
   Gerenciador de Tarefas) e aperte **Jogar** de novo.

**Esperado**: o botão passa por "Entrando…" (ticket, download do mod na primeira vez), e o **jogo abre sozinho**.
Em **Configurações**, **Mod do jogo** mostra **Instalado 0.2.0**.

**Anotar**: se houve conflito, erro de permissão ou aviso do antivírus; quanto tempo o launcher levou até abrir o jogo.

### Passo 2. Carregar o save e ir ao pátio de Hogsmeade (as duas pessoas)

**Fazer**: no menu do jogo, escolha **Continuar** / **Carregar** o save existente (não comece jogo novo) e vá até o
**pátio de Hogsmeade**. As duas pessoas vão para o **mesmo lugar**, mas **não precisam chegar juntas** agora.

**Importante**: se uma pessoa demorar **mais de 4 minutos** entre apertar **Jogar** e chegar ao menu (por exemplo,
compilação de shaders na primeira abertura), a entrada expira. Nesse caso **feche o jogo**, **guarde os arquivos**
(início da seção 3, pasta `extra-1`), **aperte Jogar de novo** e **anote** quanto demorou (Anexo A, item A32).

**Esperado**: o jogo carrega normalmente, sem engasgos fora do comum (compare com o que o jogo faz sem o mod).

**Anotar**: tempo até o menu e até o mundo; engasgos no carregamento ou no menu (Anexo A, item A24).

### Passo 3. Ver "CB Multiplayer: conectado" e o bruxo extra (as duas pessoas)

**Fazer**

1. Com o personagem **dentro do mundo** (já controlando, em Hogsmeade), olhe o canto da tela onde o jogo mostra
   notificações. Procure a mensagem **CB Multiplayer: conectado (N por perto)**.
2. Caminhem **um até o outro**, até se verem.
3. Cada pessoa deve ver **um bruxo extra** parado ou andando perto do outro.

**Esperado**: a mensagem aparece uma vez quando o personagem entra no mundo; ao chegarem perto, cada tela mostra o
outro jogador como um bruxo (aparência provavelmente igual à do seu próprio personagem: é um clone).

O **N** é quantos jogadores o servidor já tinha anunciado **no instante em que a mensagem aparece**. Na primeira
entrada ele costuma ser **0**, mesmo com o outro jogador ao lado: o servidor só sabe onde você está depois que o mod
manda a sua posição, o que acontece logo depois da mensagem. **0 aqui não reprova o passo**; o bruxo extra aparece
alguns segundos depois. O log do mod repete a mensagem com o mesmo N (`status: CB Multiplayer: conectado (N por
perto)`), e os números que contam vêm depois: `EntityEnter id=<n>` quando o outro é anunciado e `bonecos=<n>` na
linha `contadores …` a cada 10 s.

**Se a mensagem não aparecer**, isso **não** reprova o passo sozinho: o log diz se a conexão aconteceu (Anexo A, item
A18). O que reprova é **não ver o outro jogador**.

**Anotar**: a mensagem apareceu? Com quantos "por perto" (o N exato da tela)? A que distância o bruxo extra
aparece? A aparência (roupa, varinha, parado em pose estranha?).

### Passo 4. Andar, correr, pular, parar; trocar (as duas pessoas)

**Fazer**: a **Pessoa A** anda por **30 segundos** em volta da **Pessoa B** (que fica parada e observa), corre,
pula e para. Depois **trocam** de papel.

**Esperado**: quem observa vê o bruxo extra seguindo o outro **sem teleportar** de ponto em ponto.

**Anotar** (cada pergunta com "sim", "não" ou uma frase):

- O bruxo extra **desliza** parado (fica andando sem mexer as pernas, ou mexe as pernas parado)?
- **Treme** ou vibra?
- Tem **atraso** perceptível? Quanto, mais ou menos (meio segundo? um segundo?)?
- As **pernas animam** quando o outro anda (Anexo A, item A16)?
- Ele **cai no chão** / fica **flutuando** / **atravessa** paredes e escadas (Anexo A, item A29)?
- Ele **empurra** o seu personagem ou **bloqueia** o caminho (A29)?
- Quadros por segundo caem com o bruxo extra por perto (A28)?

### Passo 5. Viagem rápida (uma pessoa)

**Fazer**: a **Pessoa A** usa **viagem rápida** para outro ponto do mapa e volta **a pé ou voando** para perto da
Pessoa B. A Pessoa B observa.

**Esperado**: o bruxo extra some enquanto a Pessoa A carrega e **reaparece** quando ela volta. Ninguém é
**expulso** (nenhuma mensagem "desconectado — expulso…").

**Anotar**: sumiu e reapareceu? Quanto tempo levou para reaparecer? Apareceu alguma mensagem na tela? (Anexo A, item
A25 olha o log: `Input Apparate` e `boneco id=… recriado`.)

### Passo 6. Uma pessoa fecha o jogo (as duas)

**Fazer**: a **Pessoa A** fecha o jogo (menu → Sair, ou fechando a janela). A **Pessoa B** continua jogando por **2
minutos** e observa.

**Esperado**: para a Pessoa B, o bruxo extra **some em cerca de 15 segundos** e o jogo dela segue **normal** durante
os 2 minutos, sem travar, sem mensagem de erro e sem perder o controle.

**Anotar**: em quanto tempo o bruxo sumiu? O jogo da Pessoa B teve algum problema? A Pessoa A: o jogo fechou
normalmente, travou, deu mensagem de erro? Havia arquivo novo em `Win64\cbmp\crash` (Anexo A, itens A21 e A22)?

**Depois** (só a **Pessoa A**, com o jogo fechado, **antes** do passo 7): **guarde os arquivos** (início da seção 3)
na pasta `1-passo-6`. O passo 7 abre o jogo de novo e apaga o `UE4SS.log` desta sessão.

### Passo 7. Reabrir pelo launcher (as duas pessoas)

**Fazer**: a **Pessoa A** abre o launcher e aperta **Jogar** de novo (o mod já está instalado: não baixa de novo).
Todos voltam ao mundo e repetem o passo 3 (mensagem e bruxo extra).

**Esperado**: igual ao passo 3. Dois **logs novos** aparecem em `Win64\cbmp\logs` (um por processo do jogo).

**Anotar**: funcionou de primeira? Se não, qual mensagem apareceu na tela (as mensagens possíveis estão no Anexo
A, item A18)?

**Depois** (as duas pessoas): **feche o jogo** e **guarde os arquivos** (início da seção 3) na pasta `2-passo-7`.
Anote a data e a hora do log mais novo de `Win64\cbmp\logs` (serve para o passo 8).

### Passo 8. Abrir o jogo pela Steam ou Epic, SEM o launcher

**Fazer**: com o jogo fechado (fim do passo 7), feche também o **launcher**. Abra o Hogwarts Legacy **pela Steam ou
pela Epic**, como você sempre abriu (**não** pelo botão Jogar do launcher). Carregue o save, jogue uns 2 minutos e
**feche o jogo**. Depois **guarde os arquivos** (início da seção 3) na pasta `3-passo-8`: o `UE4SS.log` desta abertura
é a prova do item A1 e o passo 9 o apaga.

**Esperado**: o jogo roda **normal**, **sem** a mensagem "CB Multiplayer" e **sem nenhum arquivo novo** em
`Win64\cbmp\logs` (os logs dos passos anteriores continuam lá; compare com o mais novo que você anotou depois do
passo 7: nenhum arquivo mais novo pode aparecer).

**Anotar**: apareceu alguma mensagem? Apareceu log novo (então o mod não ficou parado, e isso reprova o passo)? O
`UE4SS.log` (Win64\cbmp\atual\ue4ss) foi atualizado e mostra só o carregamento do `cb-mp` (é normal: o UE4SS carrega).

### Passo 9. Remover o mod do jogo

**Fazer**: confira que a pasta `cbmp-teste-5a` da Área de Trabalho tem as cópias dos passos 6 (só a Pessoa A), 7 e 8:
este passo **apaga** `cbmp\atual`, com o `UE4SS.log` dentro. Com o jogo **fechado**, no launcher: **Configurações** → **Mod do jogo** → **Remover mod do jogo** →
**Remover agora**. Depois abra a pasta **Win64**.

**Esperado**: aparece "Mod removido do jogo." O `dwmapi.dll` e o `override.txt` **saíram** de Win64 e a pasta
`cbmp\atual` também. Os seus arquivos guardados em `cbmp-backup` (se você aceitou o conflito) **voltaram** para o
lugar. `cbmp\logs` e `cbmp\crash` **continuam** (são seus logs). Abra o jogo pela Steam/Epic: ele **abre normal**.

**Anotar**: o que ficou em Win64 e o que sumiu; se o jogo abriu; se algum arquivo seu não voltou.

### Passo 10. Mandar os arquivos

Compacte a pasta `cbmp-teste-5a` da Área de Trabalho (botão direito → **Enviar para** → **Pasta compactada**) e
mande para quem conduz o teste, junto com a sua tabela da seção 6 e os prints. **Mande sempre**, mesmo que tudo
tenha funcionado: com tudo certo, os logs são a prova do Anexo A. Não tire os arquivos de dentro do jogo de novo:
depois do passo 9 o `UE4SS.log` já não existe lá, e as cópias dos passos 6, 7 e 8 são o que vale.

## 4. Quando algo falha: o que mandar

Se algo falhar **no meio** do teste: feche o jogo, **guarde os arquivos** (início da seção 3) numa pasta com o nome
do passo (por exemplo `falha-passo-4`) **antes** de abrir o jogo de novo, e mande a pasta `cbmp-teste-5a`
compactada. A tabela abaixo diz o que cada cópia contém e onde o original fica.

| Arquivo | Onde fica | Para quê |
|---|---|---|
| Logs do mod | `<jogo>\Phoenix\Binaries\Win64\cbmp\logs\` (todos os `cb-mp-AAAAMMDD-HHMMSS-<pid>.log`) | **dois arquivos por abertura**: um por processo do jogo. A hora começa cada linha. No launcher: **Configurações** → **Abrir pasta de logs do mod** |
| Relatório de falha | `<jogo>\Phoenix\Binaries\Win64\cbmp\crash\` (arquivos `cb-mp-AAAAMMDD-HHMMSS.dmp`, **só existem se o jogo caiu**) | a única prova de que o jogo caiu por causa do mod |
| `UE4SS.log` | `<jogo>\Phoenix\Binaries\Win64\cbmp\atual\ue4ss\UE4SS.log` (recomeça a cada abertura do jogo e some com **Remover mod do jogo**: só vale a cópia feita na hora) | o que o UE4SS carregou e se o `cb-mp` subiu |
| Log do launcher | `%LOCALAPPDATA%\CBMultiplayer-Teste\logs\` (`launcher.log` e os antigos `launcher.log.1`, …; Win + R, cole o caminho) | o que o launcher fez no Jogar, no conflito e na remoção |
| Prints e anotações | a sua tabela da seção 6 | as mensagens exatas da tela |

**Privacidade**: os logs têm o endereço do servidor de teste, caminhos de pastas do seu PC e (o do launcher) o seu
nome do Discord. **Nunca** têm senha, ticket nem token (o mod e o launcher trocam esses valores por `***`). O
`.dmp` guarda pilhas e módulos do jogo, e pode ter o nome da sua pasta de usuário: mande só a quem conduz o teste.

**Mensagens que podem aparecer na tela do jogo** (anote o texto exato se aparecer):

- `CB Multiplayer: conectado (N por perto)`
- `CB Multiplayer: desconectado — entrada expirou; feche o jogo e clique Jogar`
- `CB Multiplayer: desconectado — arquivo de entrada inválido`
- `CB Multiplayer: desconectado — versão do mod diferente da do servidor`
- `CB Multiplayer: desconectado — servidor não respondeu`
- `CB Multiplayer: desconectado — recusado: <texto>`
- `CB Multiplayer: desconectado — conexão perdida`
- `CB Multiplayer: desconectado — servidor incompatível`
- `CB Multiplayer: desconectado — erro interno`
- `CB Multiplayer: o servidor não aceitou sua posição`
- `CB Multiplayer: desconectado — <texto do servidor>` (quando você é expulso)

Depois de qualquer "desconectado", **o mod não reconecta**: feche o jogo, **guarde os arquivos** (pasta `extra-<n>`)
e aperte **Jogar** de novo.

## 5. Remover o mod e voltar ao jogo original

1. Feche o jogo. Se ainda vai mandar logs, **guarde os arquivos** antes (início da seção 3): o próximo item apaga o
   `UE4SS.log`.
2. No launcher: **Configurações** → **Mod do jogo** → **Remover mod do jogo** → **Remover agora** (passo 9). Isso apaga
   `dwmapi.dll` e `override.txt` **se forem os do CB Multiplayer**, devolve os seus arquivos guardados em
   `cbmp-backup` e apaga `cbmp\atual` (com o `UE4SS.log` dentro).
3. Para **limpar tudo** o que sobrou (só depois de mandar os logs): apague a pasta `Win64\cbmp` (logs, relatórios de
   falha, `cb-mp.json`) e, se estiver vazia, a pasta `Win64\cbmp-backup`.
4. Para tirar também o launcher: **Configurações do Windows** → **Aplicativos** → **CB Multiplayer (Teste)** →
   **Desinstalar**, e apague `%LOCALAPPDATA%\CBMultiplayer-Teste` (guarda a cópia assinada do mod em `mods\package`).
5. Confirme que o jogo abre pela Steam/Epic normalmente. Se achar que algo ficou diferente, a Steam tem **Verificar
   integridade dos arquivos locais** (Propriedades → Arquivos instalados); a Epic tem **Verificar** em
   **Gerenciar**.

Se o launcher **não** abrir (ou o botão de remover falhar), apague à mão, com o jogo fechado, em Win64:
`dwmapi.dll`, `override.txt` e a pasta `cbmp`; e, se existir `cbmp-backup`, devolva os arquivos dele (`dwmapi.dll`,
`override.txt`) para Win64 antes de apagar a pasta. Se você usava outro mod do UE4SS, ele só volta a funcionar com
os seus arquivos de volta.

Se o jogo mostrar a janela **"UE4SS Error"** e fechar (antivírus tirou o `UE4SS.dll`), veja o que fazer na seção 2,
em **Se o jogo mostrar a janela "UE4SS Error" e fechar**.

## 6. Tabela de resultados

Copie, marque **OK** ou **FALHOU** e escreva o que viu na **Observação**. Uma tabela por pessoa (A e B), ou uma só com
"A: ... / B: ...".

| Passo | O que se espera | OK | FALHOU | Observação |
|---|---|---|---|---|
| P1 Jogar | launcher 0.2.0, mod 0.2.0 instalado, jogo abre sozinho (e conflito/erro de permissão, se houver, anotados) | | | |
| P2 Save | save carrega até o pátio de Hogsmeade, sem engasgos fora do comum | | | |
| P3 Conectado | mensagem "CB Multiplayer: conectado" e **cada um vê o bruxo extra** do outro | | | |
| P4 Movimento | andar/correr/pular/parar sem teleportes; anotou desliza, treme, atraso | | | |
| P5 Viagem rápida | bruxo some e reaparece; ninguém é expulso | | | |
| P6 Fechar o jogo | bruxo some em cerca de 15 s; o outro joga normal por 2 min | | | |
| P7 Reabrir | repete o passo 3 pelo launcher; dois logs novos | | | |
| P8 Sem launcher | jogo normal, sem mensagem e sem log novo | | | |
| P9 Remover | `dwmapi.dll` e `override.txt` saíram; jogo abre normal | | | |
| P10 Arquivos | pasta `cbmp-teste-5a` enviada, com as cópias dos passos 6 (Pessoa A), 7 e 8 (logs, `.dmp` se houver, `UE4SS.log`, logs do launcher) | | | |

Complete também a tabela do **Anexo A** (quem conduz o teste confere com os logs) e diga: versão do Windows, Steam
ou Epic, placa de vídeo, e o que achou estranho ou confuso.

## 7. Critério de aprovação

- **5a validado** quando os passos **3, 4, 6 e 8** forem aprovados.
- Se os passos 1, 2, 5, 7, 9 ou 10 (ou qualquer item dos anexos) falharem, **o 5a não é reaberto**: cada falha vira
  pendência registrada em `docs/PENDENCIAS.md`, com o log que a prova.
- Se o passo 3 falhar, o resto não pode ser julgado: mande os logs e pare.

---

## Anexo A. O que só o jogo responde

Cada linha é uma suposição do mod que **só o jogo confirma**. Quem conduz o teste abre os logs de
`Win64\cbmp\logs` (Bloco de Notas, **Ctrl + F**) e confere: **Confirma** = a linha esperada aparece; **Refuta** = a
linha que aparece se a suposição estiver errada. Os textos de log entre crases são **exatos** (o resto da linha
é livre). Cada linha do log começa com data e hora (`[aaaa-mm-dd hh:mm:ss.mmm]`) e o nível (`INFO`, `AVISO`, `ERRO`).

Marque **OK** ou **FALHOU** e anote o que viu. Itens que dependem de uma opção do `cb-mp.json` dizem "só com" e o
modo de ligar está no Anexo C.

| # | O que se confere | Confirma | Refuta | OK | FALHOU | Observação |
|---|---|---|---|---|---|---|
| A1 | Sem launcher o mod fica parado | nenhuma pasta `cbmp\logs` nova nem arquivo novo; o `UE4SS.log` mostra só o carregamento do `cb-mp` | qualquer `cb-mp-*.log` novo (passo 8) | | | |
| A2 | Com o launcher, o mod lê o bilhete | `==== cb-mp 0.2.0 pid=<pid> epoca=<n> ====` e `armado: launchId=…` | ausência dessas linhas com o launcher aberto | | | |
| A3 | Dois processos na abertura; o mais novo é o dono | dois logs, com `epoca=1` e `epoca=2`; no primeiro `substituido epoca=1→2`, no segundo `dono epoca=2` | `portão de processos incompleto (seção ou mutex não criados)` | | | |
| A4 | Crash handler e configuração | `crash handler instalado (filtro anterior: sim)` ou `(filtro anterior: não)`; `config: statusNaTela=… neutralizacaoExtra=… animacaoBoneco=… maxBonecos=…` | (nenhuma) | | | |
| A5 | O jogo entrega a inicialização e o tick roda | `Unreal inicializado`, `tick registrado`, `primeiro tick` | `falha ao registrar o tick (ERROR_ID) - veja UE4SS.log`; sem `primeiro tick`; `falha ao armar o mod; inerte: …`; `falha ao iniciar a sessão: …` | | | |
| A6 | A rede inicia dentro do jogo | `conectando a <ip> porta <p> servidor <id>` depois do resgate | `GameNetworkingSockets não iniciou: …; sem sessão` | | | |
| A7 | Achar o motor, o viewport e o mundo | `jogo: GameEngine <classe>` (uma vez) | `jogo: GameEngine não encontrado (ainda)` persistente; `jogo: propriedade GameViewport ausente em …`; `jogo: propriedade World ausente em …` | | | |
| A8 | Achar o PlayerController e o pawn do mundo atual | `jogo: pawn local trocou (frontend=…)` com `mundo: RootLevel`, `PlayerController: … (BP_Phoenix_Player_Controller_C)` e `Pawn: … (BP_Biped_Player_C)`; depois o mesmo com `mundo: Overland`; e `mundo: … → Overland InWorld` | `Pawn: null` em `Overland`; `jogo: propriedade Pawn ausente em …` | | | |
| A9 | Saber se está no menu (`frontendHint`) | `jogo: UIManager <nome>`; `frontend=true` no menu (`RootLevel`) e `frontend=false` em `Overland` | `jogo: classe UIManager não encontrada; …`; `jogo: UIManager.Get não encontrada; …`; `jogo: UIManager.IsInFrontendLevel não encontrada; …`; ou `frontend=false` no menu | | | |
| A10 | Ler posição, rotação, velocidade e modo de movimento | o outro jogador vê o bruxo no lugar certo e na direção certa | `jogo: RelativeLocation/RelativeRotation ausentes no RootComponent`; `jogo: CharacterMovement sem Velocity; velocidade 0`; `jogo: CharacterMovement sem MovementMode; modo Other` | | | |
| A11 | Teleporte local numa correção curta (100 a 2000 cm) | `jogo: teleporte local via K2_TeleportTo` (só aparece se o servidor corrigir; ver `Correction seq=… distancia=…`) | `… via K2_SetActorLocation` (reserva usada) ou `jogo: teleporte local indisponível (…)` | | | |
| A12 | Criar o bruxo extra a partir da classe do jogador | `jogo: bonecos da classe BP_Biped_Player_C` e `boneco id=<n> criado` | `SpawnPuppet: exceção SEH …`; nenhum bruxo visível | | | |
| A13 | Neutralizar o bruxo extra (sem colisão, sem ser percebido) | `boneco: SetActorEnableCollision ok`, `boneco: SetInvulnerable ok`, `boneco: SetAutoRegisterAsSource ok`, `boneco: UnregisterFromPerceptionSystem ok`, `boneco: EnablePawnInteraction ok` (e `boneco: DetachFromControllerPendingDestroy ok` se nasceu com controller) | `boneco: <passo> indisponível`; `boneco: <passo> falhou: …`; `jogo: classe CognitionStimuliSourceComponent não encontrada`; `boneco: sem componente CognitionStimuliSourceComponent`; `jogo: Actor.GetComponentByClass não encontrada` | | | |
| A14 | O bruxo extra não toma o lugar do jogador no jogo | `singleton=ok` | `singleton trocado pelo boneco; depois de destruir: …`; `jogo: classe Biped_Player não encontrada; …`; `jogo: Biped_Player.GetTheBipedPlayer não encontrada; conferência desligada` (conferência desligada = **não confirmado**) | | | |
| A15 | Mover o bruxo extra | `jogo: bonecos movidos via K2_SetActorLocationAndRotation` e o bruxo anda liso | `… via K2_TeleportTo` ou `… via RootComponent.K2_SetWorldLocationAndRotation` (alternativas; **anote qual**); `jogo: nenhuma função para mover bonecos (…)` | | | |
| A16 | Animação `velocidade` (padrão) | as pernas animam quando o outro anda | `boneco: animação desligada: …`; o bruxo desliza parado | | | |
| A17 | Animação `ambulatory` (só com `animacaoBoneco: "ambulatory"`) | animação visível, sem aviso | `boneco: animação desligada: SetWorldSpeedAndDirection…` ou `…SetIsInIdle…` | | | |
| A18 | Texto na tela (`UIManager.AddTextTickerNotification`) aceita texto livre e acentos | `jogo: status na tela via UIManager.AddTextTickerNotification(NotificationText)`, e `status: CB Multiplayer: …` no log da sessão, e o texto aparece no canto da tela | `jogo: UIManager.AddTextTickerNotification não encontrada`; `status na tela desligado: …`; texto não aparece ou acentos errados | | | |
| A19 | Destruir o bruxo extra e trocar de mundo | o bruxo some quando o outro sai; `bonecos esquecidos (mundo trocado): <n>` ao voltar ao menu | `DestroyPuppet: …` com `exceção SEH` | | | |
| A20 | Neutralização extra (só com `neutralizacaoExtra: true`) | `boneco: CameraStackComponent.Deactivate ok` e `… .SetComponentTickEnabled ok` (idem `InputWatcherComponent` e `BP_PlayerPerceptionPoint_C`); `boneco: StartSettingTargetableByAiState ok` | `jogo: classe <X> não encontrada`; `boneco: sem componente <X>`; `boneco: <X>.<passo> falhou: …` | | | |
| A21 | Nenhuma chamada ao jogo derruba o jogo | ausência de `exceção SEH (provável ponteiro inválido)` no log e **nenhum** arquivo novo `cb-mp-*.dmp` em `Win64\cbmp\crash` (o filtro de falha **não escreve no log**: a prova é o `.dmp`) | qualquer um dos dois | | | |
| A22 | Fechar o jogo normalmente | **sem** `encerrando` e **sem** `cb-mp descarregado` no log (com o processo terminando, o UE4SS não descarrega o mod; **é o esperado**); o servidor tira o jogador por tempo esgotado em cerca de 10 s | **`.dmp` ao fechar**; jogador preso no servidor muito depois de 10 s | | | |
| A23 | O bruxo extra não toma o controle do jogador | ao surgir o primeiro bruxo (`boneco id=<n> criado`) **não** aparece `jogo: pawn local trocou` logo depois, e o jogador continua com o controle | `boneco: nasceu com o PlayerController local; DetachFromControllerPendingDestroy pulado`; `jogo: pawn local trocou` logo depois do primeiro bruxo; perda de controle | | | |
| A24 | Custo por tick | sem engasgos perceptíveis no carregamento e no menu com o mod armado | engasgos que somem sem o mod (então: baixar a frequência da procura do controller) | | | |
| A25 | Os caches da reflexão sobrevivem à troca de mundo | Overland → menu → Overland (e uma viagem rápida): os bruxos voltam (`boneco id=<n> recriado`), andam e animam como antes; nenhum `exceção SEH` depois da volta | `exceção SEH` ou `propriedade … com tipo inesperado` só depois da troca de mundo; bruxo parado ou no lugar errado depois da volta | | | |
| A26 | `frontendHint` falhando não derruba a amostra | ausência de `jogo: frontendHint falhou; frontendHint = false: …` | essa linha (o mod segue, mas o menu passa a depender só do nome `RootLevel`) | | | |
| A27 | Qual dos dois processos sobrevive e quanto tempo passa entre eles | nos dois logs da abertura: hora de início (nome do arquivo), `primeiro tick`, `dono epoca=…` e a hora do `Hello enviado`; **anote os intervalos** (numa abertura vista no reconhecimento, os dois processos começaram com cerca de 33 s de diferença e rodaram juntos por cerca de 13 s) | só um log tem `Hello enviado`, e o outro `substituido`: normal; **a sessão não começar** (nenhum `Hello enviado` em nenhum log) é a falha, e os tempos servem para ajustar o prazo de espera (`kClaimDelay`) | | | |
| A28 | Quadros por segundo com N bruxos extras | queda pequena e estável com N = 1 (anote o FPS sozinho e com o outro por perto; N maior se mais gente entrar) | queda grande ou engasgos só com o bruxo por perto (o custo de clonar o jogador é a preocupação) | | | |
| A29 | O bruxo extra cai, atravessa ou empurra? | o bruxo fica sobre o chão, não empurra nem bloqueia o seu personagem (a colisão dele está desligada) | cai no vazio, afunda, empurra ou bloqueia | | | |
| A30 | Inimigos reagem ao bruxo extra? | inimigos **não** o percebem como jogador (A13 ok) | inimigos atacam ou olham para o bruxo extra | | | |
| A31 | Câmera, HUD e singleton do jogador | câmera e HUD seguem **o seu** personagem o tempo todo; `singleton=ok` | câmera pula para o bruxo, HUD some ou muda, `singleton trocado pelo boneco` | | | |
| A32 | Mais de 4 minutos até o menu | `desconectado: sem_arquivo` ou `desconectado: ticket_expirado` e, na tela, "entrada expirou; feche o jogo e clique Jogar" (esperado nesse caso) | conexão que deveria existir e não existe, sem esses motivos | | | |
| A33 | Epic: pasta, permissão e abrir direto pelo executável | o jogo abre pelo launcher; o `UE4SS.log` é gravado; o mod instala | `mod_folder_protected` (pasta sem permissão), o jogo fecha ou pede login ao abrir direto, `UE4SS.log` ausente | | | |
| A34 | Cada chave do `cb-mp.json` liga e desliga o que diz | ver Anexo C: o log `config: …` mostra o valor lido e o efeito é visível | o log mostra o valor padrão mesmo com o arquivo criado | | | |

> A18 e A5 também são provas do passo 3: se a mensagem do canto da tela não aparecer, o log diz se foi a conexão ou
> só o texto.

**Linhas do log que provam o passo 3 em diante** (de `Win64\cbmp\logs`, nesta ordem, para cada pessoa):

| Prova | Linha a procurar (o resto é livre) | Em qual log |
|---|---|---|
| O processo certo resgatou o bilhete | `substituido epoca=<a>→<b>` (no que foi trocado) e `dono epoca=<n>` | um em cada arquivo |
| Entrou no servidor | `Hello enviado`, depois `Welcome playerId=<n>` | no log do dono |
| Está no mundo | `mundo: <de> → <para>` com `InWorld` | no log do dono |
| Viu o outro jogador | `EntityEnter id=<n>` e `boneco id=<n> criado` | no log do dono |
| Está saudável | `contadores snapshots=<n> acks=<n> correcoes=<n> bonecos=<n> erros_jogo=<n>` a cada 10 s; `erros_jogo=0` | no log do dono |
| Não rouba o controle | `singleton=ok` | no log do dono |
| Viagem rápida (passo 5) | `Input Apparate` e nenhuma expulsão (`desconectado: expulso: …`) | no log de quem viajou |
| O outro saiu (passo 6) | `EntityLeave id=<n>` e `boneco id=<n> destruído` | no log de quem ficou |
| Sem launcher (passo 8) | **nenhum** arquivo novo | pasta `cbmp\logs` |

Linhas extras que podem aparecer e **devem ser anotadas** quando surgirem: `boneco id=<n> desistiu`,
`posicao_recusada`, `bonecos_desligados: <motivo>`, `desconectado: <motivo>` (motivos possíveis: `sem_arquivo`,
`arquivo_invalido`, `protocolo_diferente`, `ticket_expirado`, `sem_resposta`, `recusado: <texto>`, `sem_welcome`,
`expulso: <texto>`, `conexao_perdida`, `servidor_invalido`, `servidor_incompativel`, `erro_interno`).

## Anexo B. Conferências avançadas (opcionais)

Só para quem quiser ir além. Nenhuma delas é necessária para aprovar o 5a. **Use pastas falsas, nunca mexa nas
permissões da pasta real do jogo.**

**B1. Pasta do jogo protegida.** Mostra que o launcher **não muda nada** e diz qual é a pasta.

1. Crie a pasta `C:\cbmp-jogo-falso` com `Phoenix\Binaries\Win64` dentro e um arquivo vazio chamado
   `HogwartsLegacy.exe` (o launcher só confere que eles existem).
2. Num Prompt de Comando: `icacls "C:\cbmp-jogo-falso\Phoenix\Binaries\Win64" /deny "%USERNAME%:(W)"`
3. No launcher, **Configurações** → **Escolher pasta do jogo** → `C:\cbmp-jogo-falso`. Aperte **Jogar**.
4. **Esperado**: o erro `O launcher não tem permissão para gravar na pasta do jogo (C:\cbmp-jogo-falso)...`, citando
   a pasta, **antes** de qualquer download do mod. Nada foi criado dentro de `C:\cbmp-jogo-falso\...\Win64`.
5. **Desfazer**: `icacls "C:\cbmp-jogo-falso\Phoenix\Binaries\Win64" /remove:d "%USERNAME%"`; volte a
   **Pasta do jogo** para a pasta verdadeira; apague `C:\cbmp-jogo-falso`.

**B2. Pasta do jogo acessada por uma junção** (como uma biblioteca da Steam movida de disco).

1. Num Prompt de Comando: `mklink /J C:\cbmp-jogo-juncao "<pasta do jogo>"` (não precisa de administrador).
2. **Escolher pasta do jogo** → `C:\cbmp-jogo-juncao`. Aperte **Jogar**.
3. **Esperado**: tudo funciona como antes; o launcher usa o caminho final da pasta; se o mod já estava registrado
   pelo caminho real, **não** pergunta de novo o conflito nem tira e põe o mod de novo. **Anote** qualquer diferença.
4. **Desfazer**: `rmdir C:\cbmp-jogo-juncao` (o `rmdir` remove só a junção, não o conteúdo) e volte a **Pasta do
   jogo** para a verdadeira.

**B3. Esquecer pasta.** Mostra o botão para uma pasta anterior do jogo que não existe mais.

1. Refaça o passo 1 do B1 (pasta falsa) **sem** o passo 2 (sem a negação do `icacls`), escolha essa pasta e aperte
   **Jogar**: o launcher instala o mod nela (o erro ao abrir o executável vazio é esperado). Depois volte a
   **Pasta do jogo** para a pasta verdadeira, **sem** apertar Jogar.
2. Apague `C:\cbmp-jogo-falso` e reabra **Configurações** → **Mod do jogo**.
3. **Esperado**: aparece `C:\cbmp-jogo-falso` com "Esta pasta não existe mais." e o botão **Esquecer pasta**; a
   confirmação avisa que o launcher não consegue devolver arquivos seus que estivessem guardados lá. Ao confirmar,
   a pasta some da lista.
4. Se não aparecer, **anote** o que a tela mostra (o resultado serve para ajustar o launcher).

## Anexo C. Ligar as opções do `cb-mp.json`

O mod tem quatro opções. Todas são **opcionais**: sem arquivo, valem os padrões. O arquivo **não** vem do servidor e o
launcher nunca o altera.

1. Feche o jogo.
2. Crie o arquivo `Win64\cbmp\cb-mp.json` (**não** dentro de `atual`; se a pasta `cbmp` ainda não existe, crie-a),
   em UTF-8, com **até 4 KiB**. Exemplo com os valores **padrão**:

   ```json
   { "statusNaTela": true, "neutralizacaoExtra": false, "animacaoBoneco": "velocidade", "maxBonecos": 32 }
   ```

3. Mude **uma** chave por vez, abra o jogo pelo launcher e confira no log a linha `config: statusNaTela=…
   neutralizacaoExtra=… animacaoBoneco=… maxBonecos=…`. Arquivo ausente ou inválido volta aos padrões (o log diz
   `cb-mp.json ausente (ou > 4 KiB): padrões`).
4. **Guarde os arquivos** (início da seção 3) **antes de começar este anexo** (se os passos 6 a 8 ainda não foram
   guardados) e **depois de cada abertura**, com o jogo fechado, numa pasta `anexo-c-<chave>`: cada abertura gera
   dois logs e o mod guarda só os 10 mais novos, então os logs dos passos 1 a 7 somem depois de umas 5 aberturas.

| Chave | Valores | Para que serve no teste |
|---|---|---|
| `statusNaTela` | `true` (padrão) ou `false` | `false` desliga o texto no canto da tela (A18); a conexão não é afetada |
| `neutralizacaoExtra` | `false` (padrão) ou `true` | `true` desliga também a câmera, a entrada e a percepção do bruxo extra (A20); use se o bruxo mexer na câmera ou no HUD |
| `animacaoBoneco` | `"velocidade"` (padrão), `"ambulatory"` ou `"off"` | modo de animar o bruxo extra: A16 (`velocidade`), A17 (`ambulatory`); `"off"` não anima |
| `maxBonecos` | número de 0 a 64 (padrão 32) | máximo de bruxos extras ao mesmo tempo; `0` não cria nenhum |

Ao terminar, apague o `cb-mp.json` (ou volte os valores padrão).

## Anexo D. Para quem conduz o teste

- **Antes**: o ambiente de teste online precisa estar com o mod `mod-p1-v0.2.0`, o launcher 0.2.0 e a configuração de
  movimento nova no servidor de teste (`node tools/teste-online.mjs --check` verde; ver
  [`AMBIENTE-TESTE-ONLINE.md`](AMBIENTE-TESTE-ONLINE.md)). **Essa publicação é feita pelo controlador depois da
  revisão final**, e esta rodada só vale depois dela.
- **Servidor**: o servidor de teste aceita que o cliente declare `Apparate` (necessário para o jogador aparecer onde
  está no save e para a viagem rápida); por isso, **qualquer jogador conectado pode se teleportar** nesse servidor.
  Aceito no 5a (dono do ajuste: subprojeto 5e).
- **Durante e depois**: na VPS, `journalctl -u cbmp-server-teste -n 500 --no-pager` não deve ter **`movimento
  inválido`** (é a razão do `Kick` do servidor para o jogador). Anote também os horários de entrada e saída de cada
  jogador.
- **Depois**: preencher a tabela do Anexo A com os logs, registrar cada **FALHOU** em `docs/PENDENCIAS.md` (e o que
  valeu o passo 5a), e medir a **margem do ticket** (A27: o tempo entre apertar Jogar e o `Hello enviado`; pendência
  M-1 em `PENDENCIAS.md`).
