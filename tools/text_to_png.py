"""Converte um arquivo de texto (ex.: saida do `column -s, -t`) em imagem PNG monoespacada.

uso: python tools/text_to_png.py results/results_column.txt  ->  results/results_column.png
"""
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def text_to_png(txt_path):
    with open(txt_path, encoding='utf-8', errors='replace') as f:
        lines = f.read().expandtabs(4).splitlines() or ['(vazio)']
    png_path = os.path.splitext(txt_path)[0] + '.png'

    width = max(len(l) for l in lines)
    fig = plt.figure(figsize=(max(6, 0.085 * width + 0.6), 0.17 * len(lines) + 0.6))
    fig.patch.set_facecolor('#0f1419')
    fig.text(0.01, 0.99, '\n'.join(lines), family='monospace', fontsize=8,
             color='#d7dde4', va='top', ha='left', linespacing=1.3)
    plt.savefig(png_path, dpi=150, facecolor=fig.get_facecolor())
    plt.close()
    return png_path


if __name__ == '__main__':
    if len(sys.argv) != 2 or not os.path.exists(sys.argv[1]):
        print("uso: python tools/text_to_png.py <arquivo.txt>")
        sys.exit(1)
    print(f"✅ Imagem salva em: {text_to_png(sys.argv[1])}")
