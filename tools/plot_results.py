import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import os

os.makedirs('results', exist_ok=True)
sns.set_theme(style="whitegrid")

# Lê o arquivo de MÉDIAS em vez do arquivo de uma única rodada
df = pd.read_csv('results/results_avg.csv')

# ---------------------------------------------------------
# Gráfico 1: Mesh vs Line (Média com barra de erro)
# ---------------------------------------------------------
df_g1 = df[(df['rotulo'] == 'FileC') & (df['arquivo_bytes'] == 10485760) & 
           (df['peers'] == 4) & (df['bloco_bytes'] == 1024)]

plt.figure(figsize=(6, 4))
# yerr adiciona as barras de erro (desvio padrão)
ax1 = sns.barplot(data=df_g1, x='topologia', y='media_tempo_s', hue='topologia', 
                  legend=False, palette=['#2ca02c', '#d62728'], 
                  errorbar='sd', capsize=0.1)
plt.title('Impacto da Topologia (File C - 10MB, 4 Peers, Bloco 1KB)')
plt.ylabel('Tempo Médio do Peer Mais Lento (s)')
plt.xlabel('Topologia')

for p in ax1.patches:
    ax1.annotate(f"{p.get_height():.2f}s", 
                 (p.get_x() + p.get_width() / 2., p.get_height()), 
                 ha='center', va='center', xytext=(0, 9), textcoords='offset points')
plt.savefig('results/grafico_topologia.png', bbox_inches='tight', dpi=300)
plt.close()

# ---------------------------------------------------------
# Gráfico 2: Impacto do Tamanho do Bloco
# ---------------------------------------------------------
df_g2 = df[(df['rotulo'] == 'FileC') & (df['arquivo_bytes'] == 10485760) & 
           (df['peers'] == 2) & (df['topologia'] == 'mesh')] # Usando 2 peers para isolar

plt.figure(figsize=(6, 4))
ax2 = sns.barplot(data=df_g2, x='bloco_bytes', y='media_tempo_s', hue='bloco_bytes',
                  legend=False, palette=['#1f77b4', '#ff7f0e'], 
                  errorbar='sd', capsize=0.1)
plt.title('Impacto do Tamanho do Bloco (File C - 10MB, 2 Peers, Mesh)')
plt.ylabel('Tempo Médio do Peer Mais Lento (s)')
plt.xlabel('Tamanho do Bloco (Bytes)')

for p in ax2.patches:
    ax2.annotate(f"{p.get_height():.2f}s", 
                 (p.get_x() + p.get_width() / 2., p.get_height()), 
                 ha='center', va='center', xytext=(0, 9), textcoords='offset points')
plt.savefig('results/grafico_bloco.png', bbox_inches='tight', dpi=300)
plt.close()

# ---------------------------------------------------------
# Gráfico 3: Escalabilidade (Linhas com barras de erro)
# ---------------------------------------------------------
df_g3 = df[(df['bloco_bytes'] == 1024) & (df['topologia'] == 'mesh')].copy()
df_g3['Tamanho'] = df_g3['arquivo_bytes'].apply(lambda b: f"{b // 1048576}MB" if b >= 1048576 else f"{b // 1024}KB")
ordem = ['10KB', '20KB', '1MB', '5MB', '10MB', '20MB']

plt.figure(figsize=(7, 4))
sns.lineplot(data=df_g3, x='Tamanho', y='media_tempo_s', 
             hue='peers', marker='o', sort=False,
             palette={2: '#f1a5a5', 4: '#3b1c4a'})

plt.title('Escalabilidade: Tempo Médio de Download por Tamanho (Mesh, Bloco 1KB)')
plt.ylabel('Tempo Médio do Peer Mais Lento (s)')
plt.xlabel('Tamanho do Arquivo')
plt.savefig('results/grafico_escalabilidade.png', bbox_inches='tight', dpi=300)
plt.close()

print("✅ Gráficos com médias e barras de erro gerados com sucesso!")