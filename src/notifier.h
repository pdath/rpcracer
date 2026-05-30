/* notifier.h — ZMQ subscriptions, deduplication, and block notification relay */

#ifndef NOTIFIER_H
#define NOTIFIER_H

#include <stdint.h>
#include <stdbool.h>

#include "event_loop.h"
#include "config.h"

typedef struct notifier notifier_t;
typedef void (*notify_cb)(const uint8_t *hash, void *data);

/* Create the notifier subsystem.
 * Subscribes to ZMQ "hashblock" on each node with a zmq_port configured.
 * Integrates ZMQ fds with the event loop via epoll.
 * Invokes cb(hash, data) on each unique block hash received.
 * Returns NULL on failure. */
notifier_t *notifier_create(event_loop_t *loop, config_t *cfg,
                            notify_cb cb, void *data);

/* Destroy the notifier: close ZMQ sockets and context, free resources. */
void notifier_destroy(notifier_t *n);

/* Set the stats pointer for recording notify wins.
 * Must be called after notifier_create, before the event loop runs. */
#include "stats.h"
void notifier_set_stats(notifier_t *n, stats_t *stats);

/* Set the proxy pointer for IBD state checks in silence detection.
 * Must be called after notifier_create, before the event loop runs. */
struct rpc_proxy;
void notifier_set_proxy(notifier_t *n, struct rpc_proxy *proxy);

/* Process a block hash received from an external source.
 * node_label: human-readable label identifying the source node (e.g. "local-knots")
 * protocol: notification protocol used ("zmq" or "http")
 * Performs deduplication against last_hash and invokes the callback if unique.
 * Returns 1 if the hash was new (callback invoked), 0 if duplicate. */
int notifier_process_hash(notifier_t *n, const uint8_t *hash,
                          const char *node_label, const char *protocol);

#endif /* NOTIFIER_H */
