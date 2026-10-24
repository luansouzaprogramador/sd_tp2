#!/usr/bin/env bash
# Executa UM cenario de teste P2P e verifica integridade.
# uso: scripts/run_test.sh <npeers: 2..8> <tam_bloco> <tam_arquivo_bytes> [mesh|line] [rotulo]
#   mesh: todos os peers sao vizinhos entre si | line: A-B-C-D (cadeia)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build"
NPEERS="${1:?npeers}"; BS="${2:?tam_bloco}"; FSIZE="${3:?tam_arquivo}"
TOPO="${4:-mesh}"; LABEL="${5:-custom}"
BASEPORT="${BASEPORT:-7000}"
TIMEOUT="${TIMEOUT:-300}"
NAMES=(A B C D E F G H)

TEST="${LABEL}_p${NPEERS}_b${BS}_f${FSIZE}_${TOPO}"
WORK="$ROOT/work/$TEST"; LOGS="$ROOT/logs/$TEST"
rm -rf "$WORK" "$LOGS"
mkdir -p "$WORK" "$LOGS" "$ROOT/data" "$ROOT/results"

# 1) Preparacao: arquivo aleatorio (reutilizado entre testes) + metadados
DATAFILE="$ROOT/data/file_${FSIZE}.bin"
[ -f "$DATAFILE" ] || head -c "$FSIZE" /dev/urandom > "$DATAFILE"
META="$WORK/file.meta"
"$BIN/mkmeta" "$DATAFILE" "$BS" "$META" > /dev/null || { echo "falha no mkmeta"; exit 1; }
FNAME="$(basename "$DATAFILE")"
ORIG_SHA="$(sha256sum "$DATAFILE" | cut -d' ' -f1)"

PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

# vizinhos conforme topologia
neighbors_of() {
  local i=$1 out=() j
  for ((j = 0; j < NPEERS; j++)); do
    [ "$j" -eq "$i" ] && continue
    if [ "$TOPO" = "line" ]; then
      [ $((j - i)) -eq 1 ] || [ $((i - j)) -eq 1 ] || continue
    fi
    out+=("127.0.0.1:$((BASEPORT + j))")
  done
  (IFS=,; echo "${out[*]}")
}

# 2) Inicio: Seeder (A) primeiro, depois os Leechers
T0=$(python3 -c "import time; print(f'{time.monotonic():.6f}')")
for ((i = 0; i < NPEERS; i++)); do
  n="${NAMES[$i]}"
  mkdir -p "$WORK/$n"
  [ "$i" -eq 0 ] && cp "$DATAFILE" "$WORK/$n/$FNAME"
  "$BIN/peer" --id "$n" --ip 127.0.0.1 --port $((BASEPORT + i)) --meta "$META" --dir "$WORK/$n" \
      --neighbors "$(neighbors_of "$i")" --log "$LOGS/$n.log" --quiet \
      > /dev/null 2> "$LOGS/$n.err" &
  PIDS+=($!)
  [ "$i" -eq 0 ] && sleep 0.5
done

# 3) Monitoramento: espera todos os leechers completarem
STATUS="OK"
while :; do
  done_n=0
  for ((i = 1; i < NPEERS; i++)); do
    grep -q "DOWNLOAD COMPLETO" "$LOGS/${NAMES[$i]}.log" 2>/dev/null && done_n=$((done_n + 1))
  done
  [ "$done_n" -eq $((NPEERS - 1)) ] && break

  now=$(python3 -c "import time; print(f'{time.monotonic():.6f}')")
    if awk "BEGIN{exit !($now - $T0 > $TIMEOUT)}"; then STATUS="TIMEOUT"; break; fi
    sleep 0.2
done
T1=$(python3 -c "import time; print(f'{time.monotonic():.6f}')")
WALL=$(awk "BEGIN{d = $T1 - $T0; printf \"%.3f\", (d < 0 ? 0 : d)}")
sleep 0.5   # deixa os peers fecharem os logs
cleanup; PIDS=()

# 4) Verificacao: integridade (SHA-256), tamanho e origem dos blocos
INTEG="OK"; SIZE="OK"
for ((i = 1; i < NPEERS; i++)); do
  f="$WORK/${NAMES[$i]}/$FNAME"
  if [ ! -f "$f" ]; then INTEG="FALHA"; SIZE="FALHA"; continue; fi
  [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$ORIG_SHA" ] || INTEG="FALHA"
  [ "$(stat -c%s "$f")" -eq "$FSIZE" ] || SIZE="FALHA"
done
MAXT=$(grep -h "DOWNLOAD COMPLETO" "$LOGS"/*.log 2>/dev/null | sed -E 's/.*tempo=([0-9.]+)s.*/\1/' | sort -n | tail -1)
MAXT="${MAXT:-NA}"

echo "=== $TEST ==="
echo "status=$STATUS  tempo_total=${WALL}s  maior_tempo_peer=${MAXT}s  integridade(SHA-256)=$INTEG  tamanho=$SIZE"
grep -h -E "DOWNLOAD COMPLETO|origem" "$LOGS"/*.log | sed 's/^/  /'
for ((i = 0; i < NPEERS; i++)); do
  grep -h "encerrando" "$LOGS/${NAMES[$i]}.log" | sed "s/^/  /"
done

CSV="$ROOT/results/results.csv"
[ -f "$CSV" ] || echo "teste,rotulo,peers,bloco_bytes,arquivo_bytes,topologia,status,tempo_total_s,maior_tempo_peer_s,integridade,tamanho" > "$CSV"
echo "$TEST,$LABEL,$NPEERS,$BS,$FSIZE,$TOPO,$STATUS,$WALL,$MAXT,$INTEG,$SIZE" >> "$CSV"
[ "$STATUS" = "OK" ] && [ "$INTEG" = "OK" ] && [ "$SIZE" = "OK" ]