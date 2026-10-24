/*
 * peer.c - Peer P2P simétrico (servidor + cliente no mesmo processo).
 *
 *  - Servidor: thread de accept + uma thread por conexão recebida; responde
 *    GET_BITFIELD e GET_BLOCK com os blocos que o peer já possui.
 *  - Cliente: uma thread por vizinho configurado; cada uma mantém uma conexão
 *    TCP, escolhe blocos faltantes (rarest-first), solicita, verifica o SHA-256
 *    do bloco e grava no arquivo. Bloco gravado => já pode ser servido (Seeder).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
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
#include <sys/types.h>
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
    uint8_t *bitfield;      /* blocos que o vizinho possui (último GET_BITFIELD) */
    uint32_t blocks_recv;   /* blocos baixados deste vizinho */
    uint64_t bytes_recv;
} Neighbor;

typedef struct {
    char id[32];
    char ip[64];
    int port;
    Meta meta;
    char dir[400], final_path[1024], part_path[1040];
    int fd;                       /* arquivo (final ou .part) */
    uint8_t *have;                /* blocos já gravados e verificados */
    uint8_t *inflight;            /* blocos com requisição em andamento */
    uint32_t have_count;
    volatile int complete;        /* todos os blocos presentes */
    volatile int verified;        /* arquivo remontado e SHA-256 conferido */
    int initial_seeder;
    pthread_mutex_t mu;           /* protege have/inflight/contadores/bitfields */
    Neighbor neigh[MAX_NEIGH];
    int nneigh;
    volatile sig_atomic_t stop;
    double t_start, t_dl_start, t_done;
    FILE *logf;
    int quiet;
    int linger;                   /* segundos servindo após completar (-1 = infinito) */
    pthread_mutex_t logmu;
    uint64_t served_blocks, served_bytes;
    uint32_t preexisting;         /* blocos já presentes na partida (retomada) */
} Peer;

static Peer P;

/* ------------------------------------------------------------------ log */
static void plog(const char *lvl, const char *fmt, ...) {
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&P.logmu);
    double t = now_s() - P.t_start;
    int verbose_only = (strcmp(lvl, "RECV") == 0 || strcmp(lvl, "SEND") == 0);
    if (!(P.quiet && verbose_only)) {
        printf("[%9.3f] [%s] [%-5s] %s\n", t, P.id, lvl, msg);
        fflush(stdout);
    }
    if (P.logf) {
        fprintf(P.logf, "[%9.3f] [%s] [%-5s] %s\n", t, P.id, lvl, msg);
        fflush(P.logf);
    }
    pthread_mutex_unlock(&P.logmu);
}

/* ------------------------------------------------------------- storage */
static uint32_t scan_blocks(int fd) {
    uint8_t *buf = malloc(P.meta.blocksize);
    uint32_t ok = 0;
    if (!buf) return 0;
    for (uint32_t i = 0; i < P.meta.numblocks; i++) {
        uint32_t len = meta_block_len(&P.meta, i);
        ssize_t n = pread(fd, buf, len, (off_t)i * P.meta.blocksize);
        if (n == (ssize_t)len) {
            uint8_t h[32];
            sha256_buf(buf, len, h);
            if (memcmp(h, P.meta.block_hash[i], 32) == 0) {
                P.have[i] = 1;
                ok++;
            }
        }
    }
    free(buf);
    return ok;
}

static void finalize_download(void) {
    char hex[65], want[65];
    fsync(P.fd);
    hex_encode(P.meta.file_hash, 32, want);
    if (sha256_file(P.part_path, hex) != 0 || strcmp(hex, want) != 0) {
        plog("ERROR", "SHA-256 do arquivo remontado NAO confere (%s != %s)", hex, want);
        return;
    }
    if (rename(P.part_path, P.final_path) != 0) {
        plog("ERROR", "rename(%s): %s", P.part_path, strerror(errno));
        return;
    }
    P.t_done = now_s();
    P.verified = 1;
    double el = P.t_done - P.t_dl_start;
    plog("INFO", "DOWNLOAD COMPLETO: arquivo=%s bytes=%llu tempo=%.3fs taxa=%.2fMB/s sha256=OK",
         P.meta.filename, (unsigned long long)P.meta.filesize, el,
         el > 0 ? (double)P.meta.filesize / 1048576.0 / el : 0.0);
    uint32_t downloaded = P.meta.numblocks - P.preexisting;
    for (int i = 0; i < P.nneigh; i++) {
        Neighbor *n = &P.neigh[i];
        plog("INFO", "  origem %s:%d -> %u blocos (%.1f%%), %llu bytes", n->host, n->port,
             n->blocks_recv, downloaded ? 100.0 * n->blocks_recv / downloaded : 0.0,
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
        if (P.fd >= 0) {
            uint32_t ok = scan_blocks(P.fd);
            if (ok == P.meta.numblocks) {
                P.have_count = ok;
                P.complete = P.verified = P.initial_seeder = 1;
                plog("INFO", "SEEDER: arquivo completo e verificado (%u blocos de %u B, %llu bytes)", ok,
                     P.meta.blocksize, (unsigned long long)P.meta.filesize);
                return 0;
            }
            close(P.fd);
            memset(P.have, 0, P.meta.numblocks);
            plog("WARN", "%s existe mas nao confere com os metadados; ignorando", P.final_path);
        }
    }
    P.fd = open(P.part_path, O_RDWR | O_CREAT, 0644);
    if (P.fd < 0) { plog("ERROR", "open(%s): %s", P.part_path, strerror(errno)); return -1; }
    if (ftruncate(P.fd, (off_t)P.meta.filesize) != 0) { plog("ERROR", "ftruncate: %s", strerror(errno)); return -1; }
    P.have_count = scan_blocks(P.fd);
    P.preexisting = P.have_count;
    plog("INFO", "LEECHER: %u/%u blocos ja presentes (blocos de %u B, %llu bytes)", P.have_count,
         P.meta.numblocks, P.meta.blocksize, (unsigned long long)P.meta.filesize);
    return 0;
}

/* -------------------------------------------------------------- servidor */
typedef struct {
    int fd;
    char addr[64];
} Conn;

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
            if (!tmp) { free(pl); break; }
            pthread_mutex_lock(&P.mu);
            memcpy(tmp, P.have, P.meta.numblocks);
            pthread_mutex_unlock(&P.mu);
            int rc = msg_send(c->fd, MSG_BITFIELD, tmp, P.meta.numblocks);
            free(tmp);
            if (rc < 0) { free(pl); break; }
        } else if (type == MSG_GET_BLOCK && len == 4) {
            uint32_t idx;
            memcpy(&idx, pl, 4);
            idx = ntohl(idx);
            int ok = 0;
            uint8_t *out = NULL;
            uint32_t bl = 0;
            if (idx < P.meta.numblocks) {
                pthread_mutex_lock(&P.mu);
                ok = P.have[idx];
                pthread_mutex_unlock(&P.mu);
            }
            if (ok) {
                bl = meta_block_len(&P.meta, idx);
                out = malloc(4 + bl);
                if (out) {
                    uint32_t n = htonl(idx);
                    memcpy(out, &n, 4);
                    ssize_t r = pread(P.fd, out + 4, bl, (off_t)idx * P.meta.blocksize);
                    if (r != (ssize_t)bl) ok = 0;
                } else ok = 0;
            }
            int rc;
            if (ok) {
                rc = msg_send(c->fd, MSG_BLOCK, out, 4 + bl);
                if (rc == 0) {
                    pthread_mutex_lock(&P.mu);
                    P.served_blocks++;
                    P.served_bytes += bl;
                    pthread_mutex_unlock(&P.mu);
                    plog("SEND", "bloco %u (%u B) -> %s", idx, bl, remote);
                }
            } else {
                uint32_t n = htonl(idx);
                rc = msg_send(c->fd, MSG_NO_BLOCK, &n, 4);
            }
            free(out);
            if (rc < 0) { free(pl); break; }
        } else if (type == MSG_BYE) {
            free(pl);
            break;
        }
        free(pl);
    }
    close(c->fd);
    free(c);
    return NULL;
}

static int make_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)P.port);
    if (inet_pton(AF_INET, P.ip, &a.sin_addr) != 1) { close(fd); return -1; }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 64) < 0) { close(fd); return -1; }
    return fd;
}

static void *server_thread(void *arg) {
    int lfd = (int)(intptr_t)arg;
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};
    while (!P.stop) {
        if (poll(&pfd, 1, 200) <= 0) continue;
        struct sockaddr_in ra;
        socklen_t rl = sizeof ra;
        int cfd = accept(lfd, (struct sockaddr *)&ra, &rl);
        if (cfd < 0) continue;
        Conn *c = calloc(1, sizeof *c);
        if (!c) { close(cfd); continue; }
        c->fd = cfd;
        char ip[48];
        inet_ntop(AF_INET, &ra.sin_addr, ip, sizeof ip);
        snprintf(c->addr, sizeof c->addr, "%s:%d", ip, ntohs(ra.sin_port));
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, c) != 0) { close(cfd); free(c); continue; }
        pthread_detach(t);
    }
    close(lfd);
    return NULL;
}

/* --------------------------------------------------------------- cliente */
static int connect_to(const Neighbor *nb, double deadline) {
    char port[16];
    snprintf(port, sizeof port, "%d", nb->port);
    while (!P.stop && now_s() < deadline) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(nb->host, port, &hints, &res) == 0 && res) {
            int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (fd >= 0) {
                if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
                    int one = 1;
                    struct timeval tv = {RECV_TIMEOUT_S, 0};
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                    freeaddrinfo(res);
                    return fd;
                }
                close(fd);
            }
            freeaddrinfo(res);
        }
        usleep(250000);
    }
    return -1;
}

static int refresh_bitfield(int fd, Neighbor *nb) {
    uint8_t t, *pl;
    uint32_t len;
    if (msg_send(fd, MSG_GET_BITFIELD, NULL, 0) < 0) return -1;
    if (msg_recv(fd, &t, &pl, &len) < 0) return -1;
    if (t != MSG_BITFIELD || len != P.meta.numblocks) { free(pl); return -1; }
    pthread_mutex_lock(&P.mu);
    memcpy(nb->bitfield, pl, len);
    pthread_mutex_unlock(&P.mu);
    free(pl);
    return 0;
}

/* Rarest-first: entre os blocos que faltam, que ninguém está baixando e que
 * este vizinho possui, escolhe o que menos vizinhos possuem. */
static long pick_block(Neighbor *nb) {
    static __thread unsigned seed = 0;
    if (!seed) seed = (unsigned)(now_s() * 1e6) ^ (unsigned)(uintptr_t)pthread_self();
    long best = -1;
    int best_r = INT_MAX;
    uint32_t n = P.meta.numblocks;
    pthread_mutex_lock(&P.mu);
    uint32_t start = (uint32_t)(rand_r(&seed) % n);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (start + k) % n;
        if (P.have[i] || P.inflight[i] || !nb->bitfield[i]) continue;
        int r = 0;
        for (int j = 0; j < P.nneigh; j++) r += P.neigh[j].bitfield[i] ? 1 : 0;
        if (r < best_r) {
            best_r = r;
            best = i;
            if (r <= 1) break;
        }
    }
    if (best >= 0) P.inflight[best] = 1;
    pthread_mutex_unlock(&P.mu);
    return best;
}

static void release_inflight(uint32_t idx) {
    pthread_mutex_lock(&P.mu);
    P.inflight[idx] = 0;
    pthread_mutex_unlock(&P.mu);
}

static void store_block(uint32_t idx, const uint8_t *data, uint32_t len, Neighbor *nb) {
    if (pwrite(P.fd, data, len, (off_t)idx * P.meta.blocksize) != (ssize_t)len) {
        plog("ERROR", "pwrite bloco %u: %s", idx, strerror(errno));
        release_inflight(idx);
        return;
    }
    int last = 0;
    uint32_t cnt;
    pthread_mutex_lock(&P.mu);
    if (!P.have[idx]) {
        P.have[idx] = 1;
        P.have_count++;
        nb->blocks_recv++;
        nb->bytes_recv += len;
    }
    P.inflight[idx] = 0;
    cnt = P.have_count;
    if (cnt == P.meta.numblocks && !P.complete) {
        P.complete = 1;
        last = 1;
    }
    pthread_mutex_unlock(&P.mu);
    plog("RECV", "bloco %u (%u B) <- %s:%d [%u/%u] SHA-256 ok", idx, len, nb->host, nb->port, cnt,
         P.meta.numblocks);
    if (last) finalize_download();
}

/* 0 = ok (ou NO_BLOCK/hash inválido tratados), -1 = erro de conexão. */
static int request_block(int fd, Neighbor *nb, uint32_t idx) {
    uint32_t n = htonl(idx);
    uint8_t t, *pl;
    uint32_t len;
    if (msg_send(fd, MSG_GET_BLOCK, &n, 4) < 0) return -1;
    if (msg_recv(fd, &t, &pl, &len) < 0) return -1;
    int rc = 0;
    if (t == MSG_NO_BLOCK) {
        pthread_mutex_lock(&P.mu);
        nb->bitfield[idx] = 0;
        P.inflight[idx] = 0;
        pthread_mutex_unlock(&P.mu);
    } else if (t == MSG_BLOCK && len >= 4) {
        uint32_t ridx, bl = meta_block_len(&P.meta, idx);
        memcpy(&ridx, pl, 4);
        ridx = ntohl(ridx);
        if (ridx != idx || len - 4 != bl) {
            rc = -1;
        } else {
            uint8_t h[32];
            sha256_buf(pl + 4, bl, h);
            if (memcmp(h, P.meta.block_hash[idx], 32) != 0) {
                plog("ERROR", "bloco %u de %s:%d com SHA-256 invalido; descartado", idx, nb->host, nb->port);
                pthread_mutex_lock(&P.mu);
                nb->bitfield[idx] = 0;
                P.inflight[idx] = 0;
                pthread_mutex_unlock(&P.mu);
            } else {
                store_block(idx, pl + 4, bl, nb);
            }
        }
    } else {
        rc = -1;
    }
    free(pl);
    return rc;
}

static void *client_worker(void *arg) {
    Neighbor *nb = arg;
    int fd = -1;
    uint32_t since_refresh = 0;
    while (!P.stop && !P.complete) {
        if (fd < 0) {
            fd = connect_to(nb, now_s() + CONNECT_RETRY_S);
            if (fd < 0) continue;
            if (msg_send(fd, MSG_HELLO, P.id, (uint32_t)strlen(P.id)) < 0 || refresh_bitfield(fd, nb) < 0) {
                close(fd);
                fd = -1;
                usleep(200000);
                continue;
            }
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
            release_inflight((uint32_t)idx);
            plog("WARN", "falha na conexao com %s:%d; reconectando", nb->host, nb->port);
            close(fd);
            fd = -1;
            usleep(200000);
        } else if (++since_refresh >= 256) {
            since_refresh = 0;
            if (refresh_bitfield(fd, nb) < 0) { close(fd); fd = -1; }
        }
    }
    if (fd >= 0) {
        msg_send(fd, MSG_BYE, NULL, 0);
        close(fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ main */
static void on_signal(int s) { (void)s; P.stop = 1; }

static void usage(const char *p) {
    fprintf(stderr,
            "uso: %s --id NOME --port PORTA --meta ARQ.meta --dir DIR [opcoes]\n"
            "  --ip IP               endereco para escutar (padrao 127.0.0.1)\n"
            "  --neighbors h:p,h:p   vizinhos (lista estatica, sem tracker)\n"
            "  --log ARQUIVO         grava log tambem em arquivo\n"
            "  --linger SEG          segundos servindo apos completar (padrao -1 = ate Ctrl+C)\n"
            "  --quiet               nao imprime RECV/SEND no terminal (continuam no log)\n"
            "O arquivo completo (Seeder) deve estar em DIR/<filename do .meta>.\n",
            p);
}

static int parse_neighbors(char *list) {
    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *c = strrchr(tok, ':');
        if (!c || P.nneigh >= MAX_NEIGH) return -1;
        *c = '\0';
        Neighbor *n = &P.neigh[P.nneigh++];
        snprintf(n->host, sizeof n->host, "%s", tok);
        n->port = atoi(c + 1);
        if (n->port <= 0 || n->port > 65535) return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *meta_path = NULL, *log_path = NULL;
    char *neigh = NULL;
    P.t_start = now_s();
    P.linger = -1;
    snprintf(P.ip, sizeof P.ip, "127.0.0.1");
    pthread_mutex_init(&P.mu, NULL);
    pthread_mutex_init(&P.logmu, NULL);

    static struct option opts[] = {{"id", 1, 0, 'i'},   {"ip", 1, 0, 'a'},     {"port", 1, 0, 'p'},
                                   {"meta", 1, 0, 'm'}, {"dir", 1, 0, 'd'},    {"neighbors", 1, 0, 'n'},
                                   {"log", 1, 0, 'l'},  {"linger", 1, 0, 'g'}, {"quiet", 0, 0, 'q'},
                                   {0, 0, 0, 0}};
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
    if (log_path && !(P.logf = fopen(log_path, "w"))) { perror("log"); return 1; }
    if (neigh && parse_neighbors(neigh) != 0) { fprintf(stderr, "lista de vizinhos invalida\n"); return 1; }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (meta_load(meta_path, &P.meta) != 0) { fprintf(stderr, "metadados invalidos: %s\n", meta_path); return 1; }
    for (int i = 0; i < P.nneigh; i++) {
        P.neigh[i].bitfield = calloc(P.meta.numblocks, 1);
        if (!P.neigh[i].bitfield) return 1;
    }
    plog("INFO", "peer %s em %s:%d, %d vizinho(s) configurado(s)", P.id, P.ip, P.port, P.nneigh);
    if (storage_init() != 0) return 1;

    int lfd = make_listener();
    if (lfd < 0) { plog("ERROR", "nao foi possivel escutar em %s:%d: %s", P.ip, P.port, strerror(errno)); return 1; }
    pthread_t st;
    pthread_create(&st, NULL, server_thread, (void *)(intptr_t)lfd);
    plog("INFO", "servidor escutando em %s:%d", P.ip, P.port);

    if (!P.complete) {
        if (P.nneigh == 0) { plog("ERROR", "leecher sem vizinhos: nada a baixar"); return 1; }
        P.t_dl_start = now_s();
        if (P.have_count == P.meta.numblocks) {
            P.complete = 1;
            finalize_download();
        } else {
            for (int i = 0; i < P.nneigh; i++) {
                pthread_t t;
                pthread_create(&t, NULL, client_worker, &P.neigh[i]);
                pthread_detach(t);
            }
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
    pthread_mutex_lock(&P.mu);
    plog("INFO", "encerrando: blocos servidos=%llu (%llu bytes)", (unsigned long long)P.served_blocks,
         (unsigned long long)P.served_bytes);
    pthread_mutex_unlock(&P.mu);
    if (P.logf) fclose(P.logf);
    return 0;
}
