# Kit de reconhecimento do jogo (PC de casa)

Roteiro para rodar **uma vez**, no PC que tem o **Hogwarts Legacy** (Steam ou Epic), o kit
**`cbmp-kit-reconhecimento.zip`**. Ele faz duas coisas na mesma visita:

1. **Reconhecimento**: um mod (`cb-recon`) espera você entrar no mundo com um save e, sozinho (sem apertar nada),
   grava um relatório sobre o personagem (classes, componentes, malhas, animação, movimento, mundo) e roda os
   "dumpers" do UE4SS, que listam todas as classes e objetos carregados no jogo. Esses dados são a referência para
   escrever a DLL de verdade do cliente (subprojeto 5): sem eles a gente adivinha nomes de classes e propriedades.
2. **Teste do spike** (pendente desde o subprojeto 1): o mod `cb-mp` registra a posição do bruxo e, com **F8**, cria
   um clone parado na sua frente; F8 de novo remove.

> Este arquivo é o `LEIA-ME.md` que vem dentro do kit. Na máquina de desenvolvimento o kit é montado com
> `node tools/make-recon-kit.mjs` (fonte em `recon-kit/`; teste sem o jogo: `node tools/recon-kit-smoke.mjs`).

Tempo: uns 10 minutos para preparar, **20 a 40 minutos com o jogo aberto** (a maior parte é esperar os dumpers) e
uns 10 para coletar e desinstalar.

**Como ler cada passo**: o que fazer, o que deve acontecer (**Esperado**) e o que anotar se der errado (**Se
falhar**). No fim há uma **tabela de resultados** para preencher.

---

## 0. É seguro? O que o kit mexe

- **Nenhum arquivo original do jogo é alterado.** O kit só **acrescenta** dois itens na pasta
  `Phoenix\Binaries\Win64` do jogo: o arquivo `dwmapi.dll` (carrega o UE4SS) e a pasta `ue4ss\` (UE4SS + os dois
  mods). Se já existir um `dwmapi.dll` ou uma pasta `ue4ss` ali (de outro mod), eles são **movidos** para
  `Win64\cbmp-recon-backup\` e voltam exatamente iguais na desinstalação (o kit confere o hash de cada arquivo).
- O **DESINSTALAR.bat** apaga só o que o kit pôs e devolve os originais. Depois disso a pasta fica como antes.
- O UE4SS é um projeto aberto (licença MIT) muito usado para mods de jogos Unreal; as licenças vão na pasta
  `licencas\` do kit.
- O jogo pode **travar ou fechar** enquanto os dumpers rodam (é o tipo de coisa que este teste existe para
  descobrir). Isso não estraga a instalação: abra de novo e siga o roteiro; o mod pula a etapa que travou.
- **Faça backup dos saves antes** (passo 1.2). O kit não mexe em saves, mas se o jogo fechar no meio de um
  salvamento automático o save em uso pode se perder.
- Jogue **offline do modo online da WB** se o jogo pedir login: nada aqui precisa de conta.

### Os dados coletados são PRIVADOS

O zip gerado pelo COLETAR tem dados extraídos do jogo (nomes internos, estruturas, logs com caminhos do seu PC).
**Nunca** coloque esse zip (nem nada de dentro dele) no repositório, numa issue, PR, Discord público ou qualquer
lugar público. O repositório é **público**. O COLETAR grava o zip em `OneDrive\CasteloBruxoRP\cbmp-dumps\` (fora
do repositório), que só sincroniza entre os seus PCs. O `.gitignore` do projeto bloqueia esses nomes por garantia.

---

## 1. Antes de começar

### 1.1 Precisa de

- Windows 11, o Hogwarts Legacy instalado (Steam ou Epic) e **atualizado** (sem atualização pendente na loja).
- **10 GB livres** no disco do jogo e no disco C: (os dumps ocupam alguns GB antes de compactar).
- Nada para instalar: sem Node, Git, Visual Studio. O kit usa só o PowerShell que já vem no Windows.

### 1.2 Backup dos saves (recomendado)

1. Feche o jogo.
2. Cole na barra de endereço do Explorador de Arquivos e aperte Enter:
   `%LOCALAPPDATA%\Hogwarts Legacy\Saved\SaveGames`
3. Copie a pasta inteira que aparecer ali dentro (um número comprido) para um lugar seguro, por exemplo
   `Documentos\backup-saves-hogwarts`.

**Esperado**: a cópia tem arquivos `HL-00-00.sav`, `HL-01-00.sav` etc.

---

## 2. Pegar e extrair o kit

O kit é **`cbmp-kit-reconhecimento.zip`** (uns 9 MB), com um `cbmp-kit-reconhecimento.zip.sha256` ao lado. Na
pasta do OneDrive ele fica em `OneDrive\CasteloBruxoRP\Launcher_livre\dist\`.

1. **OneDrive com "arquivos sob demanda"**: se o ícone do zip for uma **nuvem**, clique com o botão direito →
   **Sempre manter neste dispositivo** e espere virar um **check verde**.
2. **Copie** o zip para fora do OneDrive, para `C:\cbmp-kit\` (crie a pasta).
3. (Recomendado) Confira se chegou inteiro. No PowerShell:

   ```
   Get-FileHash C:\cbmp-kit\cbmp-kit-reconhecimento.zip
   ```

   **Esperado**: o `Hash` é igual ao texto do arquivo `.sha256` (maiúsculas/minúsculas não importam).
4. Se o zip veio por download do navegador: botão direito → **Propriedades** → marque **Desbloquear** → OK.
5. Botão direito no zip → **Extrair Tudo…**. O Windows sugere `C:\cbmp-kit\cbmp-kit-reconhecimento`. **Apague o
   final `\cbmp-kit-reconhecimento`** para o campo ficar **exatamente `C:\cbmp-kit`** (o zip já traz essa pasta
   dentro; se deixar como o Windows sugere, ela fica repetida). Clique **Extrair**.

**Esperado**: existe `C:\cbmp-kit\cbmp-kit-reconhecimento\` e, direto dentro dela, `LEIA-ME.md`, `INSTALAR.bat`,
`COLETAR.bat`, `DESINSTALAR.bat`, `kit.json` e as pastas `ferramentas`, `payload`, `licencas`. No PowerShell:

```
Test-Path C:\cbmp-kit\cbmp-kit-reconhecimento\INSTALAR.bat
```

**Esperado**: `True`.

**Se falhar**: `False` e existe `C:\cbmp-kit\cbmp-kit-reconhecimento\cbmp-kit-reconhecimento` (pasta repetida) →
apague `C:\cbmp-kit\cbmp-kit-reconhecimento` inteira e refaça o item 5. Use caminho curto, sem acentos, fora do
OneDrive (não extraia dentro da pasta do jogo: este kit, ao contrário do pacote do spike, acha o jogo sozinho).

---

## 3. Instalar

1. Feche o jogo (e o launcher da Epic/Steam pode ficar aberto).
2. Dê dois cliques em **`INSTALAR.bat`**. Uma janela azul/preta abre.
3. Ela mostra `Jogo encontrado (Steam): ...` ou `(Epic): ...` e pergunta **"É esta a pasta do jogo? [S/n]"** →
   confira o caminho e aperte **Enter** (ou `S`).
   - Se não achar o jogo, ela pede o caminho: na Steam, biblioteca → botão direito no jogo → **Gerenciar** →
     **Procurar arquivos locais**, copie o endereço da janela que abrir e cole; na Epic, biblioteca → **…** no jogo
     → **Gerenciar** → ícone de pasta ao lado de "Instalação".
4. Se o jogo está em `C:\Program Files` (comum na Epic), ela avisa que precisa de administrador e pergunta se pode
   abrir de novo como administrador → **Enter**, e no aviso do Windows clique **Sim**. O resto acontece numa
   segunda janela.

**Esperado**: `Instalado em: ...\Phoenix\Binaries\Win64` em verde. Aperte Enter para fechar.

**Se falhar** (texto vermelho `ERRO: ...`): anote a mensagem. Casos comuns:
- `o jogo está aberto` → feche o jogo (confira no Gerenciador de Tarefas que não há `HogwartsLegacy.exe`).
- `o pacote do spike (cb-mp-spike) está instalado` → rode o `DESINSTALAR.bat` que ficou em
  `Phoenix\Binaries\Win64` do jogo (do teste antigo) e depois este INSTALAR de novo.
- `encontrei 'xinput1_3.dll'/'UE4SS.dll'` → há outro UE4SS/mod instalado; anote qual e pare aqui.
- `o kit está incompleto ou corrompido` → extraia o zip de novo (passo 2).

---

## 4. Jogar: reconhecimento + teste do F8

1. Abra o jogo **normalmente pela Steam ou pela Epic**.
2. Junto com o jogo abre uma **janela preta de console do UE4SS**. É esperado: **não feche** essa janela. Deixe-a
   visível ao lado (ou dê Alt+Tab para olhar) — é nela que o progresso aparece, em linhas começando com
   `[cb-recon]`.
3. No menu do jogo, **carregue um save logo** (Continuar/Carregar). Não fique muito tempo no menu.
4. Já no mundo, pare o personagem num lugar calmo (sem inimigos, fora de menus e cutscenes) e **solte o
   controle**. Na janela preta deve aparecer `[cb-recon] INFO: candidato: ...` e, uns 45 segundos depois,
   `jogador encontrado` e `relatório gravado`.
5. Em seguida começam as etapas pesadas (`etapa 1/6: objetos ...` até `etapa 6/6`). Durante cada uma o **jogo
   congela** e o Windows pode mostrar **"Não está respondendo"** — **não feche**: espere. Cada etapa leva de
   segundos a alguns minutos. Não mexa no jogo até o fim.
6. Quando aparecer **`TUDO PRONTO`** na janela preta, o reconhecimento acabou (no total, uns 5 a 20 minutos).
7. **Teste do spike** (agora sim pode jogar):
   1. Ande com o personagem uns **10 segundos**.
   2. Aperte **F8**: deve aparecer um **clone parado** do bruxo uns 2 m à frente. Espere 5 segundos (não use
      viagem rápida nem troque de área entre os dois F8).
   3. Aperte **F8** de novo: o clone some.
8. Saia do jogo pelo menu (Sair para a área de trabalho). Se aparecer uma caixa do UE4SS dizendo "Crashdump
   written", clique OK.

**Esperado**: `TUDO PRONTO` na janela preta; F8 cria e remove o clone; o jogo não fecha sozinho.

**Se falhar**:
- **Nenhuma janela preta** ao abrir o jogo → o UE4SS não carregou. Anote e pule para o passo 5 (COLETAR) mesmo
  assim.
- **Fica só em `aguardando (... s): ...`** por mais de 5 minutos já dentro do mundo → anote a última linha
  `aguardando` (mostra controller/pawn/mundo). Saia do jogo e siga para o COLETAR.
- **O jogo fechou ou travou de vez numa etapa** (ex.: `etapa 3/6: sdk_cxx começando` e nada mais por 15 minutos)
  → feche o jogo pelo Gerenciador de Tarefas se precisar, **abra de novo**, carregue o save e espere: o mod pula a
  etapa que travou e continua das outras (aparece `começou numa sessão anterior e não terminou`). Anote qual etapa
  foi.
- **F8 não faz nada / o jogo fecha no F8** → anote. Isso é resultado do spike, não precisa repetir.

---

## 5. Coletar os resultados

1. Com o jogo **fechado**, dê dois cliques em **`COLETAR.bat`**.
2. Espere a compactação (pode levar alguns minutos: os dumps são grandes).

**Esperado** (em verde):

```
Coleta pronta:
  C:\Users\<você>\OneDrive\CasteloBruxoRP\cbmp-dumps\cbmp-recon-AAAAMMDD-HHMMSS.zip
  tamanho: ... MB, ... itens
```

Se a pasta `OneDrive\CasteloBruxoRP` não existir nesse PC, o zip vai para a **Área de Trabalho**. Anote o caminho e
o tamanho.

**Se falhar**: anote a mensagem. `ATENÇÃO: nenhum relatorio-*.json` ou `sem UE4SS_ObjectDump.txt` não são erros do
COLETAR: só dizem que aquela parte não rodou no jogo (anote na tabela).

---

## 6. Desinstalar

1. Com o jogo fechado, dê dois cliques em **`DESINSTALAR.bat`** (se o jogo está em `C:\Program Files`, ele pede
   administrador como no INSTALAR).
2. Se você esqueceu o COLETAR, ele roda o COLETAR sozinho antes de apagar qualquer coisa.

**Esperado**: `Desinstalado. A pasta ... voltou a ser como antes da instalação.`

3. **Confira os arquivos do jogo pela loja** (garante que tudo está original):
   - **Steam**: biblioteca → botão direito no jogo → **Propriedades** → **Arquivos instalados** → **Verificar
     integridade dos arquivos do jogo**.
   - **Epic**: biblioteca → **…** no jogo → **Gerenciar** → **Verificar**.

   **Esperado**: a loja termina sem baixar nada (ou só alguns MB). Ela não olha arquivos extras; por isso o
   DESINSTALAR confere a pasta antes.
4. Abra o jogo uma vez e veja se o save carrega normal (sem janela preta agora).

**Se falhar**: `não consegui remover ... (arquivo em uso?)` → feche o jogo e rode de novo. Os originais (se havia)
continuam em `Phoenix\Binaries\Win64\cbmp-recon-backup\` até a desinstalação terminar.

Depois de desinstalar pode apagar `C:\cbmp-kit\cbmp-kit-reconhecimento` e o zip do kit.

---

## 7. Tabela de resultados

Copie, preencha e mande junto (texto simples basta):

| # | Item | Resultado (OK / FALHOU / anotação) |
|---|------|-----------------------------------|
| 1 | Loja do jogo (Steam/Epic) e pasta do jogo | |
| 2 | INSTALAR achou o jogo sozinho? Pediu administrador? | |
| 3 | Janela preta do UE4SS abriu com o jogo? | |
| 4 | `jogador encontrado` + `relatório gravado` apareceram? Quanto tempo depois de carregar o save? | |
| 5 | Etapas 1 a 6: quais terminaram, quais deram ERRO, alguma travou o jogo? | |
| 6 | Apareceu `TUDO PRONTO`? Tempo total aproximado | |
| 7 | F8: clone apareceu? Como estava (aparência, pose)? | |
| 8 | F8 de novo: clone sumiu? | |
| 9 | O jogo fechou sozinho ou travou em algum momento? Quando? | |
| 10 | COLETAR: caminho e tamanho do zip | |
| 11 | DESINSTALAR terminou com "voltou a ser como antes"? | |
| 12 | Verificação da Steam/Epic: baixou algo? | |
| 13 | Save carrega normal depois? | |

## 8. O que mandar de volta

- A **tabela** acima (pode colar no chat).
- O **caminho do zip** do COLETAR. Se ele ficou em `OneDrive\CasteloBruxoRP\cbmp-dumps\`, não precisa mandar o
  arquivo: o OneDrive leva para o PC de desenvolvimento. Se ficou na Área de Trabalho, mova para essa pasta do
  OneDrive (crie `cbmp-dumps` dentro de `OneDrive\CasteloBruxoRP` se precisar) ou para um pendrive.
- **Nunca** dentro de `Launcher_livre` (o repositório) nem em lugar público.
