#!/usr/bin/env bash
# Roda a matriz de testes N vezes e salva os resultados individuais.
N=${1:-5}
echo "=== Iniciando Benchmark: $N rodadas ==="

for ((i=1; i<=N; i++)); do
    echo -e "\n>>> RODADA $i de $N <<<"
    bash scripts/run_all.sh
    mv results/results.csv results/results_run_${i}.csv
    [ -f results/results.png ] && mv results/results.png results/results_run_${i}.png
    [ -f results/results_column.png ] && mv results/results_column.png results/results_run_${i}_column.png
    rm -f results/results_column.txt
    echo "Resultados da rodada $i salvos em results/results_run_${i}.csv"
done

echo "=== Benchmark concluído! ==="