"""Converte um CSV de resultados em imagem de tabela (PNG), ao lado do CSV.

uso: python tools/csv_to_png.py results/results.csv   ->  results/results.png
"""
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import pandas as pd


def csv_to_png(csv_path):
    df = pd.read_csv(csv_path)
    png_path = os.path.splitext(csv_path)[0] + '.png'

    fig, ax = plt.subplots(figsize=(max(8, 1.35 * len(df.columns)), 0.34 * (len(df) + 1) + 0.6))
    ax.axis('off')
    tab = ax.table(cellText=df.astype(str).values, colLabels=df.columns,
                   cellLoc='center', loc='center')
    tab.auto_set_font_size(False)
    tab.set_fontsize(8)
    tab.auto_set_column_width(list(range(len(df.columns))))
    for (row, _), cell in tab.get_celld().items():
        if row == 0:
            cell.set_facecolor('#0b3d6b')
            cell.set_text_props(color='white', weight='bold')
        elif row % 2 == 0:
            cell.set_facecolor('#eef2f6')
    plt.savefig(png_path, bbox_inches='tight', dpi=200)
    plt.close()
    return png_path


if __name__ == '__main__':
    if len(sys.argv) != 2 or not os.path.exists(sys.argv[1]):
        print("uso: python tools/csv_to_png.py <arquivo.csv>")
        sys.exit(1)
    print(f"✅ Tabela salva em: {csv_to_png(sys.argv[1])}")
