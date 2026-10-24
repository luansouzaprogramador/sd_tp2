/*
 * peer.c - Peer P2P simétrico (servidor + cliente no mesmo processo).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "meta.h"
#include "protocol.h"
#include "sha256.h"
#include "util.h"

#define MAX_NEIGH 16
#define RECV_TIMEOUT_S 15
#define CONNECT_RETRY_S 60

typedef struct {
    char host[64];
    int port;
    uint8_t *bitfield;
    uint32_t blocks_recv;
    uint64_t bytes_recv;
} Neighbor;

typedef struct {
    char id[32];
    char ip[64];
    int port;
    Meta meta;
    char dir[400], final_path[1024], part_path[1040];
    int fd;
    uint8_t *have;
    uint8_t *inflight;
    uint32_t have_count;
    volatile int complete;
    volatile int verified;
    int initial_seeder;
    pthread_mutex_t mu;
    Neighbor neigh[MAX_NEIGH];
    int nneigh;
    volatile sig_atomic_t stop;
    double t_start, t_dl_start, t_done;
    FILE *logf;
    int quiet;
    int linger;
    pthread_mutex_t logmu;
    uint64_t served_blocks, served_bytes;
} Peer;

static Peer P;

/* ------------------------------------------------------------------ log */
static void plog(const char *lvl, const char *fmt, ...) {
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    
    if (P.quiet && (strcmp(lvl, "RECV") == 0 || strcmp(lvl, "SEND") == 0)) return;

    pthread_mutex_lock(&P.logmu);
    double t = now_s() - P.t_start;
    printf("[%9.3f] [%s] [%-5s] %s\n", t, P.id, lvl, msg);
    fflush(stdout);
    if (P.logf) {
        fprintf(P.logf, "[%9.3f] [%s] [%-5s] %s\n", t, P.id, lvl, msg);
        fflush(P.logf);
    }
    pthread_mutex_unlock(&P.logmu);
}

/* ------------------------------------------------------------- storage */
static void finalize_download(void) {
    char hex[65], want[65];
    fsync(P.fd);
    hex_encode(P.meta.file_hash, 32, want);
    if (sha256_file(P.part_path, hex) != 0 || strcmp(hex, want) != 0) {
        plog("ERROR", "SHA-256 do arquivo remontado NAO confere");
        return;
    }
    rename(P.part_path, P.final_path);
    P.t_done = now_s();
    P.verified = 1;
    double el = P.t_done - P.t_dl_start;
    plog("INFO", "DOWNLOAD COMPLETO: arquivo=%s bytes=%llu tempo=%.3fs taxa=%.2fMB/s sha256=OK",
         P.meta.filename, (unsigned long long)P.meta.filesize, el,
         el > 0 ? (double)P.meta.filesize / 1048576.0 / el : 0.0);
    
    for (int i = 0; i < P.nneigh; i++) {
        Neighbor *n = &P.neigh[i];
        plog("INFO", "  origem %s:%d -> %u blocos (%.1f%%), %llu bytes", n->host, n->port,
             n->blocks_recv, 100.0 * n->blocks_recv / P.meta.numblocks,
             (unsigned long long)n->bytes_recv);
    }
}

static int storage_init(void) {
    snprintf(P.final_path, sizeof P.final_path, "%s/%s", P.dir, P.meta.filename);
    snprintf(P.part_path, sizeof P.part_path, "%s/%s.part", P.dir, P.meta.filename);
    mkdir(P.dir, 0755);
    
    P.have = calloc(P.meta.numblocks, 1);
    P.inflight = calloc(P.meta.numblocks, 1);
    if (!P.have || !P.inflight) return -1;

    struct stat st;
    if (stat(P.final_path, &st) == 0 && (uint64_t)st.st_size == P.meta.filesize) {
        P.fd = open(P.final_path, O_RDONLY);
        P.complete = P.verified = P.initial_seeder = 1;
        P.have_count = P.meta.numblocks;
        memset(P.have, 1, P.meta.numblocks);
        plog("INFO", "SEEDER: arquivo completo (%u blocos, %llu bytes)", P.meta.numblocks, (unsigned long long)P.meta.filesize);
        return 0;
    }
    
    P.fd = open(P.part_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (P.fd < 0 || ftruncate(P.fd, (off_t)P.meta.filesize) != 0) return -1;
    plog("INFO", "LEECHER: 0/%u blocos (blocos de %u B, %llu bytes)", P.meta.numblocks, P.meta.blocksize, (unsigned long long)P.meta.filesize);
    return 0;
}

/* -------------------------------------------------------------- servidor */
typedef struct { int fd; char addr[64]; } Conn;

static void *conn_thread(void *arg) {
    Conn *c = arg;
    char remote[64];
    snprintf(remote, sizeof remote, "%s", c->addr);
    uint8_t type, *pl;
    uint32_t len;
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    while (!P.stop && msg_recv(c->fd, &type, &pl, &len) == 0) {
        if (type == MSG_HELLO) {
            snprintf(remote, sizeof remote, "%.*s", (int)(len < 40 ? len : 40), pl ? (char *)pl : "");
            plog("INFO", "conexao recebida de %s (%s)", remote, c->addr);
        } else if (type == MSG_GET_BITFIELD) {
            uint8_t *tmp = malloc(P.meta.numblocks);
            pthread_mutex_lock(&P.mu);
            memcpy(tmp, P.have, P.meta.numblocks);
            pthread_mutex_unlock(&P.mu);
            msg_send(c->fd, MSG_BITFIELD, tmp, P.meta.numblocks);
            free(tmp);
        } else if (type == MSG_GET_BLOCK && len == 4) {
            uint32_t idx; memcpy(&idx, pl, 4); idx = ntohl(idx);
            int ok = 0;
            uint8_t *out = NULL;
            uint32_t bl = 0;
            pthread_mutex_lock(&P.mu);
            if (idx < P.meta.numblocks && P.have[idx]) {
                ok = 1; bl = meta_block_len(&P.meta, idx);
                out = malloc(4 + bl);
                uint32_t n = htonl(idx); memcpy(out, &n, 4);
                if (pread(P.fd, out + 4, bl, (off_t)idx * P.meta.blocksize) != (ssize_t)bl) ok = 0;
            }
            pthread_mutex_unlock(&P.mu);

            if (ok) {
                msg_send(c->fd, MSG_BLOCK, out, 4 + bl);
                pthread_mutex_lock(&P.mu);
                P.served_blocks++; P.served_bytes += bl;
                pthread_mutex_unlock(&P.mu);
                plog("SEND", "bloco %u (%u B) -> %s", idx, bl, remote);
            } else {
                uint32_t n = htonl(idx);
                msg_send(c->fd, MSG_NO_BLOCK, &n, 4);
            }
            free(out);
        } else if (type == MSG_BYE) {
            free(pl); break;
        }
        free(pl);
    }
    close(c->fd);
    free(c);
    return NULL;
}

static int make_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)P.port) };
    inet_pton(AF_INET, P.ip, &a.sin_addr);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 64) < 0) { close(fd); return -1; }
    return fd;
}

static void *server_thread(void *arg) {
    int lfd = (int)(intptr_t)arg;
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};
    while (!P.stop) {
        if (poll(&pfd, 1, 200) <= 0) continue;
        struct sockaddr_in ra; socklen_t rl = sizeof ra;
        int cfd = accept(lfd, (struct sockaddr *)&ra, &rl);
        if (cfd < 0) continue;
        Conn *c = calloc(1, sizeof *c);
        c->fd = cfd;
        char ip[48]; inet_ntop(AF_INET, &ra.sin_addr, ip, sizeof ip);
        snprintf(c->addr, sizeof c->addr, "%s:%d", ip, ntohs(ra.sin_port));
        pthread_t t;
        pthread_create(&t, NULL, conn_thread, c);
        pthread_detach(t);
    }
    close(lfd);
    return NULL;
}

/* --------------------------------------------------------------- cliente */
static int connect_to(const Neighbor *nb) {
    while (!P.stop) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(nb->port) };
        inet_pton(AF_INET, nb->host, &a.sin_addr);
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
            int one = 1;
            struct timeval tv = {RECV_TIMEOUT_S, 0};
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            return fd;
        }
        close(fd);
        usleep(250000);
    }
    return -1;
}

static int refresh_bitfield(int fd, Neighbor *nb) {
    uint8_t t, *pl; uint32_t len;
    if (msg_send(fd, MSG_GET_BITFIELD, NULL, 0) < 0) return -1;
    if (msg_recv(fd, &t, &pl, &len) < 0) return -1;
    if (t == MSG_BITFIELD && len == P.meta.numblocks) {
        pthread_mutex_lock(&P.mu);
        memcpy(nb->bitfield, pl, len);
        pthread_mutex_unlock(&P.mu);
        free(pl); return 0;
    }
    free(pl); return -1;
}

static long pick_block(Neighbor *nb) {
    long best = -1; int best_r = INT_MAX;
    pthread_mutex_lock(&P.mu);
    for (uint32_t i = 0; i < P.meta.numblocks; i++) {
        if (P.have[i] || P.inflight[i] || !nb->bitfield[i]) continue;
        int r = 0;
        for (int j = 0; j < P.nneigh; j++) r += P.neigh[j].bitfield[i] ? 1 : 0;
        if (r < best_r) { best_r = r; best = i; if (r <= 1) break; }
    }
    if (best >= 0) P.inflight[best] = 1;
    pthread_mutex_unlock(&P.mu);
    return best;
}

static void store_block(uint32_t idx, const uint8_t *data, uint32_t len, Neighbor *nb) {
    if (pwrite(P.fd, data, len, (off_t)idx * P.meta.blocksize) != (ssize_t)len) return;
    int last = 0;
    pthread_mutex_lock(&P.mu);
    if (!P.have[idx]) {
        P.have[idx] = 1; P.have_count++;
        nb->blocks_recv++; nb->bytes_recv += len;
    }
    P.inflight[idx] = 0;
    if (P.have_count == P.meta.numblocks && !P.complete) { P.complete = 1; last = 1; }
    pthread_mutex_unlock(&P.mu);
    plog("RECV", "bloco %u (%u B) <- %s:%d [%u/%u] SHA-256 ok", idx, len, nb->host, nb->port, P.have_count, P.meta.numblocks);
    if (last) finalize_download();
}

static int request_block(int fd, Neighbor *nb, uint32_t idx) {
    uint32_t n = htonl(idx);
    uint8_t t, *pl; uint32_t len;
    if (msg_send(fd, MSG_GET_BLOCK, &n, 4) < 0) return -1;
    if (msg_recv(fd, &t, &pl, &len) < 0) return -1;
    
    int rc = 0;
    if (t == MSG_NO_BLOCK) {
        pthread_mutex_lock(&P.mu); nb->bitfield[idx] = 0; P.inflight[idx] = 0; pthread_mutex_unlock(&P.mu);
    } else if (t == MSG_BLOCK && len >= 4) {
        uint32_t ridx; memcpy(&ridx, pl, 4); ridx = ntohl(ridx);
        uint32_t bl = meta_block_len(&P.meta, idx);
        if (ridx == idx && len - 4 == bl) {
            uint8_t h[32]; sha256_buf(pl + 4, bl, h);
            if (memcmp(h, P.meta.block_hash[idx], 32) == 0) store_block(idx, pl + 4, bl, nb);
            else { pthread_mutex_lock(&P.mu); nb->bitfield[idx] = 0; P.inflight[idx] = 0; pthread_mutex_unlock(&P.mu); }
        } else rc = -1;
    } else rc = -1;
    free(pl);
    return rc;
}

static void *client_worker(void *arg) {
    Neighbor *nb = arg;
    int fd = -1;
    uint32_t since_refresh = 0;
    while (!P.stop && !P.complete) {
        if (fd < 0) {
            fd = connect_to(nb);
            if (fd < 0) continue;
            msg_send(fd, MSG_HELLO, P.id, strlen(P.id));
            if (refresh_bitfield(fd, nb) < 0) { close(fd); fd = -1; continue; }
            plog("INFO", "conectado ao vizinho %s:%d", nb->host, nb->port);
            since_refresh = 0;
        }
        long idx = pick_block(nb);
        if (idx < 0) {
            usleep(100000);
            if (refresh_bitfield(fd, nb) < 0) { close(fd); fd = -1; }
            continue;
        }
        if (request_block(fd, nb, (uint32_t)idx) < 0) {
            pthread_mutex_lock(&P.mu); P.inflight[idx] = 0; pthread_mutex_unlock(&P.mu);
            plog("WARN", "falha na conexao com %s:%d; reconectando", nb->host, nb->port);
            close(fd); fd = -1; usleep(200000);
        } else if (++since_refresh >= 256) {
            since_refresh = 0;
            if (refresh_bitfield(fd, nb) < 0) { close(fd); fd = -1; }
        }
    }
    if (fd >= 0) { msg_send(fd, MSG_BYE, NULL, 0); close(fd); }
    return NULL;
}

/* ------------------------------------------------------------------ main */
static void on_signal(int s) { (void)s; P.stop = 1; }

static void usage(const char *p) {
    fprintf(stderr, "uso: %s --id NOME --port PORTA --meta ARQ.meta --dir DIR [opcoes]\n"
            "  --ip IP, --neighbors h:p,h:p, --log ARQ, --linger SEG, --quiet\n", p);
}

static int parse_neighbors(char *list) {
    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *c = strrchr(tok, ':'); if (!c) return -1;
        *c = '\0';
        Neighbor *n = &P.neigh[P.nneigh++];
        snprintf(n->host, sizeof n->host, "%s", tok);
        n->port = atoi(c + 1);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *meta_path = NULL, *log_path = NULL; char *neigh = NULL;
    P.t_start = now_s(); P.linger = -1;
    snprintf(P.ip, sizeof P.ip, "127.0.0.1");
    pthread_mutex_init(&P.mu, NULL); pthread_mutex_init(&P.logmu, NULL);

    static struct option opts[] = {{"id", 1, 0, 'i'}, {"ip", 1, 0, 'a'}, {"port", 1, 0, 'p'},
        {"meta", 1, 0, 'm'}, {"dir", 1, 0, 'd'}, {"neighbors", 1, 0, 'n'},
        {"log", 1, 0, 'l'}, {"linger", 1, 0, 'g'}, {"quiet", 0, 0, 'q'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'i': snprintf(P.id, sizeof P.id, "%s", optarg); break;
        case 'a': snprintf(P.ip, sizeof P.ip, "%s", optarg); break;
        case 'p': P.port = atoi(optarg); break;
        case 'm': meta_path = optarg; break;
        case 'd': snprintf(P.dir, sizeof P.dir, "%s", optarg); break;
        case 'n': neigh = strdup(optarg); break;
        case 'l': log_path = optarg; break;
        case 'g': P.linger = atoi(optarg); break;
        case 'q': P.quiet = 1; break;
        default: usage(argv[0]); return 1;
        }
    }
    if (!P.id[0] || !P.port || !meta_path || !P.dir[0]) { usage(argv[0]); return 1; }
    if (log_path) P.logf = fopen(log_path, "w");
    if (neigh) parse_neighbors(neigh);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    if (meta_load(meta_path, &P.meta) != 0) return 1;
    for (int i = 0; i < P.nneigh; i++) P.neigh[i].bitfield = calloc(P.meta.numblocks, 1);
    
    plog("INFO", "peer %s em %s:%d, %d vizinho(s) configurado(s)", P.id, P.ip, P.port, P.nneigh);
    if (storage_init() != 0) return 1;

    int lfd = make_listener();
    pthread_t st; pthread_create(&st, NULL, server_thread, (void *)(intptr_t)lfd);
    plog("INFO", "servidor escutando em %s:%d", P.ip, P.port);

    if (!P.complete) {
        P.t_dl_start = now_s();
        for (int i = 0; i < P.nneigh; i++) {
            pthread_t t; pthread_create(&t, NULL, client_worker, &P.neigh[i]); pthread_detach(t);
        }
    }

    double verified_at = -1;
    while (!P.stop) {
        if (P.verified) {
            if (verified_at < 0) {
                verified_at = now_s();
                if (!P.initial_seeder) plog("INFO", "agora atuando como SEEDER do arquivo completo");
            }
            if (P.linger >= 0 && now_s() - verified_at >= P.linger) break;
        }
        usleep(100000);
    }
    P.stop = 1;
    plog("INFO", "encerrando: blocos servidos=%llu (%llu bytes)", (unsigned long long)P.served_blocks, (unsigned long long)P.served_bytes);
    if (P.logf) fclose(P.logf);
    return 0;
}