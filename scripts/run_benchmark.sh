#!/usr/bin/env bash
# Roda a matriz de testes N vezes e salva os resultados individuais.
N=${1:-5}
echo "=== Iniciando Benchmark: $N rodadas ==="

for ((i=1; i<=N; i++)); do
    echo -e "\n>>> RODADA $i de $N <<<"
    bash scripts/run_all.sh
    mv results/results.csv results/results_run_${i}.csv
    echo "Resultados da rodada $i salvos em results/results_run_${i}.csv"
done

echo "=== Benchmark concluído! ==="