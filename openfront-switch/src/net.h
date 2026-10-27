// Networking for LAN multiplayer: plain HTTP requests and one WebSocket, over
// BSD sockets (libnx's on the Switch). Everything runs on background threads;
// callers poll for results and events.
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Brings up the socket service (Switch) once; safe to call again. */
bool net_init(void);
void net_shutdown(void);

/** Splits "host:port" (port optional, default `def_port`). */
bool net_parse_addr(const char *addr, char *host, size_t host_len, int *port, int def_port);

/**
 * Starts an HTTP/1.1 request on its own thread; returns its id (> 0).
 * `bearer` and `body` (JSON) may be NULL.
 */
int http_request(const char *method, const char *host, int port, const char *path, const char *bearer,
                 const char *body);
/**
 * True once request `id` has finished: *status is the HTTP status (0 on a
 * network error) and *body the response (caller frees, may be NULL). The
 * request is forgotten after that.
 */
bool http_result(int id, int *status, char **body);

typedef enum {
    WS_EV_OPEN,
    WS_EV_MESSAGE,
    WS_EV_CLOSE,
} WsEventKind;

typedef struct {
    WsEventKind kind;
    unsigned char *data;  // WS_EV_MESSAGE: the binary frame; WS_EV_CLOSE: the reason text
    size_t len;
    int code;  // WS_EV_CLOSE: close code (1006 when the connection just dropped)
} WsEvent;

/** Opens the WebSocket ws://host:port/path, dropping any previous one. */
void ws_open(const char *host, int port, const char *path);
/** Queues a binary message. */
void ws_send(const void *data, size_t len);
void ws_close(void);
/** Pops the next event into *ev (ev->data: caller frees); false if none. */
bool ws_poll(WsEvent *ev);
