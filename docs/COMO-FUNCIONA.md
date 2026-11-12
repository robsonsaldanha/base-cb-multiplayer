# Como funciona: vários jogadores no mesmo Hogwarts

Hogwarts Legacy foi feito para um jogador só e não tem nenhum código de rede. Para juntar pessoas no mesmo castelo, a
plataforma CB Multiplayer acrescenta quatro peças. Três rodam fora do jogo; a quarta é uma DLL que roda dentro dele.

- [As quatro peças](#as-quatro-peças)
- [O que acontece quando você clica em "Jogar"](#o-que-acontece-quando-você-clica-em-jogar)
- [Do Jogar até ver o outro jogador (mod 0.2.0)](#do-jogar-até-ver-o-outro-jogador-mod-020)
- [Como a DLL funciona](#como-a-dll-funciona)
- [O que viaja pela rede](#o-que-viaja-pela-rede)
- [Por que os outros jogadores não "teleportam"](#por-que-os-outros-jogadores-não-teleportam)
- [O que é difícil de verdade](#o-que-é-difícil-de-verdade)
- [Glossário](#glossário)

## As quatro peças

```
 PC do jogador                                    Internet
┌──────────────────────────────┐            ┌─────────────────┐
│ Launcher                     │──HTTPS────▶│ Hub             │
│  login, lista, downloads     │◀─ticket────│ contas, lista,  │
│  abre o jogo                 │            │ atualizações    │
│                              │            └─────────────────┘
│ Hogwarts Legacy (original)   │
│  └─ mod (DLL)  ◀─────────────┼──UDP──────▶┌─────────────────┐
│     lê e envia o seu         │  posições  │ Servidor        │
│     personagem, desenha      │  eventos   │ (cb-server)     │
│     os outros                │            │ Lua, MySQL      │
└──────────────────────────────┘            └─────────────────┘
```

| Peça | Onde roda | O que faz | Status |
|---|---|---|---|
| **Launcher** | PC do jogador | Faz o login, mostra a lista de servidores, baixa o que cada servidor precisa e abre o jogo já apontado para o servidor escolhido. | pronto (`launcher/`) |
| **Hogwarts Legacy** | PC do jogador | O jogo de cada jogador, comprado normalmente. Os arquivos do jogo não são editados. | original, sem alteração |
| **Mod (DLL)** | Dentro do jogo | Lê o que o seu personagem faz, manda para o servidor e desenha os outros jogadores no seu mundo. | etapa 5a pronta (conecta e mostra os outros jogadores); falta testar no jogo (`docs/TESTE-NO-JOGO-5A.md`) |
| **Hub** | Internet | A "portaria": contas (login com Discord), lista de servidores, banimentos e distribuição das atualizações do launcher e do mod. | pronto (`hub/`) |
| **Servidor (cb-server)** | Internet | Onde a sessão acontece. Recebe a posição e as ações de cada jogador e repassa para os outros. Roda os scripts do servidor em Lua, guarda dados em MySQL e foi feito para até 512 jogadores. | pronto (`server/`) |

Como as peças conversam:

- **Launcher ⇄ Hub** (HTTPS): login, lista de servidores, ticket de entrada, atualizações.
- **Launcher ⇄ Servidor** (HTTPS): arquivos dos resources, conferidos por hash.
- **DLL ⇄ Servidor** (UDP): posições, animações e eventos, dezenas de vezes por segundo.

## O que acontece quando você clica em "Jogar"

A ordem importa: cada passo depende do anterior.

1. **Launcher → Hub: pede um ticket de entrada.** O Hub confere quem você é e se não está banido, e devolve um ticket
   assinado que vale por poucos minutos e só para aquele servidor.
2. **Launcher → Servidor: busca a lista do que o servidor usa** (o manifesto) e baixa só o que falta. Cada arquivo é
   identificado pelo hash do conteúdo; se o hash não bate, o arquivo é descartado.
3. **Launcher → Hub: confere o mod.** Se houver versão nova da DLL, baixa e verifica a assinatura digital antes de
   instalar na pasta do jogo e ligar o carregador. Mod sem assinatura válida não é usado.
4. **Launcher: deixa um bilhete para a DLL**: um arquivo pequeno com o endereço do servidor e o ticket, que só o seu
   usuário do Windows pode ler e que é apagado em seguida, e um segundo bilhete, sem segredo, que só diz à DLL onde
   achar o primeiro.
5. **Launcher: abre o jogo.** A partir daqui o launcher sai de cena.
6. **DLL → Servidor: a DLL lê o bilhete e conecta.** O servidor valida o ticket com a chave pública do Hub, sem
   precisar perguntar nada ao Hub, e coloca você na sessão.

## Do Jogar até ver o outro jogador (mod 0.2.0)

O mesmo caminho, agora com o mod de verdade, em linguagem simples:

1. **Você clica em Jogar.** O launcher confere se o jogo está fechado, se consegue gravar na pasta dele (ele nunca
   pede permissão de administrador: se não puder, avisa e para) e se já tem o mod. Na primeira vez baixa o pacote do
   mod, confere a assinatura e copia para uma pasta só dele dentro do jogo (`Win64cbmp`). Liga o carregador do mod
   (`dwmapi.dll` e `override.txt`). Se já houver outro carregador de mods ali, **pergunta** antes de guardar os
   arquivos dele numa pasta de backup (`cbmp-backup`); eles voltam quando você remove o mod.
2. **O launcher deixa os dois bilhetes e abre o jogo.** Um tem o endereço e o ticket (válido por poucos minutos); o
   outro só aponta onde está o primeiro. Se o jogo for aberto **sem** o launcher, não há bilhete novo e o mod fica
   parado: o jogo é o de sempre.
3. **O jogo abre duas vezes seguidas.** Na abertura, o Hogwarts Legacy inicia o mod em dois processos, com cerca de 30
   segundos de diferença, e o primeiro some. Como o ticket só vale uma vez, o mod só pega o bilhete quando é o processo
   que vai ficar (ou depois de uma espera de segurança), para não perder a entrada.
4. **Você carrega o save.** Enquanto você está no menu ou numa tela de carregamento o mod não manda nada. Só quando o
   seu bruxo está de verdade no mundo é que ele começa a avisar a posição ao servidor, 20 vezes por segundo.
5. **O servidor responde com os outros.** Para cada jogador por perto o mod cria um **boneco**: um clone do seu
   personagem, com a colisão desligada e feito para os inimigos não o perceberem (a confirmar no jogo), e que o mod move a cada quadro a partir das posições
   recebidas (desenhadas uma fração de segundo no passado, para não parecer que teleportam).
6. **Na tela** aparece uma mensagem curta: "CB Multiplayer: conectado (N por perto)". Se algo falha (entrada
   expirada, servidor fora do ar, expulsão) aparece "CB Multiplayer: desconectado — <motivo>", o jogo segue
   normalmente como single-player e, para voltar, é preciso **fechar o jogo e clicar em Jogar de novo**.

O que ainda **não** existe: os bonecos não têm aparência nem animação próprias (são clones do seu bruxo, e a
animação é experimental), não há chat nem comandos no jogo (etapa 5c), nem feitiços (5f). Tudo isso vem nas próximas
etapas.

## Como a DLL funciona

Uma DLL é uma biblioteca de código que um programa do Windows carrega e executa como se fosse dele. O mod é uma DLL
que passa a rodar *dentro* do processo do jogo, com acesso à mesma memória. É o mesmo princípio que FiveM usa no GTA V
e que outros projetos usam em outros jogos.

O trabalho dela tem quatro partes.

### 1. Entrar no processo do jogo

Quando um programa precisa de uma DLL do sistema, o Windows procura primeiro na pasta do próprio programa. O mod se
aproveita disso: na pasta do executável fica uma DLL com o nome de uma do sistema (por exemplo `version.dll`). O jogo
a carrega achando que é a do Windows.

Ela repassa todas as chamadas para a DLL verdadeira, para o jogo não perceber diferença, e, junto, inicia o código do
multiplayer numa linha de execução própria.

```
HogwartsLegacy.exe  precisa de  version.dll
  acha 1º   Win64\version.dll          (o mod)
  repassa   → System32\version.dll     (a verdadeira)
  e inicia  → multiplayer (thread própria)
```

Nenhum arquivo original do jogo é alterado. Apagar a DLL do mod devolve o jogo ao normal.

> Essa técnica se chama "proxy DLL". No mod 0.2.0 a escolha foi usar o **UE4SS** (uma ferramenta pública de mods
> para jogos da Unreal Engine): o arquivo da pasta do jogo é o `dwmapi.dll` dele, que carrega o UE4SS e, por ele, o mod
> `cb-mp`. Por isso o UE4SS fica carregado **sempre** que o jogo abre, mesmo sem o launcher (o `cb-mp` é que fica
> parado); quem quiser o jogo 100% como antes usa **Remover mod do jogo** nas Configurações do launcher. Um carregador
> próprio, que só liga quando o launcher pede, está nas pendências (`docs/PENDENCIAS.md`).

### 2. Achar o caminho dentro da Unreal Engine

O jogo usa a Unreal Engine 4. Toda Unreal mantém duas tabelas na memória: uma lista com todos os objetos vivos
(`GObjects`) e uma com todos os nomes (`GNames`). Com elas, a DLL encontra o mundo, o seu personagem e as funções da
engine pelo nome, como faria um script dentro do próprio jogo.

Para achar essas tabelas, a DLL procura uma sequência de bytes conhecida dentro do executável, uma "assinatura". Isso
funciona mesmo quando um patch muda os endereços de lugar.

```cpp
// Pseudocódigo ilustrativo.
// Procura no executável um trecho que sempre aparece perto da tabela GObjects.
auto p = scan("48 8B 05 ?? ?? ?? ?? 48 8B 0C C8");   // "??" = byte que muda a cada build
GObjects = resolveRelative(p, 3);

// A partir daí, tudo é por nome:
auto player = findObject("BP_Biped_Player_C");
auto spawn  = findFunction("SpawnActor");
```

### 3. Hooks: entrar no meio do caminho

Um hook é um desvio. A DLL troca os primeiros bytes de uma função do jogo por um salto para uma função dela. Quando o
jogo chama, por exemplo, a atualização de cada quadro, o código do mod roda primeiro e depois chama a função original,
que continua funcionando normalmente.

```
sem hook:  jogo → Tick()
com hook:  jogo → mod: lê e envia → Tick() original
```

Com hooks em poucos pontos, como o quadro, o input e a animação, a DLL sabe tudo o que o seu personagem faz sem mexer
na lógica do jogo. O trecho trocado é guardado; ao desligar o mod, os bytes originais voltam.

### 4. O ciclo de cada quadro

Com os hooks no lugar, a DLL repete o mesmo ciclo enquanto você joga:

```
// Pseudocódigo: roda a cada quadro, dentro do jogo
onTick(dt):
  eu = lerPersonagemLocal()       // posição, rotação, animação
  se passou 1/20 s:
    enviar(servidor, eu)          // ~20 vezes por segundo

  para cada pacote recebido:
    guardar(pacote)               // posições dos outros

  para cada jogador remoto:
    se não existe no mundo:
      criarBoneco(jogador)        // SpawnActor
    moverSuave(boneco, histórico) // ver "Por que os outros não teleportam"
```

## O que viaja pela rede

O jogo não manda o "vídeo" do seu personagem. Manda um pacote pequeno, com números que descrevem o estado dele. Cada
PC recria a cena a partir desses números.

Exemplo simplificado (o formato real tem mais campos, mas a ordem de grandeza é esta):

| Campo | O que é | Bytes |
|---|---|---:|
| `jogador` | Número do jogador na sessão | 2 |
| `sequência` | Contador, para descartar pacote velho que chegou atrasado | 2 |
| `posição x, y, z` | Em centímetros, a unidade da Unreal (1 unidade = 1 cm) | 12 |
| `rotação` | Para onde o personagem olha, comprimida em 0–65535 | 2 |
| `velocidade` | Ajuda a prever o próximo passo | 6 |
| `animação` | Andando, correndo, lançando feitiço… | 2 |
| `flags` | No chão, varinha na mão, agachado | 1 |
| **Total por pacote** | | **27** |

A 20 pacotes por segundo, um jogador gera uns **540 bytes/s**. Com 50 pessoas por perto, você recebe uns **27 KB/s**,
bem menos que um vídeo. O servidor só repassa a cada jogador o que acontece perto dele, e é isso que torna possível
ter centenas de pessoas no mesmo servidor.

Movimento vai num canal "rápido e descartável": se um pacote se perde, o próximo já o substitui. Chat, comandos e
eventos dos scripts vão num canal confiável, que reenvia até chegar.

## Por que os outros jogadores não "teleportam"

O seu jogo desenha 60 quadros por segundo, mas só recebe a posição dos outros umas 20 vezes por segundo, e com atraso.
Se a DLL apenas pulasse para cada posição nova, os outros andariam aos trancos.

Por isso ela desenha os outros jogadores uma fração de segundo "no passado" e calcula as posições entre dois pacotes
(interpolação):

```
pacotes que chegaram:   A ───────── B ───────── C
tempo desenhado:              ▲
                     (agora − atraso): fica entre A e B,
                     posição = A + (B − A) × fração
```

O preço da suavização é um pequeno atraso extra, em geral uns 100 ms. Para andar, conversar e encenar, ninguém
percebe. Para combate, o servidor precisa compensar esse atraso ao decidir quem acertou quem.

## O que é difícil de verdade

| Problema | Por quê | Caminho |
|---|---|---|
| **Um mundo feito para uma pessoa** | Cada jogo roda a própria simulação: NPCs, missões, horário e clima. Dois jogadores lado a lado podem estar em momentos diferentes da história. | Desligar o que atrapalha (missões, inimigos da história) e deixar o servidor ditar horário e clima para todos. |
| **Atualizações do jogo** | Cada patch pode mudar onde as coisas ficam na memória e quebrar o mod. | Assinaturas de bytes em vez de endereços fixos, e o mod como pacote separado, assinado e versionado, que dá para atualizar sozinho no mesmo dia. |
| **Animações e feitiços** | Fazer o feitiço de outro jogador aparecer certo no seu jogo, com efeito, som e alvo, é muito mais trabalhoso que sincronizar posição. | Começar por andar, correr e gestos; feitiços vêm em etapas. |
| **Confiança** | O jogo roda no PC do jogador, e quem quiser trapacear pode alterar a memória dele. | O servidor decide o que importa (dinheiro, inventário, dano, teleporte) e confere o resto, como velocidade impossível. |

Cada jogador precisa ter a própria cópia original do jogo. O projeto é escrito do zero: não usa código de FiveM,
MafiaHub ou de outros mods de multiplayer.

## Glossário

- **DLL**: biblioteca de código carregada por um programa do Windows.
- **Hook**: desvio colocado numa função para rodar código antes ou depois dela.
- **Ticket**: autorização temporária, assinada pelo Hub, para entrar num servidor específico.
- **Manifesto**: lista dos arquivos que um servidor usa, cada um com o seu hash.
- **Hash**: "impressão digital" de um arquivo; qualquer alteração muda o hash.
- **Interpolação**: calcular posições intermediárias entre dois pacotes recebidos.

Uma versão interativa desta explicação, com uma demonstração da suavização de movimento, está em
<https://claude.ai/artifact/StNqCw8u9xoYMnX3Z99FNQ> (abre só para quem tiver acesso compartilhado).
