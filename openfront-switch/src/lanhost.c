#include "lanhost.h"

#include <SDL.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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

#include "cJSON.h"
#include "netutil.h"
#include "quickjs.h"

#define MAX_CONNS 16
#define ANNOUNCE_MS 1000

// --- SHA-1 (for Sec-WebSocket-Accept) ----------------------------------------------------

static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

static void sha1(const unsigned char *data, size_t len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char *m = calloc(total, 1);
    memcpy(m, data, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 | (uint32_t)m[off + 4 * i + 2] << 8 |
                   m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    free(m);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}

// --- Local address ------------------------------------------------------------------------

static uint32_t local_addr(void) {
#ifdef __SWITCH__
    return (uint32_t)gethostid();  // network order, 0 without a connection
#else
    // The address a UDP socket would leave from (nothing is sent).
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in to = {0}, me = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);
    socklen_t len = sizeof me;
    uint32_t a = 0;
    if (connect(fd, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(fd, (struct sockaddr *)&me, &len) == 0)
        a = me.sin_addr.s_addr;
    close(fd);
    return a;
#endif
}

void net_local_ip(char *out, size_t n) {
    uint32_t a = local_addr();
    out[0] = 0;
    if (a == 0 || a == htonl(INADDR_LOOPBACK)) return;
    struct in_addr in = {a};
    inet_ntop(AF_INET, &in, out, (socklen_t)n);
}

// --- The host's game server ---------------------------------------------------------------

typedef struct {
    int fd;  // -1 = free
    bool ws;
    bool closing;
    NetBuf in, msg;
    int msg_op;
} Conn;

static SDL_Thread *thread;
static SDL_sem *ready;
static volatile bool stop_flag;
static bool running;
static Conn conns[MAX_CONNS];
static int listen_fd = -1, udp_fd = -1, port;
static char romfs_root[256], creator[64], lobby_id[16];
static JSRuntime *srt;
static JSContext *sctx;
static JSValue server_obj;

static bool send_frame(int fd, int op, const void *data, size_t len) {
    unsigned char head[10];
    size_t h = 0;
    head[h++] = (unsigned char)(0x80 | op);
    if (len < 126) {
        head[h++] = (unsigned char)len;
    } else if (len < 65536) {
        head[h++] = 126;
        head[h++] = (unsigned char)(len >> 8);
        head[h++] = (unsigned char)len;
    } else {
        head[h++] = 127;
        for (int i = 7; i >= 0; i--) head[h++] = (unsigned char)((uint64_t)len >> (8 * i));
    }
    return net_send_all(fd, head, h) && (len == 0 || net_send_all(fd, data, len));
}

static void drop_conn(int i, bool tell_js);

static JSValue js_log(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *s = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    if (s) printf("[server] %s\n", s);
    JS_FreeCString(c, s);
    return JS_UNDEFINED;
}

static JSValue js_now(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(c, (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency());
}

static JSValue js_send(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t i = -1;
    size_t len = 0;
    if (argc < 2 || JS_ToInt32(c, &i, argv[0]) || i < 0 || i >= MAX_CONNS) return JS_UNDEFINED;
    uint8_t *data = JS_GetArrayBuffer(c, &len, argv[1]);
    if (data && conns[i].fd >= 0 && conns[i].ws && !conns[i].closing && !send_frame(conns[i].fd, 2, data, len))
        conns[i].closing = true;
    return JS_UNDEFINED;
}

static JSValue js_close(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t i = -1;
    if (argc > 0 && !JS_ToInt32(c, &i, argv[0]) && i >= 0 && i < MAX_CONNS && conns[i].fd >= 0) {
        uint8_t code[2] = {1000 >> 8, 1000 & 255};
        if (conns[i].ws) send_frame(conns[i].fd, 8, code, 2);
        conns[i].closing = true;
    }
    return JS_UNDEFINED;
}

static void js_exception(void) {
    JSValue e = JS_GetException(sctx);
    const char *s = JS_ToCString(sctx, e);
    fprintf(stderr, "[server] exception: %s\n", s ? s : "?");
    JS_FreeCString(sctx, s);
    JS_FreeValue(sctx, e);
}

/** SERVER[name](...args); the result (caller frees). */
static JSValue call(const char *name, int argc, JSValue *argv) {
    JSValue fn = JS_GetPropertyStr(sctx, server_obj, name);
    JSValue r = JS_Call(sctx, fn, server_obj, argc, argv);
    JS_FreeValue(sctx, fn);
    for (int i = 0; i < argc; i++) JS_FreeValue(sctx, argv[i]);
    if (JS_IsException(r)) js_exception();
    JSContext *jc;
    while (JS_ExecutePendingJob(srt, &jc) > 0) {
    }
    return r;
}

static char *call_str(const char *name, int argc, JSValue *argv) {
    JSValue r = call(name, argc, argv);
    const char *s = JS_ToCString(sctx, r);
    char *out = strdup(s ? s : "");
    JS_FreeCString(sctx, s);
    JS_FreeValue(sctx, r);
    return out;
}

static bool load_server(void) {
    char path[300];
    snprintf(path, sizeof path, "%sserver.js", romfs_root);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)len + 1);
    size_t got = fread(src, 1, (size_t)len, f);
    fclose(f);
    src[got] = 0;
    srt = JS_NewRuntime();
    JS_SetMaxStackSize(srt, 1024 * 1024);
    sctx = JS_NewContext(srt);
    JSValue global = JS_GetGlobalObject(sctx);
    JSValue host = JS_NewObject(sctx);
    JS_SetPropertyStr(sctx, host, "log", JS_NewCFunction(sctx, js_log, "log", 1));
    JS_SetPropertyStr(sctx, host, "now", JS_NewCFunction(sctx, js_now, "now", 0));
    JS_SetPropertyStr(sctx, host, "send", JS_NewCFunction(sctx, js_send, "send", 2));
    JS_SetPropertyStr(sctx, host, "close", JS_NewCFunction(sctx, js_close, "close", 1));
    JS_SetPropertyStr(sctx, global, "host", host);
    JSValue r = JS_Eval(sctx, src, got, "server.js", JS_EVAL_TYPE_GLOBAL);
    free(src);
    bool ok = !JS_IsException(r);
    if (!ok) js_exception();
    JS_FreeValue(sctx, r);
    server_obj = JS_GetPropertyStr(sctx, global, "SERVER");
    JS_FreeValue(sctx, global);
    return ok && JS_IsObject(server_obj);
}

static void unload_server(void) {
    if (!sctx) return;
    JS_FreeValue(sctx, server_obj);
    JS_FreeContext(sctx);
    JS_FreeRuntime(srt);
    sctx = NULL;
    srt = NULL;
}

static void drop_conn(int i, bool tell_js) {
    Conn *c = &conns[i];
    if (c->fd < 0) return;
    if (tell_js && c->ws) {
        JSValue a[1] = {JS_NewInt32(sctx, i)};
        JS_FreeValue(sctx, call("close", 1, a));
    }
    close(c->fd);
    free(c->in.p);
    free(c->msg.p);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static void http_reply(int fd, int status, const char *body) {
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                     status, status == 200 ? "OK" : "Not Found", strlen(body));
    net_send_all(fd, head, (size_t)n);
    net_send_all(fd, body, strlen(body));
}

/** A request on a fresh connection: the WebSocket upgrade, or GET /api/game/:id/exists. */
static void handle_http(int i) {
    Conn *c = &conns[i];
    char *end = strstr((char *)c->in.p, "\r\n\r\n");
    if (!end) {
        if (c->in.len > 8192) c->closing = true;
        return;
    }
    *end = 0;
    size_t used = (size_t)(end + 4 - (char *)c->in.p);
    char lower[8192];
    snprintf(lower, sizeof lower, "%s", (char *)c->in.p);
    for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
    char *key = strstr(lower, "\nsec-websocket-key:");
    if (key) {
        // The key's value from the original (case-sensitive) text.
        const char *v = (char *)c->in.p + (key - lower) + 19;
        while (*v == ' ') v++;
        char k[64];
        size_t kl = strcspn(v, "\r\n");
        snprintf(k, sizeof k, "%.*s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", (int)(kl < 30 ? kl : 30), v);
        unsigned char digest[20];
        sha1((unsigned char *)k, strlen(k), digest);
        char accept[32];
        net_base64(digest, 20, accept);
        char resp[256];
        int n = snprintf(resp, sizeof resp,
                         "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: %s\r\n\r\n",
                         accept);
        net_send_all(c->fd, resp, (size_t)n);
        c->ws = true;
        net_buf_drop(&c->in, used);
        return;
    }
    // GET .../api/game/<id>/exists
    char *g = strstr(lower, "/api/game/");
    char body[256] = "{\"error\":\"not found\"}";
    int status = 404;
    if (!strncmp(lower, "get ", 4) && g) {
        const char *id = (char *)c->in.p + (g - lower) + 10;
        char idbuf[32];
        snprintf(idbuf, sizeof idbuf, "%.*s", (int)strcspn(id, "/ ?"), id);
        JSValue a[1] = {JS_NewString(sctx, idbuf)};
        char *r = call_str("exists", 1, a);
        snprintf(body, sizeof body, "%s", r);
        free(r);
        status = 200;
    }
    http_reply(c->fd, status, body);
    c->closing = true;
}

/** Complete client frames: binary messages go to the server, pings get pongs. */
static void handle_frames(int i) {
    Conn *c = &conns[i];
    while (c->in.len >= 2 && !c->closing) {
        unsigned char *f = c->in.p;
        int fin = f[0] & 0x80, op = f[0] & 15;
        bool masked = f[1] & 0x80;
        uint64_t len = f[1] & 127;
        size_t h = 2;
        if (len == 126) {
            if (c->in.len < 4) return;
            len = (uint64_t)f[2] << 8 | f[3];
            h = 4;
        } else if (len == 127) {
            if (c->in.len < 10) return;
            len = 0;
            for (int k = 0; k < 8; k++) len = len << 8 | f[2 + k];
            h = 10;
        }
        if (len > (1 << 20)) {
            c->closing = true;
            return;
        }
        unsigned char mk[4] = {0};
        if (masked) {
            if (c->in.len < h + 4) return;
            memcpy(mk, f + h, 4);
            h += 4;
        }
        if (c->in.len < h + len) return;
        unsigned char *payload = f + h;
        for (uint64_t k = 0; masked && k < len; k++) payload[k] ^= mk[k & 3];
        if (op == 8) {
            send_frame(c->fd, 8, payload, len >= 2 ? 2 : 0);
            c->closing = true;
        } else if (op == 9) {
            send_frame(c->fd, 10, payload, (size_t)len);
        } else if (op <= 2) {
            if (op != 0) c->msg.len = 0, c->msg_op = op;
            net_buf_add(&c->msg, payload, (size_t)len);
            if (fin && c->msg_op == 2) {
                JSValue a[2] = {JS_NewInt32(sctx, i), JS_NewArrayBufferCopy(sctx, c->msg.p, c->msg.len)};
                JS_FreeValue(sctx, call("message", 2, a));
            }
            if (fin) c->msg.len = 0, c->msg_op = 0;
        }
        net_buf_drop(&c->in, h + (size_t)len);
    }
}

static void announce(void) {
    JSValue a[1] = {JS_NewInt32(sctx, port)};
    char *msg = call_str("announce", 1, a);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(LAN_DISCOVERY_PORT);
    // Everyone on the network, the /24 around us (some routers drop the
    // limited broadcast), and this machine (two copies on one PC).
    uint32_t me = local_addr();
    uint32_t targets[3] = {htonl(INADDR_BROADCAST), me ? (me | htonl(0xff)) : 0, htonl(INADDR_LOOPBACK)};
    for (int t = 0; t < 3; t++) {
        if (!targets[t]) continue;
        to.sin_addr.s_addr = targets[t];
        sendto(udp_fd, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof to);
    }
    free(msg);
}

static int host_main(void *unused) {
    (void)unused;
    bool ok = load_server();
    if (ok) {
        JSValue init = JS_NewObject(sctx);
        JS_SetPropertyStr(sctx, init, "creatorToken", JS_NewString(sctx, creator));
        JS_SetPropertyStr(sctx, init, "version", JS_NewString(sctx, OPENFRONT_VERSION));
        JSValue a[1] = {init};
        char *id = call_str("init", 1, a);
        snprintf(lobby_id, sizeof lobby_id, "%s", id);
        free(id);
    }
    // The TCP listener: the first free port from LAN_PORT.
    listen_fd = ok ? socket(AF_INET, SOCK_STREAM, 0) : -1;
    port = 0;
    if (listen_fd >= 0) {
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        for (int p = LAN_PORT; p < LAN_PORT + 10 && !port; p++) {
            struct sockaddr_in a = {0};
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)p);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(listen_fd, 8) == 0) port = p;
        }
    }
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd >= 0) {
        int one = 1;
        setsockopt(udp_fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    }
    running = port != 0;
    SDL_SemPost(ready);
    if (!running) {
        if (listen_fd >= 0) close(listen_fd);
        if (udp_fd >= 0) close(udp_fd);
        listen_fd = udp_fd = -1;
        unload_server();
        return 1;
    }

    Uint32 last_announce = 0;
    while (!stop_flag) {
        struct pollfd pfd[MAX_CONNS + 1];
        int map[MAX_CONNS + 1], n = 0;
        pfd[n] = (struct pollfd){listen_fd, POLLIN, 0};
        map[n++] = -1;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (conns[i].fd < 0) continue;
            pfd[n] = (struct pollfd){conns[i].fd, POLLIN, 0};
            map[n++] = i;
        }
        int r = poll(pfd, (nfds_t)n, 5);
        if (r > 0) {
            for (int k = 0; k < n; k++) {
                if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                if (map[k] < 0) {
                    int fd = accept(listen_fd, NULL, NULL);
                    if (fd < 0) continue;
                    int slot = -1;
                    for (int i = 0; i < MAX_CONNS && slot < 0; i++)
                        if (conns[i].fd < 0) slot = i;
                    if (slot < 0) {
                        close(fd);
                        continue;
                    }
                    int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    memset(&conns[slot], 0, sizeof conns[slot]);
                    conns[slot].fd = fd;
                    continue;
                }
                int i = map[k];
                char buf[16384];
                ssize_t got = recv(conns[i].fd, buf, sizeof buf, 0);
                if (got <= 0) {
                    drop_conn(i, true);
                    continue;
                }
                net_buf_add(&conns[i].in, buf, (size_t)got);
                if (!conns[i].ws) handle_http(i);
                if (conns[i].ws) handle_frames(i);
            }
        }
        for (int i = 0; i < MAX_CONNS; i++)
            if (conns[i].fd >= 0 && conns[i].closing) drop_conn(i, true);
        JS_FreeValue(sctx, call("poll", 0, NULL));
        Uint32 now = SDL_GetTicks();
        if (now - last_announce >= ANNOUNCE_MS) {
            last_announce = now;
            announce();
        }
    }
    for (int i = 0; i < MAX_CONNS; i++) drop_conn(i, false);
    close(listen_fd);
    close(udp_fd);
    listen_fd = udp_fd = -1;
    unload_server();
    return 0;
}

int lan_host_start(const char *romfs, const char *creator_token, char *game_id, size_t n) {
    lan_host_stop();
    snprintf(romfs_root, sizeof romfs_root, "%s", romfs);
    snprintf(creator, sizeof creator, "%s", creator_token);
    for (int i = 0; i < MAX_CONNS; i++) conns[i].fd = -1;
    stop_flag = false;
    if (!ready) ready = SDL_CreateSemaphore(0);
    thread = SDL_CreateThreadWithStackSize(host_main, "lanhost", 2 * 1024 * 1024, NULL);
    if (!thread) return 0;
    SDL_SemWait(ready);
    if (!running) {
        SDL_WaitThread(thread, NULL);
        thread = NULL;
        return 0;
    }
    snprintf(game_id, n, "%s", lobby_id);
    return port;
}

void lan_host_stop(void) {
    if (!thread) return;
    stop_flag = true;
    SDL_WaitThread(thread, NULL);
    thread = NULL;
    running = false;
}

bool lan_host_running(void) { return running; }

// --- Finding lobbies --------------------------------------------------------------------------

#define MAX_GAMES 16

static int disc_fd = -1;
static LanGame games[MAX_GAMES];
static int ngames;

void lan_discover_start(void) {
    if (disc_fd >= 0) return;
    disc_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (disc_fd < 0) return;
    int one = 1;
    setsockopt(disc_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(disc_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(LAN_DISCOVERY_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(disc_fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(disc_fd);
        disc_fd = -1;
        return;
    }
    fcntl(disc_fd, F_SETFL, fcntl(disc_fd, F_GETFL, 0) | O_NONBLOCK);
    ngames = 0;
}

void lan_discover_stop(void) {
    if (disc_fd >= 0) close(disc_fd);
    disc_fd = -1;
    ngames = 0;
}

int lan_discover_list(LanGame *out, int max) {
    double now = (double)SDL_GetTicks();
    char buf[1024];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t got;
    while (disc_fd >= 0 && (got = recvfrom(disc_fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl)) > 0) {
        buf[got] = 0;
        fl = sizeof from;
        cJSON *j = cJSON_Parse(buf);
        const cJSON *of = cJSON_GetObjectItemCaseSensitive(j, "of");
        if (!cJSON_IsString(of) || strcmp(of->valuestring, "openfront-switch") != 0) {
            cJSON_Delete(j);
            continue;
        }
        LanGame g = {0};
        inet_ntop(AF_INET, &from.sin_addr, g.ip, sizeof g.ip);
#define STR(k, dst)                                                              \
    do {                                                                         \
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);                 \
        if (cJSON_IsString(v)) snprintf(dst, sizeof dst, "%s", v->valuestring); \
    } while (0)
        STR("id", g.id);
        STR("host", g.host);
        STR("map", g.map);
        STR("v", g.version);
#undef STR
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "port");
        g.port = cJSON_IsNumber(v) ? v->valueint : LAN_PORT;
        v = cJSON_GetObjectItemCaseSensitive(j, "players");
        g.players = cJSON_IsNumber(v) ? v->valueint : 0;
        g.started = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "started"));
        g.seen_ms = now;
        cJSON_Delete(j);
        // The same lobby heard on several addresses (broadcast and loopback): keep one.
        int slot = -1;
        for (int i = 0; i < ngames; i++)
            if (!strcmp(games[i].id, g.id)) slot = i;
        if (slot >= 0) {
            if (!strcmp(g.ip, "127.0.0.1")) snprintf(g.ip, sizeof g.ip, "%s", games[slot].ip);
            games[slot] = g;
        } else if (ngames < MAX_GAMES) {
            games[ngames++] = g;
        }
    }
    // Lobbies not heard for 4 s are gone.
    int k = 0;
    for (int i = 0; i < ngames; i++)
        if (now - games[i].seen_ms < 4000) games[k++] = games[i];
    ngames = k;
    int n = ngames < max ? ngames : max;
    memcpy(out, games, sizeof(LanGame) * (size_t)n);
    return n;
}
