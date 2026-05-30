/* http_server.h — Minimal HTTP server for /NOTIFY and /stats endpoints */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stddef.h>

#include "event_loop.h"
#include "config.h"
#include "notifier.h"

typedef struct http_server http_server_t;

/* Callback for stats serialization.
 * Write JSON into buf (capacity cap bytes).
 * Return number of bytes written, or -1 on error. */
typedef int (*stats_handler_cb)(char *buf, size_t cap, void *data);

/* Create the HTTP server.
 * Binds and listens on cfg->http_server_bind:cfg->http_server_port.
 * Integrates with the event loop for non-blocking I/O.
 * Routes:
 *   GET /NOTIFY/<64-hex-chars> — decode hash, call notifier_process_hash()
 *   GET /stats                 — invoke stats_cb to serialize JSON response
 * stats_cb may be NULL (returns 503 for /stats until set).
 * Returns NULL on failure. */
http_server_t *http_server_create(event_loop_t *loop, config_t *cfg,
                                   notifier_t *notifier,
                                   stats_handler_cb stats_cb, void *stats_data);

/* Destroy the HTTP server: close listener and any active client fds. */
void http_server_destroy(http_server_t *srv);

#endif /* HTTP_SERVER_H */
