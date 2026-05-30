/* stats.h — Per-node metrics collection and JSON serialization */

#ifndef STATS_H
#define STATS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "config.h"

/* Opaque stats type — internal structure hidden in stats.c */
typedef struct stats stats_t;

/* Create and initialize a stats structure. Returns NULL on failure. */
stats_t *stats_create(int node_count);

/* Destroy a stats structure created by stats_create(). */
void stats_destroy(stats_t *s);

/* Record a GBT response for a node. */
void stats_record_gbt(stats_t *s, int node_idx, uint64_t response_time_us,
                      uint32_t tx_count, uint64_t since_notify_us);

/* Record a race win for a node. Increments gbt_wins and total_races. */
void stats_record_race_win(stats_t *s, int node_idx);

/* Record a notify win for a node. Increments notify_wins. */
void stats_record_notify_win(stats_t *s, int node_idx);

/* Record notify-to-GBT latency. */
void stats_record_notify_to_gbt(stats_t *s, uint64_t elapsed_us);

/* Record an RPC request. Increments total_requests. */
void stats_record_request(stats_t *s);

/* Record the timestamp of a block notification. */
void stats_record_notify_time(stats_t *s, uint64_t notify_time_ns);

/* Serialize stats to JSON.
 * Matches the stats_handler_cb signature: int fn(char *buf, size_t cap, void *data)
 * The data pointer should point to a stats_serialize_ctx_t.
 * Returns number of bytes written (excluding null terminator), or -1 on error. */
int stats_serialize_json(char *buf, size_t cap, void *data);

/* Forward declaration for rpc_proxy_t (used in stats_serialize_ctx_t) */
struct rpc_proxy;

/* Context passed to stats_serialize_json via the void *data parameter. */
typedef struct {
    stats_t *stats;
    config_t *cfg;
    struct rpc_proxy *proxy;  /* if non-NULL, used to get live node states */
} stats_serialize_ctx_t;

#endif /* STATS_H */
