import pandas as pd
import glob
import os

# Procura todos os arquivos de rodadas gerados
arquivos = glob.glob('results/results_run_*.csv')
if not arquivos:
    print("❌ Nenhum arquivo results_run_*.csv encontrado. Rode o benchmark primeiro.")
    exit(1)

print(f"Lendo {len(arquivos)} rodadas...")
dfs = [pd.read_csv(f) for f in arquivos]
df_all = pd.concat(dfs, ignore_index=True)

# Agrupa pelos parâmetros do teste e calcula a média e o desvio padrão do tempo
df_avg = df_all.groupby(['rotulo', 'peers', 'bloco_bytes', 'arquivo_bytes', 'topologia']).agg(
    media_tempo_s=('maior_tempo_peer_s', 'mean'),
    desvio_tempo_s=('maior_tempo_peer_s', 'std')
).reset_index()

# Arredonda para 3 casas decimais
df_avg['media_tempo_s'] = df_avg['media_tempo_s'].round(3)
df_avg['desvio_tempo_s'] = df_avg['desvio_tempo_s'].round(3)

# Salva o arquivo consolidado
os.makedirs('results', exist_ok=True)
df_avg.to_csv('results/results_avg.csv', index=False)
print("✅ Média das rodadas salva em: results/results_avg.csv")
print(df_avg.head(10)) # Mostra os primeiros 10 para conferir

# Gera a imagem da tabela logo depois do CSV
from csv_to_png import csv_to_png
print(f"✅ Tabela de médias salva em: {csv_to_png('results/results_avg.csv')}")