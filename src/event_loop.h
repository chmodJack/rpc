#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <stdint.h>
#include "compat.h"

/* Cross-platform poll-based event loop. Uses poll() on POSIX and
 * WSAPoll() on Windows. */

#define EL_READ  0x01
#define EL_WRITE 0x02

typedef struct event_loop event_loop_t;

typedef void (*el_callback_t)(event_loop_t *el, socket_t fd, uint32_t events, void *data);

typedef struct {
    el_callback_t cb;
    void *data;
} el_source_t;

struct event_loop {
    struct pollfd *fds;
    el_source_t *sources;
    int nfds;
    int cap;
    int running;
};

event_loop_t *el_create(void);
void el_destroy(event_loop_t *el);

/* Add socket. events is a bitmask of EL_READ / EL_WRITE. */
int el_add(event_loop_t *el, socket_t fd, uint32_t events, el_callback_t cb, void *data);

/* Modify the events bitmask for an already-added socket. */
int el_mod(event_loop_t *el, socket_t fd, uint32_t events);

/* Remove socket from the loop. */
void el_del(event_loop_t *el, socket_t fd);

int el_poll(event_loop_t *el, int timeout_ms);
void el_run(event_loop_t *el);

#endif
