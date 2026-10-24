# TP2 – Transferência de Arquivos Peer-to-Peer (C / Linux)

Sistema P2P elementar inspirado no BitTorrent: cada processo atua simultaneamente como **Servidor** e **Cliente**. O arquivo é fragmentado em blocos de tamanho fixo, e os peers trocam blocos entre si (estratégia *rarest-first*). Assim que um bloco é baixado, validado e gravado, ele já pode ser servido a outros peers (Seeder em cascata).

## Requisitos

Para compilar e executar o núcleo do sistema (C):
*   `gcc`, `make`, `bash`, `sha256sum` (coreutils)
*   *Nenhuma biblioteca externa é necessária para o C (o SHA-256 é implementado do zero).*

Para gerar os gráficos do relatório:
*   `python3`, `pip`
*   Bibliotecas listadas no arquivo `requirements.txt` (`pandas`, `matplotlib`, `seaborn`). Para instalá-las, execute:
    ```bash
    python3 -m venv venv
    source venv/bin/activate
    pip install -r requirements.txt
    ```

## Estrutura do Projeto

```text
p2p-transfer/
├── Makefile
├── README.md
├── requirements.txt
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
│   ├── run_test.sh      # Executa um cenário específico
│   ├── run_all.sh       # Executa a matriz completa (Tabela 1)
│   ├── run_benchmark.sh # Executa a matriz 5 vezes para média estatística
│   └── clean.sh         # Limpa pastas temporárias
├── tools/         # Ferramentas de análise (não fazem parte do executável)
│   ├── plot_results.py    # Gera gráficos a partir das médias
│   └── average_results.py # Calcula média e desvio padrão das rodadas
├── data/          # Arquivos binários de teste gerados
├── work/          # Diretório de cada peer durante os testes (gerado)
├── logs/          # Logs detalhados de cada peer (gerado)
└── results/       # results.csv, médias e gráficos .png (gerado)
```

## Como Compilar

```bash
make clean
make
```
Isso gerará os binários `build/peer` e `build/mkmeta`.

## Execução dos Testes

O projeto oferece duas formas de execução, dependendo do objetivo da análise.

### Opção 1: Execução Rápida (Matriz Completa - 1 Rodada)
Ideal para validar o funcionamento do código e visualizar os resultados brutos rapidamente.

```bash
# 1. Compilar e dar permissão aos scripts
make clean && make
chmod +x scripts/*.sh

# 2. Rodar a matriz completa da Tabela 1
bash scripts/run_all.sh

# 3. Visualizar a tabela de resultados no terminal
column -s, -t results/results.csv
```

*(Nota: Nesta opção, o script `plot_results.py` não deve ser executado, pois ele depende do arquivo de médias gerado na Opção 2).*

### Opção 2: Benchmark Estatístico (5 Rodadas + Gráficos)
Recomendado para gerar os dados finais do relatório. Como o ambiente WSL sofre com *jitter* (variações de escalonamento de CPU e I/O), rodar a matriz 5 vezes permite calcular a **média** e o **desvio padrão**, gerando gráficos confiáveis com barras de erro.

```bash
# 1. Compilar e dar permissão aos scripts
make clean && make
chmod +x scripts/*.sh
chmod +x scripts/run_benchmark.sh

# 2. Ativar o ambiente virtual e instalar dependências (se ainda não fez)
source venv/bin/activate
pip install -r requirements.txt

# 3. Rodar o benchmark (5 vezes)
bash scripts/run_benchmark.sh 5

# 4. Calcular as médias das 5 rodadas
python tools/average_results.py

# 5. Gerar os gráficos finais com barras de erro
python tools/plot_results.py
```

As imagens `.png` serão salvas automaticamente na pasta `results/`.

**Topologias de teste utilizadas nos scripts:**
*   `mesh`: Todos os peers são vizinhos entre si (malha completa, P2P real).
*   `line`: Peers conectados em cadeia A-B-C-D (gera gargalo sequencial, usado para comprovar a eficiência da malha).

## Detalhes do Protocolo TCP

Toda mensagem possui o formato: `[tipo: 1 byte][tamanho: 4 bytes Big-Endian][payload]`.
Mensagens implementadas:
*   `HELLO`: Identifica o peer.
*   `GET_BITFIELD` / `BITFIELD`: Informa quais blocos o peer possui (representação em array de bytes).
*   `GET_BLOCK` / `BLOCK`: Solicita e envia o conteúdo de um bloco específico.
*   `NO_BLOCK`: Informa que o peer não possui o bloco solicitado.
*   `BYE`: Encerra a conexão.

O arquivo `.meta` (similar a um `.torrent`) possui formato texto: nome do arquivo, tamanho, tamanho do bloco, número de blocos, SHA-256 do arquivo inteiro e uma lista com o SHA-256 de cada bloco.

## Decisões de Projeto e Arquitetura

*   **Simetria Real**: O processo principal cria uma thread de `accept` (servidor) e uma thread para cada vizinho configurado (cliente).
*   **Sem Tracker**: A lista de vizinhos é estática (`--neighbors`), eliminando a necessidade de um servidor central.
*   **Concorrência e I/O**: Utilização de `pread`/`pwrite` atômico no arquivo `.part` para evitar condições de corrida durante a escrita e leitura simultânea de blocos.
*   **Gerenciamento de Blocos**: Vetores `have[]` e `inflight[]` protegidos por mutexes (thread-safe).
*   **Seleção Rarest-First**: O algoritmo `pick_block` escolhe o bloco que possui a menor frequência entre os vizinhos vivos. Isso evita que blocos raros "desapareçam" se um único peer que os possuía desconectar.
*   **Integridade (Hash)**: O SHA-256 é verificado *por bloco* (blocos corrompidos são descartados e re-solicitados) e *no arquivo final* (antes do `.part` ser renomeado para o nome definitivo).
*   **Seeding em Cascata**: Requisito central atendido; assim que um bloco é gravado e validado, a thread de servidor já está pronta para fornecê-lo a outros peers.
