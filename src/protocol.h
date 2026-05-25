#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define RPC_MAGIC 0x52504301

enum {
    MSG_REQUEST  = 1,
    MSG_RESPONSE = 2,
    MSG_ERROR    = 3,
};

typedef struct {
    uint32_t msg_id;
    uint8_t type;
    char *method;
    uint16_t method_len;
    void *payload;
    uint32_t payload_len;
} rpc_msg_t;

/* Serialize message into buffer. Returns total length, or -1 on error.
   Caller must free *out. */
int protocol_encode(const rpc_msg_t *msg, uint8_t **out);

/* Deserialize from buffer of given length. Returns 0 on success.
   Caller must free msg->method and msg->payload. */
int protocol_decode(const uint8_t *buf, int len, rpc_msg_t *msg);

void rpc_msg_free(rpc_msg_t *msg);

#endif
