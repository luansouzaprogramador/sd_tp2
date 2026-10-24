/* mkmeta <arquivo> <tamanho_do_bloco> <saida.meta>
 * Gera o arquivo de metadados com SHA-256 de cada bloco e do arquivo inteiro. */
#include "meta.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "uso: %s <arquivo> <tamanho_bloco> <saida.meta>\n", argv[0]);
        return 1;
    }
    long bs = atol(argv[2]);
    if (bs <= 0 || bs > 16 * 1024 * 1024) {
        fprintf(stderr, "tamanho de bloco inválido\n");
        return 1;
    }
    struct stat st;
    if (stat(argv[1], &st) != 0 || st.st_size <= 0) {
        perror("stat");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }

    Meta m;
    memset(&m, 0, sizeof m);
    const char *base = strrchr(argv[1], '/');
    base = base ? base + 1 : argv[1];
    snprintf(m.filename, sizeof m.filename, "%s", base);
    m.filesize = (uint64_t)st.st_size;
    m.blocksize = (uint32_t)bs;
    m.numblocks = (uint32_t)((m.filesize + m.blocksize - 1) / m.blocksize);
    m.block_hash = calloc(m.numblocks, 32);
    uint8_t *buf = malloc((size_t)bs);
    if (!m.block_hash || !buf) { perror("malloc"); return 1; }

    SHA256_CTX whole;
    sha256_init(&whole);
    for (uint32_t i = 0; i < m.numblocks; i++) {
        size_t want = meta_block_len(&m, i);
        if (fread(buf, 1, want, f) != want) { fprintf(stderr, "erro de leitura\n"); return 1; }
        sha256_buf(buf, want, m.block_hash[i]);
        sha256_update(&whole, buf, want);
    }
    sha256_final(&whole, m.file_hash);
    fclose(f);
    if (meta_save(argv[3], &m) != 0) { perror("meta_save"); return 1; }
    printf("metadados: %s (%llu bytes, %u blocos de %u B)\n", argv[3],
           (unsigned long long)m.filesize, m.numblocks, m.blocksize);
    free(buf);
    meta_free(&m);
    return 0;
}
