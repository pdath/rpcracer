/* conn_pair.h — Connection pair: active + standby with 10s rotation */

#ifndef CONN_PAIR_H
#define CONN_PAIR_H

#include "rpc_conn.h"
#include "event_loop.h"
#include "config.h"

#include <stdbool.h>

#define CONN_PAIR_ROTATION_INTERVAL_MS 10000

typedef struct conn_pair conn_pair_t;

struct conn_pair {
    /* The two connections — slot[0] and slot[1] */
    upstream_conn_t slots[2];

    /* Which slot index (0 or 1) is currently the active connection */
    int active_idx;

    /* Rotation state */
    bool swap_required;       /* set by timer, cleared after successful swap */
    int rotation_timer_fd;    /* recurring 10s timerfd */

    /* References */
    event_loop_t *loop;
    node_config_t *config;
    int node_index;

    /* Startup: log "available" exactly once on first successful connect */
    bool initial_connect_logged;

    /* User data (set by proxy layer for callback routing) */
    void *user_data;
};

/* Initialize a connection pair for a node.
 * Allocates two upstream_conn_t, arms rotation timer, initiates both connects.
 * Returns 0 on success, -1 on failure (partial resources freed). */
int conn_pair_init(conn_pair_t *pair, event_loop_t *loop,
                   node_config_t *config, int node_index);

/* Destroy a connection pair. Closes both connections, disarms timer, frees memory. */
void conn_pair_destroy(conn_pair_t *pair);

/* Get the active connection. Returns pointer to the active upstream_conn_t.
 * Use conn_pair_is_available() to check if the node is ready for new requests. */
upstream_conn_t *conn_pair_get_active(conn_pair_t *pair);

/* Check if this node is available (active connection in CONN_CONNECTED state). */
bool conn_pair_is_available(const conn_pair_t *pair);

/* Report a transport error on the active connection.
 * Swaps active/standby unconditionally.
 * Returns true if new active is CONN_CONNECTED (available for retry). */
bool conn_pair_report_error(conn_pair_t *pair);

/* Called by the rotation timer callback. Evaluates whether rotation
 * preconditions are met and executes rotation if so.
 * Also called after a request completes to check deferred rotation. */
void conn_pair_tick(conn_pair_t *pair);

/* Get the node label for logging. */
const char *conn_pair_get_label(const conn_pair_t *pair);

/* Set response/error callbacks on both slots. Stores user_data for
 * callback routing (accessible via pair->user_data in callbacks). */
void conn_pair_set_callbacks(conn_pair_t *pair,
                             conn_response_cb on_response,
                             conn_error_cb on_error,
                             void *user_data);

#endif /* CONN_PAIR_H */
