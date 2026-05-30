/* event_loop.c — epoll wrapper, timer management, fd registration */

#include "event_loop.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

/* Maximum events returned per epoll_wait call */
#define MAX_EVENTS 64

/* ---- fd tracking via uthash ---- */
#include "uthash.h"

typedef struct fd_entry {
    int fd;                 /* key */
    event_cb cb;            /* event callback */
    void *data;             /* user data for event callback */
    int is_timer;           /* 1 if this fd is a timerfd */
    timer_cb tcb;           /* timer callback (only if is_timer) */
    void *tdata;            /* timer user data (only if is_timer) */
    UT_hash_handle hh;
} fd_entry_t;

/* ---- stall detection state ---- */
typedef struct {
    int timerfd;
    uint32_t threshold_ms;
    uint64_t last_fire_ns;  /* monotonic timestamp of last timer fire */
} stall_state_t;

/* ---- event loop structure ---- */
struct event_loop {
    int epoll_fd;
    int running;
    fd_entry_t *fd_map;     /* uthash map: fd → fd_entry_t */
    stall_state_t stall;    /* stall detection (timerfd = -1 if disabled) */
};

/* ---- internal helpers ---- */

static fd_entry_t *
find_entry(event_loop_t *loop, int fd)
{
    fd_entry_t *entry = NULL;
    HASH_FIND_INT(loop->fd_map, &fd, entry);
    return entry;
}

/* Timer event callback wrapper: reads the timerfd expiration count,
 * then invokes the user's timer_cb. */
static void
timer_event_cb(event_loop_t *loop, int fd, uint32_t events, void *data)
{
    (void)events;
    (void)data;

    /* Must read the timerfd to acknowledge the expiration */
    uint64_t expirations = 0;
    ssize_t r = read(fd, &expirations, sizeof(expirations));
    (void)r;

    fd_entry_t *entry = find_entry(loop, fd);
    if (entry && entry->is_timer && entry->tcb) {
        entry->tcb(loop, entry->tdata);
    }
}

/* Stall detection timer callback */
static void
stall_timer_cb(event_loop_t *loop, void *data)
{
    (void)data;

    uint64_t now_ns = clock_monotonic_ns();
    uint64_t last_ns = loop->stall.last_fire_ns;

    if (last_ns != 0) {
        uint64_t delta_ms = (now_ns - last_ns) / 1000000ULL;
        if (delta_ms > (uint64_t)loop->stall.threshold_ms) {
            log_msg(LOG_CRIT, "[event_loop] Stall detected: %llu ms since "
                    "last timer fire (threshold: %u ms)",
                    (unsigned long long)delta_ms,
                    loop->stall.threshold_ms);
            exit(1);
        }
    }

    loop->stall.last_fire_ns = now_ns;
}

/* ---- public API ---- */

event_loop_t *
event_loop_create(void)
{
    event_loop_t *loop = calloc(1, sizeof(*loop));
    if (!loop)
        return NULL;

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        free(loop);
        return NULL;
    }

    loop->running = 0;
    loop->fd_map = NULL;
    loop->stall.timerfd = -1;
    loop->stall.threshold_ms = 0;
    loop->stall.last_fire_ns = 0;

    return loop;
}

int
event_loop_add_fd(event_loop_t *loop, int fd, uint32_t events,
                  event_cb cb, void *data)
{
    if (!loop || fd < 0 || !cb)
        return -1;

    /* Check for duplicate */
    if (find_entry(loop, fd))
        return -1;

    fd_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry)
        return -1;

    entry->fd = fd;
    entry->cb = cb;
    entry->data = data;
    entry->is_timer = 0;
    entry->tcb = NULL;
    entry->tdata = NULL;

    struct epoll_event ev = {0};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        free(entry);
        return -1;
    }

    HASH_ADD_INT(loop->fd_map, fd, entry);
    return 0;
}

int
event_loop_mod_fd(event_loop_t *loop, int fd, uint32_t events)
{
    if (!loop || fd < 0)
        return -1;

    fd_entry_t *entry = find_entry(loop, fd);
    if (!entry)
        return -1;

    struct epoll_event ev = {0};
    ev.events = events;
    ev.data.fd = fd;

    return epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

int
event_loop_del_fd(event_loop_t *loop, int fd)
{
    if (!loop || fd < 0)
        return -1;

    fd_entry_t *entry = find_entry(loop, fd);
    if (!entry)
        return -1;

    epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    HASH_DEL(loop->fd_map, entry);
    free(entry);
    return 0;
}

int
event_loop_add_timer(event_loop_t *loop, uint64_t ms, timer_cb cb, void *data)
{
    if (!loop || ms == 0 || !cb)
        return -1;

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0)
        return -1;

    /* Set up recurring timer */
    struct itimerspec its = {0};
    its.it_value.tv_sec = (time_t)(ms / 1000);
    its.it_value.tv_nsec = (long)((ms % 1000) * 1000000);
    its.it_interval.tv_sec = its.it_value.tv_sec;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;

    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        close(tfd);
        return -1;
    }

    /* Register with epoll using the internal timer wrapper callback */
    fd_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        close(tfd);
        return -1;
    }

    entry->fd = tfd;
    entry->cb = timer_event_cb;
    entry->data = NULL;
    entry->is_timer = 1;
    entry->tcb = cb;
    entry->tdata = data;

    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.fd = tfd;

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, tfd, &ev) < 0) {
        free(entry);
        close(tfd);
        return -1;
    }

    HASH_ADD_INT(loop->fd_map, fd, entry);
    return tfd;
}

int
event_loop_enable_stall_detection(event_loop_t *loop,
                                  uint32_t stall_threshold_ms)
{
    if (!loop || stall_threshold_ms == 0)
        return -1;

    /* Fire every threshold/2 ms */
    uint64_t interval_ms = stall_threshold_ms / 2;
    if (interval_ms == 0)
        interval_ms = 1;

    int tfd = event_loop_add_timer(loop, interval_ms, stall_timer_cb, NULL);
    if (tfd < 0)
        return -1;

    loop->stall.timerfd = tfd;
    loop->stall.threshold_ms = stall_threshold_ms;
    loop->stall.last_fire_ns = clock_monotonic_ns();

    return 0;
}

void
event_loop_run(event_loop_t *loop)
{
    if (!loop)
        return;

    loop->running = 1;
    struct epoll_event events[MAX_EVENTS];

    while (loop->running) {
        int nfds = epoll_wait(loop->epoll_fd, events, MAX_EVENTS, -1);

        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            log_msg(LOG_CRIT, "[event_loop] epoll_wait failed: %s",
                    strerror(errno));
            break;
        }

        /* Update cached log timestamp once per dispatch iteration */
        log_update_time();

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            fd_entry_t *entry = find_entry(loop, fd);
            if (entry && entry->cb) {
                entry->cb(loop, fd, ev, entry->data);
            }
        }
    }
}

void
event_loop_stop(event_loop_t *loop)
{
    if (loop)
        loop->running = 0;
}

void
event_loop_destroy(event_loop_t *loop)
{
    if (!loop)
        return;

    /* Close all tracked fds (timerfds and any remaining) and free entries */
    fd_entry_t *entry, *tmp;
    HASH_ITER(hh, loop->fd_map, entry, tmp) {
        if (entry->is_timer) {
            close(entry->fd);
        }
        HASH_DEL(loop->fd_map, entry);
        free(entry);
    }

    if (loop->epoll_fd >= 0)
        close(loop->epoll_fd);

    free(loop);
}
