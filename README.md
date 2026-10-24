# TP2 – Transferência de Arquivos Peer-to-Peer (C / Linux)

Sistema P2P elementar inspirado no BitTorrent: cada processo atua simultaneamente como **Servidor** e **Cliente**. O arquivo é fragmentado em blocos de tamanho fixo, e os peers trocam blocos entre si (estratégia *rarest-first*). Assim que um bloco é baixado, validado e gravado, ele já pode ser servido a outros peers (Seeder em cascata).

## Requisitos

Para compilar e executar o núcleo do sistema (C):
*   `gcc`, `make`, `bash`, `sha256sum` (coreutils)
*   *Nenhuma biblioteca externa é necessária para o C (o SHA-256 é implementado do zero).*

Para gerar os gráficos do relatório (Opcional):
*   `python3`, `pip`
*   Bibliotecas: `pandas`, `matplotlib`, `seaborn` (`pip install pandas matplotlib seaborn`)

## Estrutura do Projeto

```text
p2p-transfer/
├── Makefile
├── README.md
├── include/       # Cabeçalhos (.h)
│   ├── sha256.h
│   ├── meta.h
│   ├── protocol.h
│   └── util.h
├── src/           # Código-fonte em C
│   ├── peer.c     # Lógica principal do P2P (threads, sockets, rarest-first)
│   ├── mkmeta.c   # Gerador de metadados (.meta)
│   ├── sha256.c   # Implementação própria do SHA-256
│   ├── meta.c     # Leitura/escrita de metadados
│   ├── protocol.c # Empacotamento de mensagens TCP
│   └── util.c     # Funções auxiliares (tempo, hex)
├── scripts/       # Automação de testes
│   ├── run_test.sh  # Executa um cenário específico
│   ├── run_all.sh   # Executa a matriz completa (Tabela 1)
│   └── clean.sh     # Limpa pastas temporárias
├── tools/         # Ferramentas de análise (não fazem parte do executável)
│   └── plot_results.py # Gera os gráficos a partir do results.csv
├── data/          # Arquivos binários de teste gerados
├── work/          # Diretório de cada peer durante os testes (gerado)
├── logs/          # Logs detalhados de cada peer (gerado)
└── results/       # results.csv e gráficos .png (gerado)
```

## Como Compilar

```bash
make clean
make
```
Isso gerará os binários `build/peer` e `build/mkmeta`.

## Uso Manual (Exemplo com 3 terminais)

**1. Preparar arquivo e metadados:**
```bash
head -c 1048576 /dev/urandom > data/arquivo.bin
build/mkmeta data/arquivo.bin 1024 data/arquivo.meta
mkdir -p work/A work/B work/C
cp data/arquivo.bin work/A/  # A é o Seeder inicial
```

**2. Iniciar os Peers:**
```bash
# Terminal 1 (Seeder A)
build/peer --id A --port 7000 --meta data/arquivo.meta --dir work/A --neighbors 127.0.0.1:7001,127.0.0.1:7002

# Terminal 2 (Leecher B)
build/peer --id B --port 7001 --meta data/arquivo.meta --dir work/B --neighbors 127.0.0.1:7000,127.0.0.1:7002

# Terminal 3 (Leecher C)
build/peer --id C --port 7002 --meta data/arquivo.meta --dir work/C --neighbors 127.0.0.1:7000,127.0.0.1:7001
```

**Opções do binário `peer`:**
*   `--ip`: Endereço de escuta (padrão: 127.0.0.1).
*   `--log ARQ`: Grava o log num arquivo específico.
*   `--linger SEG`: Segundos que o peer continua servindo após completar o download (padrão: infinito).
*   `--quiet`: Oculta mensagens de recebimento/envio de blocos no terminal.

O arquivo final aparece em `work/<peer>/arquivo.bin`. Durante o download, os dados ficam em `arquivo.bin.part`.

## Testes Automatizados (Scripts)

O script `run_test.sh` automatiza todo o processo: gera arquivo, cria metadados, sobe os peers, espera terminarem, valida a integridade (SHA-256) e extrai métricas.

```bash
# Teste único: <peers> <tam_bloco> <tam_arquivo> [mesh|line] [rotulo]
bash scripts/run_test.sh 4 1024 1048576 mesh FileB

# Matriz completa da Tabela 1 (demora alguns minutos)
bash scripts/run_all.sh
```

**Topologias de teste:**
*   `mesh`: Todos os peers são vizinhos entre si (malha completa, P2P real).
*   `line`: Peers conectados em cadeia A-B-C-D (gera gargalo sequencial).

## Gerando Gráficos para o Relatório

Após rodar os testes, utilize o script Python para transformar o `results/results.csv` em gráficos prontos para o PDF:

```bash
python tools/plot_results.py
```
As imagens `.png` serão salvas automaticamente na pasta `results/`.

## Detalhes do Protocolo TCP

Toda mensagem possui o formato: `[tipo: 1 byte][tamanho: 4 bytes Big-Endian][payload]`.
Mensagens implementadas:
*   `HELLO`: Identifica o peer.
*   `GET_BITFIELD` / `BITFIELD`: Informa quais blocos o peer possui (representação em array de bytes).
*   `GET_BLOCK` / `BLOCK`: Solicita e envia o conteúdo de um bloco específico.
*   `NO_BLOCK`: Informa que o peer não possui o bloco solicitado.
*   `BYE`: Encerra a conexão.

O arquivo `.meta` (similar a um `.torrent`) possui formato texto: nome do arquivo, tamanho, tamanho do bloco, número de blocos, SHA-256 do arquivo inteiro e uma lista com o SHA-256 de cada bloco.

## Decisões de Projeto e Arquitetura (Para o Relatório)

*   **Simetria Real**: O processo principal cria uma thread de `accept` (servidor) e uma thread para cada vizinho configurado (cliente).
*   **Sem Tracker**: A lista de vizinhos é estática (`--neighbors`), eliminando a necessidade de um servidor central.
*   **Concorrência e I/O**: Utilização de `pread`/`pwrite` atômico no arquivo `.part` para evitar condições de corrida durante a escrita e leitura simultânea de blocos.
*   **Gerenciamento de Blocos**: Vetores `have[]` e `inflight[]` protegidos por mutexes (thread-safe).
*   **Seleção Rarest-First**: O algoritmo `pick_block` escolhe o bloco que possui a menor frequência entre os vizinhos vivos. Isso evita que blocos raros "desapareçam" se um único peer que os possuía desconectar.
*   **Integridade (Hash)**: O SHA-256 é verificado *por bloco* (blocos corrompidos são descartados e re-solicitados) e *no arquivo final* (antes do `.part` ser renomeado para o nome definitivo).
*   **Seeding em Cascata**: Requisito central atendido; assim que um bloco é gravado e validado, a thread de servidor já está pronta para fornecê-lo a outros peers.