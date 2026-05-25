#ifndef WS_H
#define WS_H

#include <stdint.h>

#define WS_OP_TEXT   0x1
#define WS_OP_BIN    0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

typedef struct {
    uint8_t *data;
    int len;
    int cap;
} ws_buf_t;

void ws_buf_init(ws_buf_t *b);
void ws_buf_append(ws_buf_t *b, const void *data, int len);
void ws_buf_consume(ws_buf_t *b, int n);
void ws_buf_free(ws_buf_t *b);

/* Server-side: parse client handshake from buf, produce response into resp.
   Returns bytes consumed from buf, or 0 if incomplete, -1 on error. */
int ws_server_handshake(const uint8_t *buf, int len, uint8_t **resp, int *resp_len);

/* Client-side: produce handshake request. key_out receives the base64 key for validation. */
int ws_client_handshake(const char *host, int port, const char *path,
                        uint8_t **req, int *req_len, char key_out[25]);

/* Client-side: validate server response. Returns bytes consumed or 0/-1. */
int ws_client_validate(const uint8_t *buf, int len, const char key[25]);

/* Frame encoding. If mask is true, applies random mask. Returns frame in *out. */
int ws_frame_encode(int opcode, const uint8_t *payload, int payload_len,
                    int mask, uint8_t **out, int *out_len);

/* Frame decoding. Returns payload length, sets *opcode.
   Returns 0 if incomplete, -1 on error. *consumed = total frame bytes. */
int ws_frame_decode(const uint8_t *buf, int len, int *opcode,
                    uint8_t **payload, int *consumed);

#endif
