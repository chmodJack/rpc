#ifndef RPC_H
#define RPC_H

#include <stdint.h>

typedef struct rpc_ctx rpc_ctx_t;
typedef struct rpc_peer rpc_peer_t;

typedef void (*rpc_handler_t)(rpc_peer_t *peer, const void *params, int params_len,
                               void **result, int *result_len);

typedef void (*rpc_callback_t)(int status, const void *result, int result_len, void *userdata);

rpc_ctx_t *rpc_create(void);
void rpc_destroy(rpc_ctx_t *ctx);

int rpc_listen(rpc_ctx_t *ctx, const char *host, int port);
rpc_peer_t *rpc_connect(rpc_ctx_t *ctx, const char *url);

int rpc_register(rpc_ctx_t *ctx, const char *method, rpc_handler_t handler);

int rpc_call(rpc_peer_t *peer, const char *method,
             const void *params, int params_len,
             void **result, int *result_len, int timeout_ms);

int rpc_call_async(rpc_peer_t *peer, const char *method,
                   const void *params, int params_len,
                   rpc_callback_t cb, void *userdata);

int rpc_run(rpc_ctx_t *ctx);
int rpc_poll(rpc_ctx_t *ctx, int timeout_ms);

#endif
