#ifndef META_H
#define META_H
#include <stdint.h>

/* Arquivo de metadados (equivalente ao .torrent):
 *   P2PMETA 1
 *   filename <nome>
 *   filesize <bytes>
 *   blocksize <bytes>
 *   numblocks <n>
 *   sha256 <hash do arquivo completo>
 *   blocks
 *   <sha256 do bloco 0>
 *   <sha256 do bloco 1> ...
 */
typedef struct {
    char filename[256];
    uint64_t filesize;
    uint32_t blocksize;
    uint32_t numblocks;
    uint8_t file_hash[32];
    uint8_t (*block_hash)[32]; /* numblocks entradas */
} Meta;

int meta_save(const char *path, const Meta *m);
int meta_load(const char *path, Meta *m);
void meta_free(Meta *m);
/* Tamanho real do bloco idx (o último pode ser menor). */
uint32_t meta_block_len(const Meta *m, uint32_t idx);

#endif
