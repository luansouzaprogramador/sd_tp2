#ifndef SHA256_H
#define SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len);
void sha256_final(SHA256_CTX *ctx, uint8_t hash[32]);
/* Atalho: hash de um buffer inteiro. */
void sha256_buf(const void *data, size_t len, uint8_t out[32]);
/* Hash de um arquivo (hex em out_hex, 65 bytes). Retorna 0 se OK. */
int sha256_file(const char *path, char out_hex[65]);

#endif
