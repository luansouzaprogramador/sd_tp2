#include "protocol.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int send_all(int fd, const void *buf, size_t n, int flags) {
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = send(fd, p, n, flags | MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int recv_all(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) return -1; /* conexão fechada */
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int msg_send(int fd, uint8_t type, const void *payload, uint32_t len) {
    uint8_t h[5];
    uint32_t nl = htonl(len);
    h[0] = type;
    memcpy(h + 1, &nl, 4);
    if (send_all(fd, h, 5, len ? MSG_MORE : 0) < 0) return -1;
    if (len && send_all(fd, payload, len, 0) < 0) return -1;
    return 0;
}

int msg_recv(int fd, uint8_t *type, uint8_t **payload, uint32_t *len) {
    uint8_t h[5];
    uint32_t nl;
    *payload = NULL;
    if (recv_all(fd, h, 5) < 0) return -1;
    *type = h[0];
    memcpy(&nl, h + 1, 4);
    *len = ntohl(nl);
    if (*len > MAX_PAYLOAD) return -1;
    if (*len) {
        *payload = malloc(*len);
        if (!*payload) return -1;
        if (recv_all(fd, *payload, *len) < 0) {
            free(*payload);
            *payload = NULL;
            return -1;
        }
    }
    return 0;
}
