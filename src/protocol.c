#include "protocol.h"
#include "compat.h"
#include <stdlib.h>
#include <string.h>

int protocol_encode(const rpc_msg_t *msg, uint8_t **out) {
    int total = 4 + 4 + 1 + 2 + msg->method_len + 4 + msg->payload_len;
    uint8_t *buf = malloc(total);
    if (!buf) return -1;
    uint8_t *p = buf;

    uint32_t magic = htonl(RPC_MAGIC);
    memcpy(p, &magic, 4); p += 4;

    uint32_t id = htonl(msg->msg_id);
    memcpy(p, &id, 4); p += 4;

    *p++ = msg->type;

    uint16_t mlen = htons(msg->method_len);
    memcpy(p, &mlen, 2); p += 2;

    if (msg->method_len) { memcpy(p, msg->method, msg->method_len); p += msg->method_len; }

    uint32_t plen = htonl(msg->payload_len);
    memcpy(p, &plen, 4); p += 4;

    if (msg->payload_len) { memcpy(p, msg->payload, msg->payload_len); p += msg->payload_len; }

    *out = buf;
    return total;
}

int protocol_decode(const uint8_t *buf, int len, rpc_msg_t *msg) {
    if (len < 15) return -1;
    const uint8_t *p = buf;

    uint32_t magic;
    memcpy(&magic, p, 4); p += 4;
    if (ntohl(magic) != RPC_MAGIC) return -1;

    uint32_t id;
    memcpy(&id, p, 4); p += 4;
    msg->msg_id = ntohl(id);

    msg->type = *p++;

    uint16_t mlen;
    memcpy(&mlen, p, 2); p += 2;
    msg->method_len = ntohs(mlen);

    if (len < 15 + msg->method_len) return -1;

    if (msg->method_len) {
        msg->method = malloc(msg->method_len + 1);
        memcpy(msg->method, p, msg->method_len);
        msg->method[msg->method_len] = '\0';
        p += msg->method_len;
    } else {
        msg->method = NULL;
    }

    uint32_t plen;
    memcpy(&plen, p, 4); p += 4;
    msg->payload_len = ntohl(plen);

    if (len < 15 + msg->method_len + (int)msg->payload_len) {
        free(msg->method);
        return -1;
    }

    if (msg->payload_len) {
        msg->payload = malloc(msg->payload_len);
        memcpy(msg->payload, p, msg->payload_len);
    } else {
        msg->payload = NULL;
    }

    return 0;
}

void rpc_msg_free(rpc_msg_t *msg) {
    free(msg->method);
    free(msg->payload);
}
