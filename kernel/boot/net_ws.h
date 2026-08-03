/*
 * Zeos — WebSocket client (RFC 6455).
 *
 * Layers on the existing TCP (tcp_open/tcp_send_on/tcp_recv_on). Does the
 * HTTP Upgrade handshake (Sec-WebSocket-Key / -Accept verified via SHA-1 +
 * base64), then RFC 6455 frames: client->server frames are masked, text +
 * binary + ping/pong/close opcodes. Plain ws:// only (wss:// would layer on
 * net_tls, future work).
 */
#ifndef ZEOS_NET_WS_H
#define ZEOS_NET_WS_H

#include <stdint.h>
#include "net_tcp.h"

#define WS_MAX_FRAME  4096

/* Opcodes (RFC 6455 §5.2). */
#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BIN    0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

typedef struct {
    tcp_handle_t tcp;
    int          open;        /* 1 = handshake completed */
    uint8_t      rx[WS_MAX_FRAME];
} ws_conn_t;

/*
 * Connect + handshake. Resolves `host`, opens TCP to `port` (default 80 if 0),
 * sends the Upgrade request for `path`, reads the 101 response and verifies
 * Sec-WebSocket-Accept. Fills `out` and returns 0 on success, negative on error.
 */
int ws_connect(ws_conn_t *out, const char *host, const char *path,
               uint16_t port);

/* Send a text frame (masked). Returns 0 on success. */
int ws_send_text(ws_conn_t *ws, const char *data, uint32_t len);

/*
 * Receive one data frame. Returns payload length copied into `buf` (>=0), or
 * negative on error/close. Automatically answers ping with pong and skips
 * control frames, returning the next data frame's payload. `opcode_out` (if
 * non-NULL) gets the data opcode (WS_OP_TEXT/WS_OP_BIN).
 */
int ws_recv(ws_conn_t *ws, void *buf, uint32_t max_len, int *opcode_out);

/* Send a close frame and tear down the TCP connection. */
void ws_close(ws_conn_t *ws);

#endif /* ZEOS_NET_WS_H */
