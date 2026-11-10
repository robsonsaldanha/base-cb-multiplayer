# Teste de carga de 512 jogadores

> **Obrigatório antes de produção.** O CI só valida 256 bots em 2 vCPU por 120 s. Antes de abrir um servidor com
> 512 vagas, rode este roteiro em uma máquina Linux de 4 vCPU e arquive o resultado.

## 1. VPS

Alugue um Linux com 4 vCPU e 8 GB de RAM, por exemplo Hetzner CPX31 (ou equivalente), Ubuntu 24.04. Basta alugar por
hora e apagar depois do teste. Servidor e bots rodam na mesma máquina (loopback), então a rede da VPS não é medida.

## 2. Dependências

```bash
sudo apt-get update
sudo apt-get install -y build-essential ninja-build git pkg-config autoconf autoconf-archive automake libtool \
  python3 curl zip unzip tar cmake
```

## 3. Código e vcpkg

```bash
git clone <URL-do-repositório-privado> cb && cd cb
git checkout sub2-server            # ou a branch/tag que será publicada
git clone https://github.com/microsoft/vcpkg ~/vcpkg
git -C ~/vcpkg checkout ec48c11eae9448c8c6e7739d93e06fa2433aa95f   # mesmo commit do CI (VCPKG_COMMIT em .github/workflows/native.yml)
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=~/vcpkg
```

Se o commit do vcpkg no CI mudar, use o novo valor.

## 4. Build

```bash
cmake --preset linux-release
cmake --build --preset linux-release
```

A primeira compilação baixa e compila as dependências (GameNetworkingSockets etc.) e leva vários minutos.
Os binários ficam em `build-native/linux-release/server/cb-server` e `build-native/linux-release/bots/cb-bots`.

## 5. Rodar

```bash
python3 tools/loadtest.py --count 512 --duration 300 --cluster 0.3 --assert
```

- `--count 512` bots; `--duration 300` segundos de carga; `--cluster 0.3` = 30% dos bots aglomerados (pior caso de
  visibilidade/banda); `--area` (padrão 2000) é o lado da área de espalhamento.
- `--assert` sai com código 1 se algum limite for violado (a carga é repetida 1x antes de falhar).
- Outros: `--out-dir` (padrão `build-native/loadtest`), `--server-bin`, `--bots-bin`, `--skip-load`, `--skip-security`,
  `--skip-slow-script`.
- O servidor roda em modo dev (sem Hub), em porta aleatória de 127.0.0.1, com `maxPlayers = count + 16`.
- O painel admin fica ligado na carga (porta aleatória de 127.0.0.1, login por `admin.devToken`): a cada amostra o
  script lê `/api/players`, `/api/resources` e `/api/metrics` pelo painel; qualquer erro é violação. `--no-admin`
  desliga.
  Nada é exposto na internet.

O script faz (1) a carga, lendo `/metrics` a cada 5 s, (2) três cenários de segurança, cada um com servidor próprio
(teleporte, flood de eventos, tickets com seed errada) e (3) o cenário do **script lento**: servidor próprio com 32
bots por 20 s (`--slow-count`, `--slow-duration`) e um resource de teste `slow`, gerado pelo próprio script (não fica
em `resources.example`), que prende a thread de scripts num laço de 2 a 3 s dentro de `cb.events.on('echo')` a cada
10 s. Passa se a simulação não esperar (tick p99 <= 25 ms, tick máximo < 1 s, nenhum bot > 1 s sem snapshot, nenhuma
queda ou kick), se o log tiver o aviso da vigia `[slow] vigia: execução levou ...` sem aborto (2–3 s < 5 s) e se o eco
continuar respondendo. `--skip-slow-script` pula o cenário; o resultado fica em `slowScript` no relatório.

O eco dos bots (evento `echo`, payload JSON `{"t":<µs>}`) é respondido pelo resource Lua de exemplo
`resources.example/echo`, que o script copia para a pasta de resources de cada servidor de teste; o `cb-server` não
tem mais eco em C++. Assim a carga também exercita a thread de scripts e a API `cb.*`.

## 6. Interpretar o resultado

A tabela impressa e `build-native/loadtest/report.json` trazem, na carga:

| Campo | Significado | Limite |
|---|---|---|
| `tickP50Ms` / `tickP99Ms` / `tickMaxMs` | duração do tick do servidor (máximo entre as amostras) | p99 <= 25 ms |
| `avgKbpsPerPlayer` / `maxKbpsPerPlayer` | banda de saída por jogador, só payload das mensagens (sem cabeçalhos IP/UDP/GNS); última amostra com jogadores | média <= 64 kbps |
| `wireAvgKbpsPerPlayer` / `wireMaxKbpsPerPlayer` | estimativa no fio (com cabeçalhos); informativa | - |
| `rssMB` | memória residente máxima do servidor | <= 512 MB |
| `cpuPercentMax` | CPU do servidor (informativo) | - |
| `bots.connected/failedToConnect/disconnected/kicked/rejected` | resultado dos bots | todos conectados, 0 falhas, 0 quedas, 0 expulsos |
| `bots.maxSnapshotGapMs` | maior intervalo sem snapshot em qualquer bot | <= 1000 ms |
| `serverKicks`, `serverCorrections`, `sendFailures` | contadores do servidor | esperado 0 kicks e 0 falhas de envio |
| `violations` | lista de violações (vazia = ok) | vazia |
| `samples` | amostras brutas de `/metrics` ao longo do tempo | - |

Segurança (`security` no relatório), cada cenário com `violations` vazio:

- `teleport`: bot trapaceiro recebe Correction e é expulso em até 15 s;
- `flood`: 500 eventos/s leva a Kick;
- `seed errada`: todos os bots são recusados.

Ao final aparece `RESULTADO: OK` e `ok: true` no relatório. Se falhar, olhe `violations`, `samples` (a degradação
começou com quantos jogadores?) e os logs em `build-native/loadtest/load-N/`.

### Baseline do CI (referência)

256 bots, 120 s, 2 vCPU: tick p99 <= 5,8 ms; banda 49,5 kbps de payload (56,1 kbps no fio); RSS 34,5 MB; 0 quedas
e 0 kicks; maior gap de snapshot 60 ms. Segurança: kick por teleporte em 5,2 s, flood em 0,36 s, seed errada 8/8 recusada.
Com 512 bots espere números maiores, mas abaixo dos limites; resultado muito pior que o dobro do baseline merece investigação.

## 7. Arquivar

```bash
mkdir -p docs/load-results
cp build-native/loadtest/report.json docs/load-results/AAAA-MM-DD-512.json
```

Faça commit do JSON (sem segredos; a seed é efêmera), informando na mensagem o commit testado e o tipo de VPS.
Depois apague a VPS.
