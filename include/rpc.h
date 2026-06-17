#ifndef RPC_H
#define RPC_H

#include <stdint.h>

/* Opaque RPC context. A context can simultaneously act as a server (accepting
 * connections via rpc_listen) and as a client (initiating connections via
 * rpc_connect). All peers under one context share a single event loop. */
typedef struct rpc_ctx rpc_ctx_t;

/* Opaque peer handle representing one connected WebSocket endpoint.
 * Both inbound (server-accepted) and outbound (client-initiated) connections
 * are represented as peers, and both directions support calling and serving. */
typedef struct rpc_peer rpc_peer_t;

/* Handler for an incoming RPC request.
 *
 * peer        — the peer that sent the request. Use it to make reverse calls
 *               (server-to-client or client-to-server) from within the handler.
 * params      — raw request payload bytes (may be NULL if params_len == 0).
 * params_len  — length of the params buffer in bytes.
 * result      — out-param: handler allocates the response payload with
 *               malloc() and stores the pointer here. The framework will
 *               free() it after sending. Set to NULL for an empty response.
 * result_len  — out-param: length of the result buffer in bytes.
 */
typedef void (*rpc_handler_t)(rpc_peer_t *peer, const void *params, int params_len,
                               void **result, int *result_len);

/* Callback invoked when an async RPC call completes.
 *
 * status      — 0 on success, -1 if the remote returned an error response.
 * result      — response payload bytes. Only valid for the duration of the
 *               callback; copy if you need to retain it.
 * result_len  — length of the result buffer.
 * userdata    — the opaque pointer passed to rpc_call_async.
 */
typedef void (*rpc_callback_t)(int status, const void *result, int result_len, void *userdata);

/* Create a new RPC context with an internal epoll-based event loop.
 * Returns NULL on allocation failure. */
rpc_ctx_t *rpc_create(void);

/* Destroy a context, close all peers and the listen socket, and free all
 * associated resources. Safe to call with NULL. */
void rpc_destroy(rpc_ctx_t *ctx);

/* Start listening for incoming WebSocket connections.
 * host  — IPv4 address string (e.g. "127.0.0.1") or "" / NULL for INADDR_ANY.
 * port  — TCP port to bind.
 * Returns 0 on success, -1 on error (errno set by underlying syscall). */
int rpc_listen(rpc_ctx_t *ctx, const char *host, int port);

/* Connect to a remote RPC server. The URL must be of the form
 * "ws://host:port/path" (TLS is not supported). This call blocks until the
 * WebSocket handshake completes, pumping the event loop while it waits.
 * Returns the peer handle on success, NULL on failure. */
rpc_peer_t *rpc_connect(rpc_ctx_t *ctx, const char *url);

/* Register a handler for a named method. The same method name cannot be
 * registered twice. Returns 0 on success, -1 if the handler table is full. */
int rpc_register(rpc_ctx_t *ctx, const char *method, rpc_handler_t handler);

/* Synchronous RPC call. Blocks until the response arrives or the timeout
 * elapses, while continuing to drive the event loop (so other peers, reverse
 * calls, and async callbacks are still serviced).
 *
 * params/params_len — request payload to send (may be NULL/0).
 * result            — out-param: on success receives a malloc'd buffer with
 *                     the response payload. Pass NULL if you don't want it
 *                     (the buffer is then freed internally). Caller must
 *                     free() the returned buffer.
 * result_len        — out-param: length of the response payload. May be NULL.
 * timeout_ms        — max time to wait, in milliseconds. <= 0 means wait
 *                     forever.
 * Returns 0 on success, -1 on timeout or remote error. */
int rpc_call(rpc_peer_t *peer, const char *method,
             const void *params, int params_len,
             void **result, int *result_len, int timeout_ms);

/* Asynchronous RPC call. Sends the request immediately and returns; the
 * callback is invoked from inside the event loop when the response arrives.
 * Does not block. Returns 0 if the request was queued, -1 on error. */
int rpc_call_async(rpc_peer_t *peer, const char *method,
                   const void *params, int params_len,
                   rpc_callback_t cb, void *userdata);

/* Run the event loop forever (until the process exits or the loop is
 * stopped internally). Used by long-lived servers. */
int rpc_run(rpc_ctx_t *ctx);

/* Run one iteration of the event loop, waiting up to timeout_ms milliseconds
 * for events. Pass -1 to block indefinitely, 0 for a non-blocking poll.
 * Returns the number of events handled, or -1 on error. Useful when the host
 * application owns its own main loop and wants to drive RPC cooperatively. */
int rpc_poll(rpc_ctx_t *ctx, int timeout_ms);

#endif
