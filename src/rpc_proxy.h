/* rpc_proxy.h — RPC proxy: client connection, request parsing, race dispatch */

#ifndef RPC_PROXY_H
#define RPC_PROXY_H

#include "config.h"
#include "event_loop.h"
#include "stats.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct rpc_proxy rpc_proxy_t;

/* Create the RPC proxy. Binds and listens on cfg->rpc_server_bind:cfg->rpc_server_port.
 * Initializes upstream connections (does not connect them yet).
 * Returns NULL on failure. */
rpc_proxy_t *rpc_proxy_create(event_loop_t *loop, config_t *cfg);

/* Destroy the RPC proxy. Closes listener, client, and upstream connections.
 * Frees all allocated memory. */
void rpc_proxy_destroy(rpc_proxy_t *proxy);

/* Set the stats pointer for live metrics recording.
 * Must be called after rpc_proxy_create, before the event loop runs. */
void rpc_proxy_set_stats(rpc_proxy_t *proxy, stats_t *stats);

/* Notify the proxy that a new block has been found.
 * Clears sticky state and sets notify_pending = true so the next
 * getblocktemplate() triggers a race. */
void rpc_proxy_on_block_notify(rpc_proxy_t *proxy, const uint8_t *hash);

/* Fill an array of state name strings for each upstream node.
 * Each entry points to a static string: "connected", "disconnected",
 * "connecting", "sending", "receiving", "dead", or "ibd".
 * count should be at least proxy->upstream_count. */
void rpc_proxy_get_states(rpc_proxy_t *proxy, const char **out, int count);

/* Check if a node is in IBD (Initial Block Download) state.
 * Returns true if the node has reported error -10 on a GBT request. */
bool rpc_proxy_is_node_ibd(rpc_proxy_t *proxy, int node_idx);

#endif /* RPC_PROXY_H */
