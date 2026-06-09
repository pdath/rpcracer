/* rpc_conn.h — Upstream HTTP/1.1 connection management */

#ifndef RPC_CONN_H
#define RPC_CONN_H

#include "config.h"
#include "event_loop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Connection states */
typedef enum {
    CONN_DISCONNECTED,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_SENDING,
    CONN_RECEIVING
} conn_state_t;

/* Forward declaration */
typedef struct upstream_conn upstream_conn_t;

/* Callback types for proxy notification */
typedef void (*conn_established_cb)(upstream_conn_t *conn, void *data);
typedef void (*conn_send_complete_cb)(upstream_conn_t *conn, void *data);
typedef void (*conn_response_cb)(upstream_conn_t *conn, void *data);
typedef void (*conn_error_cb)(upstream_conn_t *conn, void *data);

/* Callback table set by the proxy */
typedef struct {
    conn_established_cb on_connected;
    conn_send_complete_cb on_send_complete;
    conn_response_cb on_response;
    conn_error_cb on_error;
    void *data;  /* user data passed to all callbacks */
} conn_callbacks_t;

/* Upstream connection structure */
struct upstream_conn {
    int fd;
    conn_state_t state;
    node_config_t *config;
    event_loop_t *loop;
    int node_index;  /* index in the node array, for logging */
    bool in_ibd;     /* true if node reported IBD (error -10) */

    /* Send buffer (points into shared request buffer, zero-copy; NULL when idle) */
    const uint8_t *send_buf;
    size_t send_len;
    size_t send_offset;

    /* Receive buffer (pre-allocated at startup, SOCKET_BUF_SIZE capacity) */
    uint8_t *recv_buf;
    size_t recv_len;
    size_t recv_cap;

    /* HTTP response framing */
    size_t content_length;      /* parsed from Content-Length header, 0 if unknown */
    size_t header_len;          /* length of HTTP headers (including \r\n\r\n) */
    int headers_complete;       /* 1 once headers fully received */
    int connection_close;       /* 1 if server sent Connection: close */

    /* Timing */
    uint64_t request_start_ns;  /* CLOCK_MONOTONIC nanoseconds */

    /* Callbacks */
    conn_callbacks_t cb;
};

/* Initialize an upstream connection structure.
 * Allocates the recv_buf (SOCKET_BUF_SIZE). Does not connect.
 * Returns 0 on success, -1 on allocation failure. */
int rpc_conn_init(upstream_conn_t *conn, event_loop_t *loop,
                  node_config_t *config, int node_index,
                  const conn_callbacks_t *callbacks);

/* Start a non-blocking connect to the upstream node.
 * Transitions: DISCONNECTED → CONNECTING (or CONNECTED if immediate).
 * Returns 0 on success (connection in progress), -1 on error. */
int rpc_conn_connect(upstream_conn_t *conn);

/* Begin sending a request. Sets the send buffer pointer (zero-copy).
 * Transitions: CONNECTED → SENDING.
 * Returns 0 on success, -1 if not in CONNECTED state. */
int rpc_conn_send(upstream_conn_t *conn, const uint8_t *buf, size_t len);

/* Check if the response has been fully received.
 * Returns 1 if complete, 0 if still receiving. */
int rpc_conn_response_complete(const upstream_conn_t *conn);

/* Get a pointer to the response body (after HTTP headers).
 * Sets *body and *body_len. Returns 0 on success, -1 if response not complete. */
int rpc_conn_get_response(const upstream_conn_t *conn,
                          const uint8_t **body, size_t *body_len);

/* Disconnect and close the socket. Transitions to DISCONNECTED.
 * Safe to call in any state. */
void rpc_conn_disconnect(upstream_conn_t *conn);

/* Reset the connection after a race completes.
 * Clears send_buf pointer, resets recv state.
 * If connection_close was set, disconnects only (no reconnect scheduling).
 * Otherwise transitions back to CONNECTED. */
void rpc_conn_reset(upstream_conn_t *conn);

/* Free resources (recv_buf). Does not free the conn struct itself. */
void rpc_conn_destroy(upstream_conn_t *conn);

#endif /* RPC_CONN_H */
