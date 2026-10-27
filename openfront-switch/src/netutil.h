// Helpers shared by the network code (net.c, lanhost.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    unsigned char *p;
    size_t len, cap;
} NetBuf;

/** Appends n bytes (keeps a NUL after them). */
void net_buf_add(NetBuf *b, const void *data, size_t n);
/** Drops the first n bytes. */
void net_buf_drop(NetBuf *b, size_t n);
/** send() until everything went out; false on error. */
bool net_send_all(int fd, const void *buf, size_t len);
/** Standard base64 of n bytes into out (4 * ceil(n / 3) + 1 bytes). */
void net_base64(const unsigned char *in, size_t n, char *out);
