#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../include/rpc.h"

static void handle_ping(rpc_peer_t *peer, const void *params, int params_len,
                        void **result, int *result_len) {
    (void)peer;
    printf("[client] received ping from server: %.*s\n",
           params_len, (const char *)params);
    const char *reply = "pong from client!";
    *result = strdup(reply);
    *result_len = strlen(reply);
}

int main(void) {
    rpc_ctx_t *ctx = rpc_create();

    /* Client also registers a handler (bidirectional) */
    rpc_register(ctx, "ping", handle_ping);

    rpc_peer_t *peer = rpc_connect(ctx, "ws://www.hongyunz.cc");
    if (!peer) {
        fprintf(stderr, "Failed to connect\n");
        return 1;
    }
    printf("Connected to server\n");

    /* Call hello -- this will also trigger server to call ping back */
    void *result = NULL;
    int result_len = 0;
    int rc = rpc_call(peer, "hello", NULL, 0, &result, &result_len, 5000);
    if (rc == 0) {
        printf("[client] hello() => %.*s\n", result_len, (char *)result);
        free(result);
    }

    /* Call add(3, 7) */
    int params[2] = {3, 7};
    result = NULL;
    rc = rpc_call(peer, "add", params, sizeof(params), &result, &result_len, 5000);
    if (rc == 0) {
        int sum;
        memcpy(&sum, result, 4);
        printf("[client] add(3, 7) => %d\n", sum);
        free(result);
    }

    /* Give the event loop a moment to process the server's reverse ping */
    for (int i = 0; i < 50; i++) rpc_poll(ctx, 10);

    rpc_destroy(ctx);
    return 0;
}
