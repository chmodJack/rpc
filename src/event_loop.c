#include "event_loop.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

event_loop_t *el_create(void) {
    event_loop_t *el = calloc(1, sizeof(*el));
    if (!el) return NULL;
    el->epfd = epoll_create1(0);
    if (el->epfd < 0) { free(el); return NULL; }
    return el;
}

void el_destroy(event_loop_t *el) {
    if (!el) return;
    close(el->epfd);
    for (int i = 0; i < 4096; i++) free(el->sources[i]);
    free(el);
}

int el_add(event_loop_t *el, int fd, uint32_t events, el_callback_t cb, void *data) {
    if (fd < 0 || fd >= 4096) return -1;
    el_source_t *s = malloc(sizeof(*s));
    s->fd = fd;
    s->cb = cb;
    s->data = data;
    free(el->sources[fd]);
    el->sources[fd] = s;
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(el->epfd, EPOLL_CTL_ADD, fd, &ev);
}

int el_mod(event_loop_t *el, int fd, uint32_t events) {
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(el->epfd, EPOLL_CTL_MOD, fd, &ev);
}

void el_del(event_loop_t *el, int fd) {
    if (fd < 0 || fd >= 4096) return;
    epoll_ctl(el->epfd, EPOLL_CTL_DEL, fd, NULL);
    free(el->sources[fd]);
    el->sources[fd] = NULL;
}

int el_poll(event_loop_t *el, int timeout_ms) {
    struct epoll_event events[EL_MAX_EVENTS];
    int n = epoll_wait(el->epfd, events, EL_MAX_EVENTS, timeout_ms);
    for (int i = 0; i < n; i++) {
        int fd = events[i].data.fd;
        el_source_t *s = el->sources[fd];
        if (s && s->cb) s->cb(el, fd, events[i].events, s->data);
    }
    return n;
}

void el_run(event_loop_t *el) {
    el->running = 1;
    while (el->running) el_poll(el, -1);
}
