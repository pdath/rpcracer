/* event_loop.h — epoll wrapper, timer management, fd registration */

#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <stdint.h>

typedef struct event_loop event_loop_t;
typedef void (*event_cb)(event_loop_t *loop, int fd, uint32_t events, void *data);
typedef void (*timer_cb)(event_loop_t *loop, void *data);

/* Create a new event loop (epoll fd, timer list, stall detection state).
 * Returns NULL on failure. */
event_loop_t *event_loop_create(void);

/* Register a file descriptor with epoll.
 * events: EPOLLIN, EPOLLOUT, EPOLLET, etc.
 * cb: callback invoked when events fire.
 * data: user pointer passed to callback.
 * Returns 0 on success, -1 on error. */
int event_loop_add_fd(event_loop_t *loop, int fd, uint32_t events,
                      event_cb cb, void *data);

/* Modify the event mask for an already-registered fd.
 * Returns 0 on success, -1 on error. */
int event_loop_mod_fd(event_loop_t *loop, int fd, uint32_t events);

/* Remove a file descriptor from epoll and free its tracking entry.
 * Returns 0 on success, -1 on error. */
int event_loop_del_fd(event_loop_t *loop, int fd);

/* Create a recurring timer that fires every ms milliseconds.
 * Implemented via timerfd_create() registered with epoll.
 * Returns the timerfd on success, -1 on error. */
int event_loop_add_timer(event_loop_t *loop, uint64_t ms,
                         timer_cb cb, void *data);

/* Enable stall detection: fires every stall_threshold_ms/2, records
 * monotonic timestamp; if delta exceeds stall_threshold_ms, logs critical
 * and calls exit(1).
 * Returns 0 on success, -1 on error. */
int event_loop_enable_stall_detection(event_loop_t *loop,
                                      uint32_t stall_threshold_ms);

/* Run the event loop (blocking). Dispatches callbacks until
 * event_loop_stop() is called. Calls log_update_time() at the
 * top of each epoll_wait dispatch iteration. */
void event_loop_run(event_loop_t *loop);

/* Signal the event loop to stop after the current iteration. */
void event_loop_stop(event_loop_t *loop);

/* Destroy the event loop: close epoll fd, close all timerfds,
 * free all fd tracking entries. */
void event_loop_destroy(event_loop_t *loop);

#endif /* EVENT_LOOP_H */
