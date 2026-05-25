#include "../include/rpc.h"
#include "event_loop.h"
#include "protocol.h"
#include "ws.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdatomic.h>

#define MAX_HANDLERS 128
#define MAX_PENDING 256

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
    int fd;
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
    int listen_fd;
    handler_entry_t handlers[MAX_HANDLERS];
    int handler_count;
    atomic_uint next_msg_id;
    rpc_peer_t *peers[256];
    int peer_count;
};

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void peer_on_data(event_loop_t *el, int fd, uint32_t events, void *data);

static rpc_peer_t *peer_create(rpc_ctx_t *ctx, int fd, int is_client) {
    rpc_peer_t *p = calloc(1, sizeof(*p));
    p->ctx = ctx;
    p->fd = fd;
    p->is_client = is_client;
    ws_buf_init(&p->recv_buf);
    ws_buf_init(&p->send_buf);
    set_nonblock(fd);
    el_add(ctx->el, fd, EPOLLIN, peer_on_data, p);
    ctx->peers[ctx->peer_count++] = p;
    return p;
}

static void peer_destroy(rpc_peer_t *p) {
    if (!p) return;
    el_del(p->ctx->el, p->fd);
    close(p->fd);
    ws_buf_free(&p->recv_buf);
    ws_buf_free(&p->send_buf);
    for (int i = 0; i < p->pending_count; i++) free(p->pending[i]);
    free(p);
}

static void peer_send_raw(rpc_peer_t *p, const void *data, int len) {
    int sent = write(p->fd, data, len);
    if (sent < 0) sent = 0;
    if (sent < len) {
        ws_buf_append(&p->send_buf, (uint8_t *)data + sent, len - sent);
        el_mod(p->ctx->el, p->fd, EPOLLIN | EPOLLOUT);
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
        resp.payload_len = strlen(err);
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
                free(pc);
            } else {
                /* sync call */
                pc->done = 1;
                pc->status = (msg->type == MSG_ERROR) ? -1 : 0;
                if (msg->payload_len > 0) {
                    pc->result = malloc(msg->payload_len);
                    memcpy(pc->result, msg->payload, msg->payload_len);
                    pc->result_len = msg->payload_len;
                }
            }
            /* Remove from pending */
            if (pc->cb) {
                p->pending[i] = p->pending[--p->pending_count];
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

    /* Process WebSocket frames */
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

static void peer_on_data(event_loop_t *el, int fd, uint32_t events, void *data) {
    rpc_peer_t *p = data;

    if (events & EPOLLOUT) {
        if (p->send_buf.len > 0) {
            int n = write(fd, p->send_buf.data, p->send_buf.len);
            if (n > 0) ws_buf_consume(&p->send_buf, n);
            if (p->send_buf.len == 0) el_mod(el, fd, EPOLLIN);
        }
    }

    if (events & EPOLLIN) {
        uint8_t buf[8192];
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            /* Peer disconnected */
            return;
        }
        ws_buf_append(&p->recv_buf, buf, n);
        process_peer_data(p);
    }
}

static void accept_cb(event_loop_t *el, int fd, uint32_t events, void *data) {
    rpc_ctx_t *ctx = data;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int client_fd = accept(fd, (struct sockaddr *)&addr, &alen);
    if (client_fd < 0) return;
    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    peer_create(ctx, client_fd, 0);
}

/* Public API */

rpc_ctx_t *rpc_create(void) {
    rpc_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->el = el_create();
    ctx->listen_fd = -1;
    ctx->next_msg_id = 1;
    return ctx;
}

void rpc_destroy(rpc_ctx_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->peer_count; i++) peer_destroy(ctx->peers[i]);
    if (ctx->listen_fd >= 0) { el_del(ctx->el, ctx->listen_fd); close(ctx->listen_fd); }
    el_destroy(ctx->el);
    for (int i = 0; i < ctx->handler_count; i++) free(ctx->handlers[i].method);
    free(ctx);
}

int rpc_listen(rpc_ctx_t *ctx, const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host && host[0]) inet_pton(AF_INET, host, &addr.sin_addr);
    else addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 128) < 0) { close(fd); return -1; }

    set_nonblock(fd);
    ctx->listen_fd = fd;
    el_add(ctx->el, fd, EPOLLIN, accept_cb, ctx);
    return 0;
}

rpc_peer_t *rpc_connect(rpc_ctx_t *ctx, const char *url) {
    /* Parse ws://host:port/path */
    char host[256] = {0};
    int port = 80;
    char path[256] = "/";

    if (strncmp(url, "ws://", 5) == 0) url += 5;
    const char *colon = strchr(url, ':');
    const char *slash = strchr(url, '/');

    if (colon) {
        memcpy(host, url, colon - url);
        port = atoi(colon + 1);
    } else if (slash) {
        memcpy(host, url, slash - url);
    } else {
        strcpy(host, url);
    }
    if (slash) strncpy(path, slash, sizeof(path) - 1);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    rpc_peer_t *p = peer_create(ctx, fd, 1);

    /* Send WebSocket handshake */
    uint8_t *req;
    int req_len;
    ws_client_handshake(host, port, path, &req, &req_len, p->ws_key);
    peer_send_raw(p, req, req_len);
    free(req);

    /* Wait for handshake response */
    while (!p->handshake_done) {
        el_poll(ctx->el, 100);
    }

    return p;
}

int rpc_register(rpc_ctx_t *ctx, const char *method, rpc_handler_t handler) {
    if (ctx->handler_count >= MAX_HANDLERS) return -1;
    ctx->handlers[ctx->handler_count].method = strdup(method);
    ctx->handlers[ctx->handler_count].handler = handler;
    ctx->handler_count++;
    return 0;
}

int rpc_call(rpc_peer_t *peer, const char *method,
             const void *params, int params_len,
             void **result, int *result_len, int timeout_ms) {
    rpc_ctx_t *ctx = peer->ctx;
    uint32_t msg_id = atomic_fetch_add(&ctx->next_msg_id, 1);

    rpc_msg_t msg = {0};
    msg.msg_id = msg_id;
    msg.type = MSG_REQUEST;
    msg.method = (char *)method;
    msg.method_len = strlen(method);
    msg.payload = (void *)params;
    msg.payload_len = params_len;

    pending_call_t *pc = calloc(1, sizeof(*pc));
    pc->msg_id = msg_id;
    peer->pending[peer->pending_count++] = pc;

    peer_send_rpc(peer, &msg);

    /* Poll until done or timeout */
    int elapsed = 0;
    while (!pc->done && (timeout_ms <= 0 || elapsed < timeout_ms)) {
        el_poll(ctx->el, 10);
        elapsed += 10;
    }

    if (!pc->done) {
        /* Timeout - remove from pending */
        for (int i = 0; i < peer->pending_count; i++) {
            if (peer->pending[i] == pc) {
                peer->pending[i] = peer->pending[--peer->pending_count];
                break;
            }
        }
        free(pc);
        return -1;
    }

    int status = pc->status;
    if (result) *result = pc->result;
    else free(pc->result);
    if (result_len) *result_len = pc->result_len;

    /* Remove from pending */
    for (int i = 0; i < peer->pending_count; i++) {
        if (peer->pending[i] == pc) {
            peer->pending[i] = peer->pending[--peer->pending_count];
            break;
        }
    }
    free(pc);
    return status;
}

int rpc_call_async(rpc_peer_t *peer, const char *method,
                   const void *params, int params_len,
                   rpc_callback_t cb, void *userdata) {
    rpc_ctx_t *ctx = peer->ctx;
    uint32_t msg_id = atomic_fetch_add(&ctx->next_msg_id, 1);

    rpc_msg_t msg = {0};
    msg.msg_id = msg_id;
    msg.type = MSG_REQUEST;
    msg.method = (char *)method;
    msg.method_len = strlen(method);
    msg.payload = (void *)params;
    msg.payload_len = params_len;

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
