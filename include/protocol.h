#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <stddef.h>
#include <stdint.h>

/* Formato de toda mensagem (TCP):
 *   [1 byte tipo][4 bytes tamanho do payload (big-endian)][payload]
 *
 *   HELLO         payload = id textual do peer (sem '\0')
 *   GET_BITFIELD  payload vazio
 *   BITFIELD      payload = numblocks bytes (1 = possui o bloco, 0 = não)
 *   GET_BLOCK     payload = índice do bloco (uint32 big-endian)
 *   BLOCK         payload = índice (uint32 BE) + dados do bloco
 *   NO_BLOCK      payload = índice (uint32 BE)
 *   BYE           payload vazio
 */
enum {
    MSG_HELLO = 1,
    MSG_GET_BITFIELD = 2,
    MSG_BITFIELD = 3,
    MSG_GET_BLOCK = 4,
    MSG_BLOCK = 5,
    MSG_NO_BLOCK = 6,
    MSG_BYE = 7
};

#define MAX_PAYLOAD (64u * 1024u * 1024u)

int send_all(int fd, const void *buf, size_t n, int flags);
int recv_all(int fd, void *buf, size_t n);
int msg_send(int fd, uint8_t type, const void *payload, uint32_t len);
/* Aloca *payload com malloc (NULL se len==0); o chamador faz free(). */
int msg_recv(int fd, uint8_t *type, uint8_t **payload, uint32_t *len);

#endif
