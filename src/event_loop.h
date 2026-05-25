#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <sys/epoll.h>

#define EL_MAX_EVENTS 64

typedef struct event_loop event_loop_t;

typedef void (*el_callback_t)(event_loop_t *el, int fd, uint32_t events, void *data);

typedef struct {
    int fd;
    el_callback_t cb;
    void *data;
} el_source_t;

struct event_loop {
    int epfd;
    int running;
    el_source_t *sources[4096];
};

event_loop_t *el_create(void);
void el_destroy(event_loop_t *el);
int el_add(event_loop_t *el, int fd, uint32_t events, el_callback_t cb, void *data);
int el_mod(event_loop_t *el, int fd, uint32_t events);
void el_del(event_loop_t *el, int fd);
int el_poll(event_loop_t *el, int timeout_ms);
void el_run(event_loop_t *el);

#endif
