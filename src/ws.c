#include "ws.h"
#include "compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Minimal SHA-1 implementation */
static void sha1(const uint8_t *data, int len, uint8_t out[20]) {
    uint32_t h0=0x67452301, h1=0xEFCDAB89, h2=0x98BADCFE, h3=0x10325476, h4=0xC3D2E1F0;
    int ml = len * 8;
    int padded_len = ((len + 8) / 64 + 1) * 64;
    uint8_t *msg = calloc(1, padded_len);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    msg[padded_len-4] = (ml >> 24) & 0xff;
    msg[padded_len-3] = (ml >> 16) & 0xff;
    msg[padded_len-2] = (ml >> 8) & 0xff;
    msg[padded_len-1] = ml & 0xff;

    for (int chunk = 0; chunk < padded_len; chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (msg[chunk+i*4]<<24)|(msg[chunk+i*4+1]<<16)|(msg[chunk+i*4+2]<<8)|msg[chunk+i*4+3];
        for (int i = 16; i < 80; i++) {
            uint32_t t = w[i-3]^w[i-8]^w[i-14]^w[i-16];
            w[i] = (t<<1)|(t>>31);
        }
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i<20) { f=(b&c)|((~b)&d); k=0x5A827999; }
            else if (i<40) { f=b^c^d; k=0x6ED9EBA1; }
            else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else { f=b^c^d; k=0xCA62C1D6; }
            uint32_t tmp = ((a<<5)|(a>>27)) + f + e + k + w[i];
            e=d; d=c; c=(b<<30)|(b>>2); b=a; a=tmp;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }
    free(msg);
    uint32_t h[5] = {h0,h1,h2,h3,h4};
    for (int i = 0; i < 5; i++) {
        out[i*4]=(h[i]>>24)&0xff; out[i*4+1]=(h[i]>>16)&0xff;
        out[i*4+2]=(h[i]>>8)&0xff; out[i*4+3]=h[i]&0xff;
    }
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *in, int len, char *out) {
    int i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = b64[in[i]>>2];
        out[j++] = b64[((in[i]&3)<<4)|(in[i+1]>>4)];
        out[j++] = b64[((in[i+1]&0xf)<<2)|(in[i+2]>>6)];
        out[j++] = b64[in[i+2]&0x3f];
    }
    if (i < len) {
        out[j++] = b64[in[i]>>2];
        if (i+1 < len) {
            out[j++] = b64[((in[i]&3)<<4)|(in[i+1]>>4)];
            out[j++] = b64[((in[i+1]&0xf)<<2)];
        } else {
            out[j++] = b64[((in[i]&3)<<4)];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
    return j;
}

void ws_buf_init(ws_buf_t *b) { memset(b, 0, sizeof(*b)); }

void ws_buf_append(ws_buf_t *b, const void *data, int len) {
    if (b->len + len > b->cap) {
        int nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->len + len) nc *= 2;
        b->data = realloc(b->data, nc);
        b->cap = nc;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

void ws_buf_consume(ws_buf_t *b, int n) {
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

void ws_buf_free(ws_buf_t *b) { free(b->data); memset(b, 0, sizeof(*b)); }

static const char *WS_GUID = "258EAFA5-E914-47DA-95CA-5AB5B0D3E655";

static char *find_header(const char *headers, const char *name) {
    const char *p = compat_strcasestr(headers, name);
    if (!p) return NULL;
    p += strlen(name);
    while (*p == ' ' || *p == ':') p++;
    const char *end = strstr(p, "\r\n");
    if (!end) return NULL;
    int len = (int)(end - p);
    char *val = malloc(len + 1);
    memcpy(val, p, len);
    val[len] = '\0';
    return val;
}

int ws_server_handshake(const uint8_t *buf, int len, uint8_t **resp, int *resp_len) {
    /* Find end of HTTP headers */
    const char *end = compat_memmem(buf, len, "\r\n\r\n", 4);
    if (!end) return 0;
    int hdr_len = (int)(end - (const char *)buf) + 4;

    char *key = find_header((const char *)buf, "Sec-WebSocket-Key");
    if (!key) return -1;

    /* Compute accept */
    int klen = (int)strlen(key);
    int glen = (int)strlen(WS_GUID);
    char *concat = malloc(klen + glen + 1);
    memcpy(concat, key, klen);
    memcpy(concat + klen, WS_GUID, glen + 1);
    free(key);

    uint8_t hash[20];
    sha1((uint8_t *)concat, klen + glen, hash);
    free(concat);

    char accept[30];
    base64_encode(hash, 20, accept);

    char response[512];
    int rlen = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);

    *resp = malloc(rlen);
    memcpy(*resp, response, rlen);
    *resp_len = rlen;
    return hdr_len;
}

int ws_client_handshake(const char *host, int port, const char *path,
                        uint8_t **req, int *req_len, char key_out[25]) {
    /* Generate random 16-byte key */
    uint8_t raw_key[16];
    srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)req);
    for (int i = 0; i < 16; i++) raw_key[i] = rand() & 0xff;
    base64_encode(raw_key, 16, key_out);

    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        path, host, port, key_out);

    *req = malloc(len);
    memcpy(*req, buf, len);
    *req_len = len;
    return 0;
}

int ws_client_validate(const uint8_t *buf, int len, const char key[25]) {
    const char *end = compat_memmem(buf, len, "\r\n\r\n", 4);
    if (!end) return 0;
    int hdr_len = (int)(end - (const char *)buf) + 4;

    /* Verify accept header */
    int klen = (int)strlen(key);
    int glen = (int)strlen(WS_GUID);
    char *concat = malloc(klen + glen + 1);
    memcpy(concat, key, klen);
    memcpy(concat + klen, WS_GUID, glen + 1);

    uint8_t hash[20];
    sha1((uint8_t *)concat, klen + glen, hash);
    free(concat);

    char expected[30];
    base64_encode(hash, 20, expected);

    char *got = find_header((const char *)buf, "Sec-WebSocket-Accept");
    if (!got || strcmp(got, expected) != 0) { free(got); return -1; }
    free(got);
    return hdr_len;
}

int ws_frame_encode(int opcode, const uint8_t *payload, int payload_len,
                    int mask, uint8_t **out, int *out_len) {
    int hdr_size = 2;
    if (payload_len > 65535) hdr_size += 8;
    else if (payload_len > 125) hdr_size += 2;
    if (mask) hdr_size += 4;

    int total = hdr_size + payload_len;
    uint8_t *frame = malloc(total);
    uint8_t *p = frame;

    *p++ = 0x80 | (opcode & 0x0f); /* FIN + opcode */

    uint8_t mask_bit = mask ? 0x80 : 0;
    if (payload_len > 65535) {
        *p++ = mask_bit | 127;
        uint64_t l = payload_len;
        for (int i = 7; i >= 0; i--) *p++ = (l >> (i*8)) & 0xff;
    } else if (payload_len > 125) {
        *p++ = mask_bit | 126;
        *p++ = (payload_len >> 8) & 0xff;
        *p++ = payload_len & 0xff;
    } else {
        *p++ = mask_bit | payload_len;
    }

    if (mask) {
        uint8_t mask_key[4];
        for (int i = 0; i < 4; i++) mask_key[i] = rand() & 0xff;
        memcpy(p, mask_key, 4); p += 4;
        for (int i = 0; i < payload_len; i++)
            *p++ = payload[i] ^ mask_key[i & 3];
    } else {
        memcpy(p, payload, payload_len);
    }

    *out = frame;
    *out_len = total;
    return payload_len;
}

int ws_frame_decode(const uint8_t *buf, int len, int *opcode,
                    uint8_t **payload, int *consumed) {
    if (len < 2) return 0;
    *opcode = buf[0] & 0x0f;
    int masked = (buf[1] >> 7) & 1;
    uint64_t plen = buf[1] & 0x7f;
    int offset = 2;

    if (plen == 126) {
        if (len < 4) return 0;
        plen = (buf[2] << 8) | buf[3];
        offset = 4;
    } else if (plen == 127) {
        if (len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | buf[2+i];
        offset = 10;
    }

    if (masked) {
        if (len < offset + 4 + (int)plen) return 0;
        uint8_t *mask_key = (uint8_t *)buf + offset;
        offset += 4;
        *payload = malloc(plen);
        for (uint64_t i = 0; i < plen; i++)
            (*payload)[i] = buf[offset + i] ^ mask_key[i & 3];
    } else {
        if (len < offset + (int)plen) return 0;
        *payload = malloc(plen);
        memcpy(*payload, buf + offset, plen);
    }

    *consumed = offset + (int)plen;
    return (int)plen;
}
