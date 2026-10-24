#ifndef UTIL_H
#define UTIL_H
#include <stddef.h>
#include <stdint.h>

/* Tempo monotônico em segundos (double). */
double now_s(void);
/* bytes -> string hexadecimal (out precisa de 2*n+1 bytes). */
void hex_encode(const uint8_t *in, size_t n, char *out);
/* string hexadecimal -> bytes (n bytes). Retorna 0 se OK, -1 se inválido. */
int hex_decode(const char *hex, uint8_t *out, size_t n);

#endif
