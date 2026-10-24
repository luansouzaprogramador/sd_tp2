#include "meta.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int meta_save(const char *path, const Meta *m) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    char hex[65];
    fprintf(f, "P2PMETA 1\nfilename %s\nfilesize %llu\nblocksize %u\nnumblocks %u\n", m->filename,
            (unsigned long long)m->filesize, m->blocksize, m->numblocks);
    hex_encode(m->file_hash, 32, hex);
    fprintf(f, "sha256 %s\nblocks\n", hex);
    for (uint32_t i = 0; i < m->numblocks; i++) {
        hex_encode(m->block_hash[i], 32, hex);
        fprintf(f, "%s\n", hex);
    }
    return fclose(f);
}

int meta_load(const char *path, Meta *m) {
    memset(m, 0, sizeof *m);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    unsigned long long fsz = 0;
    int got = 0, in_blocks = 0;
    if (!fgets(line, sizeof line, f) || strncmp(line, "P2PMETA 1", 9) != 0) goto fail;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (sscanf(line, "filename %255[^\n]", m->filename) == 1) got |= 1;
        else if (sscanf(line, "filesize %llu", &fsz) == 1) got |= 2;
        else if (sscanf(line, "blocksize %u", &m->blocksize) == 1) got |= 4;
        else if (sscanf(line, "numblocks %u", &m->numblocks) == 1) got |= 8;
        else if (strncmp(line, "sha256 ", 7) == 0) {
            if (hex_decode(line + 7, m->file_hash, 32) != 0) goto fail;
            got |= 16;
        } else if (strcmp(line, "blocks") == 0) { in_blocks = 1; break; }
    }
    if (!in_blocks || got != 31) goto fail;
    m->filesize = fsz;
    if (m->blocksize == 0 || m->filesize == 0 || strchr(m->filename, '/')) goto fail;
    if (m->numblocks != (uint32_t)((m->filesize + m->blocksize - 1) / m->blocksize)) goto fail;
    m->block_hash = calloc(m->numblocks, 32);
    if (!m->block_hash) goto fail;
    for (uint32_t i = 0; i < m->numblocks; i++) {
        if (!fgets(line, sizeof line, f) || hex_decode(line, m->block_hash[i], 32) != 0) goto fail;
    }
    fclose(f);
    return 0;
fail:
    fclose(f);
    meta_free(m);
    return -1;
}

void meta_free(Meta *m) {
    free(m->block_hash);
    m->block_hash = NULL;
}

uint32_t meta_block_len(const Meta *m, uint32_t idx) {
    uint64_t off = (uint64_t)idx * m->blocksize;
    uint64_t rem = m->filesize - off;
    return (uint32_t)(rem < m->blocksize ? rem : m->blocksize);
}
