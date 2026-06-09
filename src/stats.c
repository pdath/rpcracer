/* stats.c — Per-node metrics collection and JSON serialization */

#include "stats.h"
#include "rpc_proxy.h"
#include "util.h"
#include "yyjson.h"

#include <stdlib.h>
#include <string.h>

/* ---- Internal struct definitions (opaque to other modules) ---- */

typedef struct {
    uint64_t gbt_response_time_us;  /* last GBT response time in microseconds */
    uint64_t gbt_total_time_us;     /* cumulative GBT response time */
    uint32_t gbt_wins;              /* number of times this node won the GBT race */
    uint32_t gbt_last_tx_count;     /* transaction count from last GBT */
    uint64_t last_response_time_us; /* last response time for any RPC */
    uint64_t gbt_last_since_notify_us; /* time from notify to this node's last GBT response */
    uint32_t notify_wins;           /* number of times this node was first to notify */
} node_stats_t;

struct stats {
    node_stats_t per_node[MAX_NODES];
    int node_count;

    /* Global timing */
    uint64_t last_notify_to_gbt_us; /* time from notify to winning GBT response */
    uint64_t last_notify_time_ns;   /* CLOCK_MONOTONIC timestamp of last notify */
    uint32_t total_races;           /* total number of GBT races */
    uint32_t total_rpc_requests;        /* total RPC requests proxied */

    /* Uptime tracking */
    uint64_t start_time_ns;         /* CLOCK_MONOTONIC timestamp at init */
};

stats_t *stats_create(int node_count)
{
    stats_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->node_count = node_count;
    s->start_time_ns = clock_monotonic_ns();
    return s;
}

void stats_destroy(stats_t *s)
{
    free(s);
}

void stats_record_gbt(stats_t *s, int node_idx, uint64_t response_time_us,
                      uint32_t tx_count, uint64_t since_notify_us)
{
    if (node_idx < 0 || node_idx >= s->node_count)
        return;

    node_stats_t *ns = &s->per_node[node_idx];
    ns->gbt_response_time_us = response_time_us;
    ns->gbt_total_time_us += response_time_us;
    ns->gbt_last_tx_count = tx_count;
    ns->last_response_time_us = response_time_us;
    ns->gbt_last_since_notify_us = since_notify_us;
}

void stats_record_race_win(stats_t *s, int node_idx)
{
    if (node_idx < 0 || node_idx >= s->node_count)
        return;

    s->per_node[node_idx].gbt_wins++;
    s->total_races++;
}

void stats_record_notify_win(stats_t *s, int node_idx)
{
    if (node_idx < 0 || node_idx >= s->node_count)
        return;

    s->per_node[node_idx].notify_wins++;
}

void stats_record_notify_to_gbt(stats_t *s, uint64_t elapsed_us)
{
    s->last_notify_to_gbt_us = elapsed_us;
}

void stats_record_request(stats_t *s)
{
    s->total_rpc_requests++;
}

void stats_record_notify_time(stats_t *s, uint64_t notify_time_ns)
{
    s->last_notify_time_ns = notify_time_ns;
}

int stats_serialize_json(char *buf, size_t cap, void *data)
{
    stats_serialize_ctx_t *ctx = (stats_serialize_ctx_t *)data;
    stats_t *s = ctx->stats;
    config_t *cfg = ctx->cfg;

    /* Get live node states if proxy is available */
    const char *node_states[MAX_NODES];
    bool have_states = false;
    if (ctx->proxy) {
        rpc_proxy_get_states((rpc_proxy_t *)ctx->proxy, node_states,
                             s->node_count);
        have_states = true;
    }

    /* Calculate uptime in seconds */
    uint64_t now_ns = clock_monotonic_ns();
    uint64_t uptime_s = (now_ns - s->start_time_ns) / 1000000000ULL;

    /* Build JSON document using yyjson mutable API */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc)
        return -1;

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) {
        yyjson_mut_doc_free(doc);
        return -1;
    }
    yyjson_mut_doc_set_root(doc, root);

    /* Top-level fields */
    yyjson_mut_obj_add_uint(doc, root, "uptime_seconds", uptime_s);
    yyjson_mut_obj_add_uint(doc, root, "total_rpc_requests", s->total_rpc_requests);
    yyjson_mut_obj_add_uint(doc, root, "total_gbt_races", s->total_races);
    yyjson_mut_obj_add_uint(doc, root, "last_notify_to_gbt_us",
                            s->last_notify_to_gbt_us);

    /* Per-node array */
    yyjson_mut_val *nodes_arr = yyjson_mut_arr(doc);
    if (!nodes_arr) {
        yyjson_mut_doc_free(doc);
        return -1;
    }
    yyjson_mut_obj_add_val(doc, root, "nodes", nodes_arr);

    for (int i = 0; i < s->node_count; i++) {
        node_stats_t *ns = &s->per_node[i];
        yyjson_mut_val *node_obj = yyjson_mut_arr_add_obj(doc, nodes_arr);
        if (!node_obj) {
            yyjson_mut_doc_free(doc);
            return -1;
        }

        /* Label from config */
        yyjson_mut_obj_add_str(doc, node_obj, "label", cfg->nodes[i].label);

        /* Nested race_gbt object */
        yyjson_mut_val *gbt_obj = yyjson_mut_obj(doc);
        if (!gbt_obj) {
            yyjson_mut_doc_free(doc);
            return -1;
        }

        /* Average GBT response time */
        uint64_t avg_us = 0;
        if (ns->gbt_wins > 0)
            avg_us = ns->gbt_total_time_us / ns->gbt_wins;
        yyjson_mut_obj_add_uint(doc, gbt_obj, "avg_us", avg_us);

        /* Last GBT response time */
        yyjson_mut_obj_add_uint(doc, gbt_obj, "last_us",
                                ns->gbt_response_time_us);

        /* Last since-notify time */
        yyjson_mut_obj_add_uint(doc, gbt_obj, "since_notify_us",
                                ns->gbt_last_since_notify_us);

        /* GBT wins */
        yyjson_mut_obj_add_uint(doc, gbt_obj, "wins", ns->gbt_wins);

        /* Last tx count */
        yyjson_mut_obj_add_uint(doc, gbt_obj, "last_tx_count",
                                ns->gbt_last_tx_count);

        yyjson_mut_obj_add_val(doc, node_obj, "race_gbt", gbt_obj);

        /* Notify wins */
        yyjson_mut_obj_add_uint(doc, node_obj, "notify_wins", ns->notify_wins);

        /* Node state */
        const char *state_str = have_states ? node_states[i] : "unknown";
        yyjson_mut_obj_add_str(doc, node_obj, "state", state_str);
    }

    /* Serialize to buffer */
    size_t json_len = 0;
    char *json_str = yyjson_mut_write(doc, 0, &json_len);
    yyjson_mut_doc_free(doc);

    if (!json_str)
        return -1;

    if (json_len >= cap) {
        free(json_str);
        return -1;
    }

    memcpy(buf, json_str, json_len);
    buf[json_len] = '\0';
    free(json_str);

    return (int)json_len;
}
