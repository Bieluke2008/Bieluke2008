#include "net.h"

#include <SDL.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#define CONNECT_TIMEOUT_MS 4000
#define HTTP_TIMEOUT_MS 8000

static bool net_up;

bool net_init(void) {
    if (net_up) return true;
#ifdef __SWITCH__
    if (R_FAILED(socketInitializeDefault())) return false;
#endif
    net_up = true;
    return true;
}

void net_shutdown(void) {
#ifdef __SWITCH__
    if (net_up) socketExit();
#endif
    net_up = false;
}

bool net_parse_addr(const char *addr, char *host, size_t host_len, int *port, int def_port) {
    while (*addr == ' ') addr++;
    if (!strncmp(addr, "http://", 7)) addr += 7;
    const char *colon = strrchr(addr, ':');
    size_t n = colon ? (size_t)(colon - addr) : strcspn(addr, "/ ");
    if (n == 0 || n >= host_len) return false;
    memcpy(host, addr, n);
    host[n] = 0;
    *port = colon ? atoi(colon + 1) : def_port;
    return *port > 0 && *port < 65536;
}

/** A connected TCP socket to host:port, or -1. */
static int tcp_connect(const char *host, int port) {
    char ports[8];
    snprintf(ports, sizeof ports, "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ports, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (r < 0) {
        struct pollfd p = {fd, POLLOUT, 0};
        int err = 0;
        socklen_t len = sizeof err;
        if (poll(&p, 1, CONNECT_TIMEOUT_MS) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static bool send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/** recv with a timeout: bytes read, 0 on EOF, -1 on error/timeout. */
static ssize_t recv_timeout(int fd, void *buf, size_t len, int timeout_ms) {
    struct pollfd p = {fd, POLLIN, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return -1;
    return recv(fd, buf, len, 0);
}

// --- Growable byte buffer -------------------------------------------------------

typedef struct {
    unsigned char *p;
    size_t len, cap;
} Buf;

static void buf_add(Buf *b, const void *data, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void buf_drop(Buf *b, size_t n) {
    memmove(b->p, b->p + n, b->len - n);
    b->len -= n;
}

// --- HTTP -------------------------------------------------------------------------

#define MAX_HTTP 16

typedef struct {
    int id;
    bool done;
    int status;
    char *body;
    char method[8], host[128], path[256];
    int port;
    char *bearer, *req_body;
} HttpReq;

static SDL_mutex *http_lock;
static HttpReq http_reqs[MAX_HTTP];
static int http_next_id;

/** Decodes a chunked body in place; returns its length. */
static size_t dechunk(char *s, size_t len) {
    char *in = s, *out = s, *end = s + len;
    while (in < end) {
        char *nl = strstr(in, "\r\n");
        if (!nl) break;
        size_t n = strtoul(in, NULL, 16);
        in = nl + 2;
        if (n == 0 || in + n > end) break;
        memmove(out, in, n);
        out += n;
        in += n + 2;
    }
    *out = 0;
    return (size_t)(out - s);
}

static int http_main(void *arg) {
    HttpReq *r = arg;
    int status = 0;
    char *body = NULL;
    int fd = tcp_connect(r->host, r->port);
    if (fd >= 0) {
        size_t blen = r->req_body ? strlen(r->req_body) : 0;
        char head[1024];
        int n = snprintf(head, sizeof head,
                         "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: OpenFront-Switch\r\nAccept: */*\r\n"
                         "Connection: close\r\n",
                         r->method, r->path, r->host, r->port);
        if (r->bearer) n += snprintf(head + n, sizeof head - (size_t)n, "Authorization: Bearer %s\r\n", r->bearer);
        if (r->req_body || strcmp(r->method, "GET"))
            n += snprintf(head + n, sizeof head - (size_t)n,
                          "Content-Type: application/json\r\nContent-Length: %zu\r\n", blen);
        n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
        Buf resp = {0};
        if (send_all(fd, head, (size_t)n) && (!blen || send_all(fd, r->req_body, blen))) {
            char chunk[4096];
            ssize_t got;
            while ((got = recv_timeout(fd, chunk, sizeof chunk, HTTP_TIMEOUT_MS)) > 0) buf_add(&resp, chunk, (size_t)got);
        }
        close(fd);
        if (resp.len > 12 && !strncmp((char *)resp.p, "HTTP/1.", 7)) {
            status = atoi((char *)resp.p + 9);
            char *sep = strstr((char *)resp.p, "\r\n\r\n");
            if (sep) {
                *sep = 0;
                char *payload = sep + 4;
                size_t plen = resp.len - (size_t)(payload - (char *)resp.p);
                bool chunked = false;
                for (char *h = (char *)resp.p; *h; h++) *h = (char)tolower((unsigned char)*h);
                if (strstr((char *)resp.p, "transfer-encoding: chunked")) chunked = true;
                if (chunked) plen = dechunk(payload, plen);
                body = malloc(plen + 1);
                memcpy(body, payload, plen);
                body[plen] = 0;
            }
        }
        free(resp.p);
    }
    SDL_LockMutex(http_lock);
    r->status = status;
    r->body = body;
    r->done = true;
    SDL_UnlockMutex(http_lock);
    return 0;
}

int http_request(const char *method, const char *host, int port, const char *path, const char *bearer,
                 const char *body) {
    if (!http_lock) http_lock = SDL_CreateMutex();
    SDL_LockMutex(http_lock);
    HttpReq *slot = NULL;
    for (int i = 0; i < MAX_HTTP && !slot; i++)
        if (http_reqs[i].id == 0) slot = &http_reqs[i];
    if (!slot) {
        SDL_UnlockMutex(http_lock);
        return 0;
    }
    memset(slot, 0, sizeof *slot);
    slot->id = ++http_next_id;
    snprintf(slot->method, sizeof slot->method, "%s", method);
    snprintf(slot->host, sizeof slot->host, "%s", host);
    snprintf(slot->path, sizeof slot->path, "%s", path);
    slot->port = port;
    slot->bearer = bearer ? strdup(bearer) : NULL;
    slot->req_body = body ? strdup(body) : NULL;
    int id = slot->id;
    SDL_UnlockMutex(http_lock);
    SDL_DetachThread(SDL_CreateThreadWithStackSize(http_main, "http", 256 * 1024, slot));
    return id;
}

bool http_result(int id, int *status, char **body) {
    if (!http_lock || id <= 0) return false;
    bool done = false;
    SDL_LockMutex(http_lock);
    for (int i = 0; i < MAX_HTTP; i++) {
        HttpReq *r = &http_reqs[i];
        if (r->id != id || !r->done) continue;
        *status = r->status;
        *body = r->body;
        free(r->bearer);
        free(r->req_body);
        r->id = 0;
        done = true;
    }
    SDL_UnlockMutex(http_lock);
    return done;
}

// --- WebSocket --------------------------------------------------------------------

typedef struct Msg {
    struct Msg *next;
    size_t len;
    unsigned char data[];
} Msg;

typedef struct EvNode {
    struct EvNode *next;
    WsEvent ev;
} EvNode;

static SDL_mutex *ws_lock;  // everything below
static unsigned ws_gen;      // bumped by ws_open/ws_close: older threads stop
static Msg *send_head, *send_tail;
static EvNode *ev_head, *ev_tail;

typedef struct {
    unsigned gen;
    char host[128], path[256];
    int port;
} WsArgs;

static void push_event(unsigned gen, WsEventKind kind, const void *data, size_t len, int code) {
    SDL_LockMutex(ws_lock);
    if (gen == ws_gen) {
        EvNode *n = calloc(1, sizeof *n);
        n->ev.kind = kind;
        n->ev.code = code;
        n->ev.len = len;
        n->ev.data = malloc(len + 1);
        if (len) memcpy(n->ev.data, data, len);
        n->ev.data[len] = 0;
        if (ev_tail) ev_tail->next = n;
        else ev_head = n;
        ev_tail = n;
    }
    SDL_UnlockMutex(ws_lock);
}

static void clear_queues(void) {
    while (send_head) {
        Msg *m = send_head;
        send_head = m->next;
        free(m);
    }
    send_tail = NULL;
    while (ev_head) {
        EvNode *n = ev_head;
        ev_head = n->next;
        free(n->ev.data);
        free(n);
    }
    ev_tail = NULL;
}

static bool send_frame(int fd, int opcode, const void *data, size_t len) {
    unsigned char head[14];
    size_t h = 0;
    head[h++] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        head[h++] = (unsigned char)(0x80 | len);
    } else if (len < 65536) {
        head[h++] = 0x80 | 126;
        head[h++] = (unsigned char)(len >> 8);
        head[h++] = (unsigned char)len;
    } else {
        head[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) head[h++] = (unsigned char)((uint64_t)len >> (8 * i));
    }
    unsigned char mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (unsigned char)rand();
    memcpy(head + h, mask, 4);
    h += 4;
    unsigned char *masked = malloc(len + 1);
    for (size_t i = 0; i < len; i++) masked[i] = ((const unsigned char *)data)[i] ^ mask[i & 3];
    bool ok = send_all(fd, head, h) && (len == 0 || send_all(fd, masked, len));
    free(masked);
    return ok;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const unsigned char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

/** Sends the upgrade request and reads the 101 reply; leftover bytes go to *rest. */
static bool handshake(int fd, const WsArgs *a, Buf *rest) {
    unsigned char key[16];
    for (int i = 0; i < 16; i++) key[i] = (unsigned char)rand();
    char key64[32];
    base64(key, 16, key64);
    char req[1024];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: http://%s:%d\r\n"
                     "User-Agent: OpenFront-Switch\r\n\r\n",
                     a->path, a->host, a->port, key64, a->host, a->port);
    if (!send_all(fd, req, (size_t)n)) return false;
    Buf b = {0};
    char chunk[2048];
    char *end = NULL;
    while (!end) {
        ssize_t got = recv_timeout(fd, chunk, sizeof chunk, CONNECT_TIMEOUT_MS);
        if (got <= 0) break;
        buf_add(&b, chunk, (size_t)got);
        end = strstr((char *)b.p, "\r\n\r\n");
    }
    bool ok = end && b.len > 12 && !strncmp((char *)b.p + 9, "101", 3);
    if (ok) buf_add(rest, end + 4, b.len - (size_t)(end + 4 - (char *)b.p));
    free(b.p);
    return ok;
}

static int ws_main(void *arg) {
    WsArgs a = *(WsArgs *)arg;
    free(arg);
    int fd = tcp_connect(a.host, a.port);
    Buf in = {0};
    if (fd < 0 || !handshake(fd, &a, &in)) {
        if (fd >= 0) close(fd);
        free(in.p);
        const char *why = fd < 0 ? "connect failed" : "handshake failed";
        push_event(a.gen, WS_EV_CLOSE, why, strlen(why), 1006);
        return 0;
    }
    push_event(a.gen, WS_EV_OPEN, NULL, 0, 0);
    Buf msg = {0};
    int msg_op = 0;
    int close_code = 1006;
    const char *close_why = "connection lost";
    char close_text[128] = "";
    for (;;) {
        SDL_LockMutex(ws_lock);
        bool stale = a.gen != ws_gen;
        Msg *out = stale ? NULL : send_head;
        if (!stale) send_head = send_tail = NULL;
        SDL_UnlockMutex(ws_lock);
        if (stale) {
            uint8_t code[2] = {1000 >> 8, 1000 & 255};
            send_frame(fd, 8, code, 2);
            break;
        }
        bool ok = true;
        while (out) {
            Msg *m = out;
            out = m->next;
            if (ok) ok = send_frame(fd, 2, m->data, m->len);
            free(m);
        }
        if (!ok) break;

        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, 10);
        if (r < 0 && errno != EINTR) break;
        if (r > 0) {
            char chunk[16384];
            ssize_t got = recv(fd, chunk, sizeof chunk, 0);
            if (got <= 0) break;
            buf_add(&in, chunk, (size_t)got);
        }
        // Parse every complete frame in the buffer.
        bool closed = false;
        while (in.len >= 2 && !closed) {
            unsigned char *f = in.p;
            int fin = f[0] & 0x80, op = f[0] & 15;
            bool masked = f[1] & 0x80;
            uint64_t len = f[1] & 127;
            size_t h = 2;
            if (len == 126) {
                if (in.len < 4) break;
                len = (uint64_t)f[2] << 8 | f[3];
                h = 4;
            } else if (len == 127) {
                if (in.len < 10) break;
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | f[2 + i];
                h = 10;
            }
            unsigned char mk[4] = {0};
            if (masked) {
                if (in.len < h + 4) break;
                memcpy(mk, f + h, 4);
                h += 4;
            }
            if (in.len < h + len) break;
            unsigned char *payload = f + h;
            if (masked)
                for (uint64_t i = 0; i < len; i++) payload[i] ^= mk[i & 3];
            if (op == 8) {
                close_code = len >= 2 ? payload[0] << 8 | payload[1] : 1005;
                if (len > 2) snprintf(close_text, sizeof close_text, "%.*s", (int)(len - 2), payload + 2);
                close_why = close_text;
                send_frame(fd, 8, payload, len >= 2 ? 2 : 0);
                closed = true;
            } else if (op == 9) {
                send_frame(fd, 10, payload, (size_t)len);
            } else if (op == 0 || op == 1 || op == 2) {
                if (op != 0) {
                    msg.len = 0;
                    msg_op = op;
                }
                buf_add(&msg, payload, (size_t)len);
                if (fin && msg_op) {
                    push_event(a.gen, WS_EV_MESSAGE, msg.p, msg.len, msg_op);
                    msg.len = 0;
                    msg_op = 0;
                }
            }
            buf_drop(&in, h + (size_t)len);
        }
        if (closed) break;
    }
    close(fd);
    free(in.p);
    free(msg.p);
    push_event(a.gen, WS_EV_CLOSE, close_why, strlen(close_why), close_code);
    return 0;
}

void ws_open(const char *host, int port, const char *path) {
    if (!ws_lock) {
        ws_lock = SDL_CreateMutex();
        srand((unsigned)SDL_GetTicks() ^ (unsigned)(uintptr_t)&host);
    }
    WsArgs *a = calloc(1, sizeof *a);
    SDL_LockMutex(ws_lock);
    a->gen = ++ws_gen;
    clear_queues();
    SDL_UnlockMutex(ws_lock);
    snprintf(a->host, sizeof a->host, "%s", host);
    snprintf(a->path, sizeof a->path, "%s", path);
    a->port = port;
    SDL_DetachThread(SDL_CreateThreadWithStackSize(ws_main, "ws", 256 * 1024, a));
}

void ws_send(const void *data, size_t len) {
    if (!ws_lock) return;
    Msg *m = malloc(sizeof *m + len);
    m->next = NULL;
    m->len = len;
    memcpy(m->data, data, len);
    SDL_LockMutex(ws_lock);
    if (send_tail) send_tail->next = m;
    else send_head = m;
    send_tail = m;
    SDL_UnlockMutex(ws_lock);
}

void ws_close(void) {
    if (!ws_lock) return;
    SDL_LockMutex(ws_lock);
    ws_gen++;
    clear_queues();
    SDL_UnlockMutex(ws_lock);
}

bool ws_poll(WsEvent *ev) {
    if (!ws_lock) return false;
    SDL_LockMutex(ws_lock);
    EvNode *n = ev_head;
    if (n) {
        ev_head = n->next;
        if (!ev_head) ev_tail = NULL;
    }
    SDL_UnlockMutex(ws_lock);
    if (!n) return false;
    *ev = n->ev;
    free(n);
    return true;
}
