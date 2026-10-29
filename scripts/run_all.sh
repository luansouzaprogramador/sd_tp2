#!/usr/bin/env bash
# Executa toda a matriz de testes da Tabela 1 do enunciado.
#   peers {2,4} x bloco {1024,4096} x arquivos {A:10K,20K | B:1M,5M | C:10M,20M}
# Para 4 peers roda tambem a topologia em cadeia (line).
# QUICK=1 pula os arquivos grandes (C).
cd "$(dirname "$0")/.." || exit 1
make -s || exit 1
rm -f results/results.csv
FILES=("FileA:10240" "FileA:20480" "FileB:1048576" "FileB:5242880")
[ "${QUICK:-0}" = "1" ] || FILES+=("FileC:10485760" "FileC:20971520")
fail=0
for np in 2 4; do
  topos="mesh"; [ "$np" -eq 4 ] && topos="mesh line"
  for topo in $topos; do
    for bs in 1024 4096; do
      for spec in "${FILES[@]}"; do
        scripts/run_test.sh "$np" "$bs" "${spec##*:}" "$topo" "${spec%%:*}" || fail=1
      done
    done
  done
done
echo; echo "Resultados em results/results.csv"; column -s, -t results/results.csv 2>/dev/null
# imagens logo depois do CSV (precisam de pandas/matplotlib: requirements.txt)
python3 tools/csv_to_png.py results/results.csv || echo "(PNG da tabela nao gerado: instale requirements.txt)"
# o mesmo texto que o `column` mostra no terminal, como PNG
column -s, -t results/results.csv > results/results_column.txt 2>/dev/null
python3 tools/text_to_png.py results/results_column.txt || echo "(PNG do column nao gerado: instale requirements.txt)"
exit $fail
