#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../include/rpc.h"

static void on_ping_reply(int status, const void *result, int result_len, void *userdata) {
    if (status == 0)
        printf("[server] client.ping() => %.*s\n", result_len, (const char *)result);
    else
        printf("[server] client.ping() failed\n");
}

static void handle_add(rpc_peer_t *peer, const void *params, int params_len,
                       void **result, int *result_len) {
    (void)peer;
    if (params_len < 8) return;
    int a, b;
    memcpy(&a, params, 4);
    memcpy(&b, (char *)params + 4, 4);
    int *sum = malloc(sizeof(int));
    *sum = a + b;
    *result = sum;
    *result_len = sizeof(int);
}

static void handle_hello(rpc_peer_t *peer, const void *params, int params_len,
                         void **result, int *result_len) {
    (void)params; (void)params_len;
    const char *reply = "Hello from server!";
    *result = strdup(reply);
    *result_len = strlen(reply);

    /* Server calls back into the client (bidirectional RPC) */
    const char *msg = "server says hi";
    rpc_call_async(peer, "ping", msg, strlen(msg), on_ping_reply, NULL);
}

int main(void) {
    rpc_ctx_t *ctx = rpc_create();

    rpc_register(ctx, "add", handle_add);
    rpc_register(ctx, "hello", handle_hello);

    if (rpc_listen(ctx, "127.0.0.1", 9000) < 0) {
        perror("rpc_listen");
        return 1;
    }

    printf("Server listening on ws://127.0.0.1:9000\n");
    rpc_run(ctx);

    rpc_destroy(ctx);
    return 0;
}
