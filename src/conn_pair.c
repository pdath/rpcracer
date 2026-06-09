/* conn_pair.c — Connection pair: active + standby with 10s rotation */

#include "conn_pair.h"
#include "log.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* ---- internal helpers ---- */

/* Forward declaration */
static void standby_on_connected(upstream_conn_t *conn, void *data);

/* Arm (or re-arm) the rotation timerfd to fire in 10 seconds. */
static void
arm_rotation_timer(conn_pair_t *pair)
{
    struct itimerspec its = {0};
    its.it_value.tv_sec = CONN_PAIR_ROTATION_INTERVAL_MS / 1000;
    its.it_value.tv_nsec = (long)((CONN_PAIR_ROTATION_INTERVAL_MS % 1000) * 1000000);
    its.it_interval.tv_sec = its.it_value.tv_sec;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;

    timerfd_settime(pair->rotation_timer_fd, 0, &its, NULL);
}

/* Disarm the rotation timerfd (stop it from firing). */
static void
disarm_rotation_timer(conn_pair_t *pair)
{
    struct itimerspec its = {0};
    timerfd_settime(pair->rotation_timer_fd, 0, &its, NULL);
}

/* Refresh the standby: disconnect it and reconnect with a fresh connection. */
static void
refresh_standby(conn_pair_t *pair)
{
    int standby_idx = 1 - pair->active_idx;
    upstream_conn_t *standby = &pair->slots[standby_idx];

    /* Only refresh if standby is in a state we can act on */
    if (standby->state == CONN_CONNECTED || standby->state == CONN_DISCONNECTED) {
        if (standby->state == CONN_CONNECTED)
            rpc_conn_disconnect(standby);

        /* Ensure the standby has the correct on_connected callback */
        standby->cb.on_connected = standby_on_connected;
        standby->cb.data = pair;

        rpc_conn_connect(standby);
        log_msg(LOG_DEBUG, "[%s] Standby refreshed (slot %d)",
                pair->config->label, standby_idx);
    }
    /* If CONN_CONNECTING, a connect is already in progress — leave it */
}

/* Rotation timer epoll callback: always refresh standby, set swap_required. */
static void
rotation_timer_cb(event_loop_t *loop, int fd, uint32_t events, void *data)
{
    (void)loop;
    (void)events;

    conn_pair_t *pair = (conn_pair_t *)data;

    /* Acknowledge the timerfd expiration */
    uint64_t expirations = 0;
    ssize_t r = read(fd, &expirations, sizeof(expirations));
    (void)r;

    /* Always refresh the standby to keep it under 10s old */
    refresh_standby(pair);

    /* Mark that a swap is desired when the active becomes idle */
    pair->swap_required = true;

    /* If active is idle right now, swap immediately */
    conn_pair_tick(pair);
}

/* Callback invoked when the standby slot finishes connecting.
 * Attempts a swap via conn_pair_tick if conditions are right.
 * Also handles recovery: if active is down, swap immediately. */
static void
standby_on_connected(upstream_conn_t *conn, void *data)
{
    conn_pair_t *pair = (conn_pair_t *)data;
    int standby_idx = 1 - pair->active_idx;

    /* Verify this callback is for the standby slot */
    if (conn != &pair->slots[standby_idx])
        return;

    log_msg(LOG_DEBUG, "[%s] Standby connected (slot %d)",
            pair->config->label, standby_idx);

    /* If active is down, swap immediately (recovery case) */
    if (pair->slots[pair->active_idx].state != CONN_CONNECTED &&
        pair->slots[pair->active_idx].state != CONN_SENDING &&
        pair->slots[pair->active_idx].state != CONN_RECEIVING) {
        pair->active_idx = standby_idx;
        pair->swap_required = false;
        if (!pair->initial_connect_logged) {
            pair->initial_connect_logged = true;
            log_msg(LOG_INFO, "[%s] available", pair->config->label);
        } else {
            log_msg(LOG_INFO, "[%s] recovered", pair->config->label);
        }
        return;
    }

    /* Normal case: try to swap if swap_required and active is idle */
    conn_pair_tick(pair);
}

/* Default on_connected callback — used for the active slot.
 * Logs "available" once at startup so operators see all nodes come up. */
static void
slot_on_connected(upstream_conn_t *conn, void *data)
{
    (void)conn;
    conn_pair_t *pair = (conn_pair_t *)data;

    if (!pair->initial_connect_logged) {
        pair->initial_connect_logged = true;
        log_msg(LOG_INFO, "[%s] available", pair->config->label);
    }
}

/* ---- conn_pair_init and conn_pair_destroy ---- */

int
conn_pair_init(conn_pair_t *pair, event_loop_t *loop,
               node_config_t *config, int node_index)
{
    if (!pair || !loop || !config)
        return -1;

    memset(pair, 0, sizeof(*pair));

    pair->loop = loop;
    pair->config = config;
    pair->node_index = node_index;
    pair->active_idx = 0;
    pair->swap_required = false;
    pair->rotation_timer_fd = -1;

    /* Callbacks for the active slot (slot 0 at init) */
    conn_callbacks_t active_cb = {0};
    active_cb.on_connected = slot_on_connected;
    active_cb.data = pair;

    /* Callbacks for the standby slot (slot 1 at init) */
    conn_callbacks_t standby_cb = {0};
    standby_cb.on_connected = standby_on_connected;
    standby_cb.data = pair;

    /* Initialize slot 0 (active) */
    if (rpc_conn_init(&pair->slots[0], loop, config, node_index, &active_cb) < 0)
        return -1;

    /* Initialize slot 1 (standby) */
    if (rpc_conn_init(&pair->slots[1], loop, config, node_index, &standby_cb) < 0) {
        rpc_conn_destroy(&pair->slots[0]);
        return -1;
    }

    /* Initiate non-blocking connect for both slots.
     * Startup failures are not fatal — rotation timer will retry. */
    rpc_conn_connect(&pair->slots[0]);
    rpc_conn_connect(&pair->slots[1]);

    /* Create the rotation timerfd */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        rpc_conn_destroy(&pair->slots[1]);
        rpc_conn_destroy(&pair->slots[0]);
        return -1;
    }

    pair->rotation_timer_fd = tfd;

    /* Register the timerfd with the event loop */
    if (event_loop_add_fd(loop, tfd, EPOLLIN, rotation_timer_cb, pair) < 0) {
        close(tfd);
        pair->rotation_timer_fd = -1;
        rpc_conn_destroy(&pair->slots[1]);
        rpc_conn_destroy(&pair->slots[0]);
        return -1;
    }

    /* Arm the 10-second recurring timer */
    arm_rotation_timer(pair);

    return 0;
}

void
conn_pair_destroy(conn_pair_t *pair)
{
    if (!pair)
        return;

    /* Disarm and close the rotation timer */
    if (pair->rotation_timer_fd >= 0) {
        disarm_rotation_timer(pair);
        event_loop_del_fd(pair->loop, pair->rotation_timer_fd);
        close(pair->rotation_timer_fd);
        pair->rotation_timer_fd = -1;
    }

    /* Destroy both connection slots */
    rpc_conn_destroy(&pair->slots[0]);
    rpc_conn_destroy(&pair->slots[1]);
}

/* ---- conn_pair_get_active and conn_pair_is_available ---- */

upstream_conn_t *
conn_pair_get_active(conn_pair_t *pair)
{
    if (!pair)
        return NULL;

    return &pair->slots[pair->active_idx];
}

bool
conn_pair_is_available(const conn_pair_t *pair)
{
    if (!pair)
        return false;

    return pair->slots[pair->active_idx].state == CONN_CONNECTED;
}

/* ---- conn_pair_report_error ---- */

bool
conn_pair_report_error(conn_pair_t *pair)
{
    if (!pair)
        return false;

    int old_active_idx = pair->active_idx;

    /* Swap active_idx unconditionally (toggle 0↔1) */
    pair->active_idx = 1 - pair->active_idx;

    /* Close old active's fd, set state to CONN_DISCONNECTED, clear buffers */
    upstream_conn_t *old = &pair->slots[old_active_idx];
    rpc_conn_disconnect(old);

    log_msg(LOG_INFO, "[%s] swap on error, standby %s",
            pair->config->label,
            pair->slots[pair->active_idx].state == CONN_CONNECTED
                ? "available" : "unavailable");

    /* Reset rotation timer to 10s from this swap event */
    arm_rotation_timer(pair);
    pair->swap_required = false;

    /* Return true if new active is CONN_CONNECTED */
    return pair->slots[pair->active_idx].state == CONN_CONNECTED;
}

/* ---- conn_pair_tick ----
 *
 * Try to swap active and standby if:
 *   1. swap_required is true
 *   2. Active is idle (CONN_CONNECTED)
 *   3. Standby is CONN_CONNECTED
 *
 * Called by the timer callback (in case active is already idle) and
 * by the proxy after a request completes (rpc_conn_reset returns
 * the active to CONN_CONNECTED).
 */

void
conn_pair_tick(conn_pair_t *pair)
{
    if (!pair)
        return;

    if (!pair->swap_required)
        return;

    int active_idx = pair->active_idx;
    int standby_idx = 1 - active_idx;
    upstream_conn_t *active = &pair->slots[active_idx];
    upstream_conn_t *standby = &pair->slots[standby_idx];

    /* Active must be idle */
    if (active->state != CONN_CONNECTED)
        return;

    /* Standby must be ready */
    if (standby->state != CONN_CONNECTED)
        return;

    /* Swap: standby becomes active, old active becomes standby */
    pair->active_idx = standby_idx;
    pair->swap_required = false;

    log_msg(LOG_DEBUG, "[%s] Rotation complete: slot %d now active",
            pair->config->label, standby_idx);
}

/* ---- conn_pair_get_label ---- */

const char *
conn_pair_get_label(const conn_pair_t *pair)
{
    if (!pair || !pair->config)
        return "unknown";
    return pair->config->label;
}

/* ---- conn_pair_set_callbacks ---- */

void
conn_pair_set_callbacks(conn_pair_t *pair,
                        conn_response_cb on_response,
                        conn_error_cb on_error,
                        void *user_data)
{
    if (!pair)
        return;

    pair->user_data = user_data;

    pair->slots[0].cb.on_response = on_response;
    pair->slots[0].cb.on_error = on_error;

    pair->slots[1].cb.on_response = on_response;
    pair->slots[1].cb.on_error = on_error;
}
