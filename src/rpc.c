#include "../include/rpc.h"
#include "compat.h"
#include "event_loop.h"
#include "protocol.h"
#include "ws.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_HANDLERS 128
#define MAX_PENDING 256
#define MAX_PEERS 256

typedef struct {
    char *method;
    rpc_handler_t handler;
} handler_entry_t;

typedef struct {
    uint32_t msg_id;
    rpc_callback_t cb;
    void *userdata;
    /* For sync calls */
    int done;
    int status;
    void *result;
    int result_len;
} pending_call_t;

struct rpc_peer {
    rpc_ctx_t *ctx;
    socket_t fd;
    int is_client;     /* 1 = we connected to them (mask frames) */
    int handshake_done;
    ws_buf_t recv_buf;
    ws_buf_t send_buf;
    char ws_key[25];   /* client handshake key */
    pending_call_t *pending[MAX_PENDING];
    int pending_count;
};

struct rpc_ctx {
    event_loop_t *el;
    socket_t listen_fd;
    int has_listen;
    handler_entry_t handlers[MAX_HANDLERS];
    int handler_count;
    uint32_t next_msg_id;
    rpc_peer_t *peers[MAX_PEERS];
    int peer_count;
};

static void peer_on_data(event_loop_t *el, socket_t fd, uint32_t events, void *data);

static rpc_peer_t *peer_create(rpc_ctx_t *ctx, socket_t fd, int is_client) {
    rpc_peer_t *p = calloc(1, sizeof(*p));
    p->ctx = ctx;
    p->fd = fd;
    p->is_client = is_client;
    ws_buf_init(&p->recv_buf);
    ws_buf_init(&p->send_buf);
    compat_set_nonblock(fd);
    el_add(ctx->el, fd, EL_READ, peer_on_data, p);
    ctx->peers[ctx->peer_count++] = p;
    return p;
}

static void peer_destroy(rpc_peer_t *p) {
    if (!p) return;
    el_del(p->ctx->el, p->fd);
    close_sock(p->fd);
    ws_buf_free(&p->recv_buf);
    ws_buf_free(&p->send_buf);
    for (int i = 0; i < p->pending_count; i++) free(p->pending[i]);
    free(p);
}

static void peer_send_raw(rpc_peer_t *p, const void *data, int len) {
    int sent = (int)send(p->fd, (const char *)data, len, MSG_NOSIGNAL_FLAG);
    if (sent < 0) sent = 0;
    if (sent < len) {
        ws_buf_append(&p->send_buf, (const uint8_t *)data + sent, len - sent);
        el_mod(p->ctx->el, p->fd, EL_READ | EL_WRITE);
    }
}

static void peer_send_ws(rpc_peer_t *p, const uint8_t *data, int len) {
    uint8_t *frame;
    int frame_len;
    ws_frame_encode(WS_OP_BIN, data, len, p->is_client, &frame, &frame_len);
    peer_send_raw(p, frame, frame_len);
    free(frame);
}

static void peer_send_rpc(rpc_peer_t *p, const rpc_msg_t *msg) {
    uint8_t *buf;
    int len = protocol_encode(msg, &buf);
    if (len > 0) {
        peer_send_ws(p, buf, len);
        free(buf);
    }
}

static void dispatch_request(rpc_peer_t *p, rpc_msg_t *msg) {
    rpc_ctx_t *ctx = p->ctx;
    rpc_handler_t handler = NULL;

    for (int i = 0; i < ctx->handler_count; i++) {
        if (strcmp(ctx->handlers[i].method, msg->method) == 0) {
            handler = ctx->handlers[i].handler;
            break;
        }
    }

    rpc_msg_t resp = {0};
    resp.msg_id = msg->msg_id;

    if (handler) {
        void *result = NULL;
        int result_len = 0;
        handler(p, msg->payload, msg->payload_len, &result, &result_len);
        resp.type = MSG_RESPONSE;
        resp.payload = result;
        resp.payload_len = result_len;
    } else {
        resp.type = MSG_ERROR;
        const char *err = "method not found";
        resp.payload = (void *)err;
        resp.payload_len = (uint32_t)strlen(err);
    }

    peer_send_rpc(p, &resp);
    if (resp.type == MSG_RESPONSE) free(resp.payload);
}

static void dispatch_response(rpc_peer_t *p, rpc_msg_t *msg) {
    for (int i = 0; i < p->pending_count; i++) {
        if (p->pending[i]->msg_id == msg->msg_id) {
            pending_call_t *pc = p->pending[i];
            if (pc->cb) {
                int status = (msg->type == MSG_ERROR) ? -1 : 0;
                pc->cb(status, msg->payload, msg->payload_len, pc->userdata);
                p->pending[i] = p->pending[--p->pending_count];
                free(pc);
            } else {
                /* sync call: leave pc in pending list; the rpc_call frame
                 * will free it after observing pc->done. */
                pc->done = 1;
                pc->status = (msg->type == MSG_ERROR) ? -1 : 0;
                if (msg->payload_len > 0) {
                    pc->result = malloc(msg->payload_len);
                    memcpy(pc->result, msg->payload, msg->payload_len);
                    pc->result_len = msg->payload_len;
                }
            }
            return;
        }
    }
}

static void process_ws_message(rpc_peer_t *p, uint8_t *data, int len) {
    rpc_msg_t msg = {0};
    if (protocol_decode(data, len, &msg) != 0) return;

    if (msg.type == MSG_REQUEST) {
        dispatch_request(p, &msg);
    } else {
        dispatch_response(p, &msg);
    }
    rpc_msg_free(&msg);
}

static void process_peer_data(rpc_peer_t *p) {
    if (!p->handshake_done) {
        if (p->is_client) {
            int n = ws_client_validate(p->recv_buf.data, p->recv_buf.len, p->ws_key);
            if (n <= 0) return;
            ws_buf_consume(&p->recv_buf, n);
            p->handshake_done = 1;
        } else {
            uint8_t *resp;
            int resp_len;
            int n = ws_server_handshake(p->recv_buf.data, p->recv_buf.len, &resp, &resp_len);
            if (n <= 0) return;
            ws_buf_consume(&p->recv_buf, n);
            peer_send_raw(p, resp, resp_len);
            free(resp);
            p->handshake_done = 1;
        }
    }

    while (p->recv_buf.len > 0) {
        int opcode, consumed;
        uint8_t *payload;
        int plen = ws_frame_decode(p->recv_buf.data, p->recv_buf.len, &opcode, &payload, &consumed);
        if (plen == 0 && consumed == 0) break;
        if (plen < 0) { ws_buf_consume(&p->recv_buf, 1); continue; }

        ws_buf_consume(&p->recv_buf, consumed);

        if (opcode == WS_OP_BIN || opcode == WS_OP_TEXT) {
            process_ws_message(p, payload, plen);
        } else if (opcode == WS_OP_PING) {
            uint8_t *frame;
            int frame_len;
            ws_frame_encode(WS_OP_PONG, payload, plen, p->is_client, &frame, &frame_len);
            peer_send_raw(p, frame, frame_len);
            free(frame);
        } else if (opcode == WS_OP_CLOSE) {
            free(payload);
            return;
        }
        free(payload);
    }
}

static void peer_on_data(event_loop_t *el, socket_t fd, uint32_t events, void *data) {
    rpc_peer_t *p = data;

    if (events & EL_WRITE) {
        if (p->send_buf.len > 0) {
            int n = (int)send(fd, (const char *)p->send_buf.data, p->send_buf.len, MSG_NOSIGNAL_FLAG);
            if (n > 0) ws_buf_consume(&p->send_buf, n);
            if (p->send_buf.len == 0) el_mod(el, fd, EL_READ);
        }
    }

    if (events & EL_READ) {
        char buf[8192];
        int n = (int)recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            /* Peer disconnected -- leave the peer; teardown happens at rpc_destroy. */
            return;
        }
        ws_buf_append(&p->recv_buf, buf, n);
        process_peer_data(p);
    }
}

static void accept_cb(event_loop_t *el, socket_t fd, uint32_t events, void *data) {
    (void)el; (void)events;
    rpc_ctx_t *ctx = data;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    socket_t client_fd = accept(fd, (struct sockaddr *)&addr, &alen);
    if (client_fd == INVALID_SOCK) return;
    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt));
    if (ctx->peer_count < MAX_PEERS) {
        peer_create(ctx, client_fd, 0);
    } else {
        close_sock(client_fd);
    }
}

/* Public API */

rpc_ctx_t *rpc_create(void) {
    compat_init();
    rpc_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->el = el_create();
    ctx->listen_fd = INVALID_SOCK;
    ctx->next_msg_id = 1;
    return ctx;
}

void rpc_destroy(rpc_ctx_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->peer_count; i++) peer_destroy(ctx->peers[i]);
    if (ctx->has_listen) {
        el_del(ctx->el, ctx->listen_fd);
        close_sock(ctx->listen_fd);
    }
    el_destroy(ctx->el);
    for (int i = 0; i < ctx->handler_count; i++) free(ctx->handlers[i].method);
    free(ctx);
}

int rpc_listen(rpc_ctx_t *ctx, const char *host, int port) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (host && host[0]) inet_pton(AF_INET, host, &addr.sin_addr);
    else addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close_sock(fd); return -1; }
    if (listen(fd, 128) < 0) { close_sock(fd); return -1; }

    compat_set_nonblock(fd);
    ctx->listen_fd = fd;
    ctx->has_listen = 1;
    el_add(ctx->el, fd, EL_READ, accept_cb, ctx);
    return 0;
}

/* Resolve host:port (host may be a name or IPv4 literal). Returns 0 on success. */
static int resolve_host(const char *host, int port, struct sockaddr_in *out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) return 0;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;
    memcpy(out, res->ai_addr, sizeof(*out));
    out->sin_port = htons((uint16_t)port);
    freeaddrinfo(res);
    return 0;
}

rpc_peer_t *rpc_connect(rpc_ctx_t *ctx, const char *url) {
    /* Parse ws://host[:port][/path] */
    char host[256] = {0};
    int port = 80;
    char path[256] = "/";

    if (strncmp(url, "ws://", 5) == 0) url += 5;
    const char *slash = strchr(url, '/');
    const char *colon = strchr(url, ':');
    if (slash && colon && colon > slash) colon = NULL;

    int host_len;
    if (colon) {
        host_len = (int)(colon - url);
        port = atoi(colon + 1);
    } else if (slash) {
        host_len = (int)(slash - url);
    } else {
        host_len = (int)strlen(url);
    }
    if (host_len <= 0 || host_len >= (int)sizeof(host)) return NULL;
    memcpy(host, url, host_len);
    if (slash) snprintf(path, sizeof(path), "%s", slash);

    struct sockaddr_in addr;
    if (resolve_host(host, port, &addr) < 0) return NULL;

    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) return NULL;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close_sock(fd);
        return NULL;
    }

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt));

    rpc_peer_t *p = peer_create(ctx, fd, 1);

    uint8_t *req;
    int req_len;
    ws_client_handshake(host, port, path, &req, &req_len, p->ws_key);
    peer_send_raw(p, req, req_len);
    free(req);

    while (!p->handshake_done) {
        el_poll(ctx->el, 100);
    }

    return p;
}

int rpc_register(rpc_ctx_t *ctx, const char *method, rpc_handler_t handler) {
    if (ctx->handler_count >= MAX_HANDLERS) return -1;
    size_t mlen = strlen(method);
    char *copy = malloc(mlen + 1);
    memcpy(copy, method, mlen + 1);
    ctx->handlers[ctx->handler_count].method = copy;
    ctx->handlers[ctx->handler_count].handler = handler;
    ctx->handler_count++;
    return 0;
}

static void remove_pending(rpc_peer_t *peer, pending_call_t *pc) {
    for (int i = 0; i < peer->pending_count; i++) {
        if (peer->pending[i] == pc) {
            peer->pending[i] = peer->pending[--peer->pending_count];
            return;
        }
    }
}

int rpc_call(rpc_peer_t *peer, const char *method,
             const void *params, int params_len,
             void **result, int *result_len, int timeout_ms) {
    rpc_ctx_t *ctx = peer->ctx;
    uint32_t msg_id = ctx->next_msg_id++;

    rpc_msg_t msg = {0};
    msg.msg_id = msg_id;
    msg.type = MSG_REQUEST;
    msg.method = (char *)method;
    msg.method_len = (uint16_t)strlen(method);
    msg.payload = (void *)params;
    msg.payload_len = (uint32_t)params_len;

    pending_call_t *pc = calloc(1, sizeof(*pc));
    pc->msg_id = msg_id;
    peer->pending[peer->pending_count++] = pc;

    peer_send_rpc(peer, &msg);

    int elapsed = 0;
    while (!pc->done && (timeout_ms <= 0 || elapsed < timeout_ms)) {
        el_poll(ctx->el, 10);
        elapsed += 10;
    }

    int status;
    if (!pc->done) {
        status = -1;
    } else {
        status = pc->status;
        if (result) *result = pc->result;
        else free(pc->result);
        if (result_len) *result_len = pc->result_len;
    }

    remove_pending(peer, pc);
    free(pc);
    return status;
}

int rpc_call_async(rpc_peer_t *peer, const char *method,
                   const void *params, int params_len,
                   rpc_callback_t cb, void *userdata) {
    rpc_ctx_t *ctx = peer->ctx;
    uint32_t msg_id = ctx->next_msg_id++;

    rpc_msg_t msg = {0};
    msg.msg_id = msg_id;
    msg.type = MSG_REQUEST;
    msg.method = (char *)method;
    msg.method_len = (uint16_t)strlen(method);
    msg.payload = (void *)params;
    msg.payload_len = (uint32_t)params_len;

    pending_call_t *pc = calloc(1, sizeof(*pc));
    pc->msg_id = msg_id;
    pc->cb = cb;
    pc->userdata = userdata;
    peer->pending[peer->pending_count++] = pc;

    peer_send_rpc(peer, &msg);
    return 0;
}

int rpc_run(rpc_ctx_t *ctx) {
    el_run(ctx->el);
    return 0;
}

int rpc_poll(rpc_ctx_t *ctx, int timeout_ms) {
    return el_poll(ctx->el, timeout_ms);
}
