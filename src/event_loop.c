#include "event_loop.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <time.h>
#endif

static short to_pollev(uint32_t e) {
    short r = 0;
    if (e & EL_READ)  r |= POLLIN;
    if (e & EL_WRITE) r |= POLLOUT;
    return r;
}

static uint32_t from_pollev(short e) {
    uint32_t r = 0;
    if (e & POLLIN)  r |= EL_READ;
    if (e & POLLOUT) r |= EL_WRITE;
    if (e & (POLLERR | POLLHUP | POLLNVAL)) r |= EL_READ;
    return r;
}

static int find_idx(event_loop_t *el, socket_t fd) {
    for (int i = 0; i < el->nfds; i++)
        if (el->fds[i].fd == fd) return i;
    return -1;
}

event_loop_t *el_create(void) {
    event_loop_t *el = calloc(1, sizeof(*el));
    if (!el) return NULL;
    el->cap = 16;
    el->fds = calloc(el->cap, sizeof(struct pollfd));
    el->sources = calloc(el->cap, sizeof(el_source_t));
    return el;
}

void el_destroy(event_loop_t *el) {
    if (!el) return;
    free(el->fds);
    free(el->sources);
    free(el);
}

int el_add(event_loop_t *el, socket_t fd, uint32_t events, el_callback_t cb, void *data) {
    if (find_idx(el, fd) >= 0) return -1;
    if (el->nfds >= el->cap) {
        int nc = el->cap * 2;
        el->fds = realloc(el->fds, nc * sizeof(struct pollfd));
        el->sources = realloc(el->sources, nc * sizeof(el_source_t));
        memset(el->fds + el->cap, 0, (nc - el->cap) * sizeof(struct pollfd));
        memset(el->sources + el->cap, 0, (nc - el->cap) * sizeof(el_source_t));
        el->cap = nc;
    }
    el->fds[el->nfds].fd = fd;
    el->fds[el->nfds].events = to_pollev(events);
    el->sources[el->nfds].cb = cb;
    el->sources[el->nfds].data = data;
    el->nfds++;
    return 0;
}

int el_mod(event_loop_t *el, socket_t fd, uint32_t events) {
    int i = find_idx(el, fd);
    if (i < 0) return -1;
    el->fds[i].events = to_pollev(events);
    return 0;
}

void el_del(event_loop_t *el, socket_t fd) {
    int i = find_idx(el, fd);
    if (i < 0) return;
    /* Swap with last */
    el->fds[i] = el->fds[el->nfds - 1];
    el->sources[i] = el->sources[el->nfds - 1];
    el->nfds--;
}

int el_poll(event_loop_t *el, int timeout_ms) {
    if (el->nfds == 0) {
        /* Nothing to poll -- would block forever on poll() with no fds */
#ifdef _WIN32
        Sleep(timeout_ms > 0 ? timeout_ms : 0);
#else
        struct timespec ts = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000L };
        if (timeout_ms > 0) nanosleep(&ts, NULL);
#endif
        return 0;
    }

    int n = COMPAT_POLL(el->fds, el->nfds, timeout_ms);
    if (n <= 0) return n;

    /* Snapshot of ready fds, because callbacks may mutate the array. */
    int handled = 0;
    int snapshot_n = el->nfds;
    for (int i = 0; i < snapshot_n && handled < n; i++) {
        if (el->fds[i].revents == 0) continue;
        socket_t fd = el->fds[i].fd;
        uint32_t ev = from_pollev(el->fds[i].revents);
        el_source_t s = el->sources[i];
        el->fds[i].revents = 0;
        handled++;
        if (s.cb) s.cb(el, fd, ev, s.data);
    }
    return n;
}

void el_run(event_loop_t *el) {
    el->running = 1;
    while (el->running) el_poll(el, -1);
}
