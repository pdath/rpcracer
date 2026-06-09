/* test_stats_json.c — Property test for statistics JSON serialization (Property 15)
 *
 * Property 15: Statistics JSON Serialization
 * Generate random stats via the public API; serialize to JSON; deserialize
 * with yyjson and verify field equivalence against a local oracle.
 *
 * Validates: Requirements 10.4
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand48/lrand48),
 * 1000 trials, seed printed for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "../src/stats.h"
#include "../src/config.h"
#include "yyjson.h"

#define BUF_SIZE 8192

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_MSG(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: " fmt " (line %d)\n", __VA_ARGS__, __LINE__); \
        return -1; \
    } \
} while (0)

/* Oracle for per-node expected values */
typedef struct {
    uint32_t gbt_wins;
    uint64_t gbt_total_time_us;
    uint64_t gbt_last_response_us;
    uint32_t gbt_last_tx_count;
    uint64_t gbt_last_since_notify_us;
    uint32_t notify_wins;
} node_oracle_t;

/* Generate a random label string */
static void
gen_random_label(char *buf, size_t buf_size, int index)
{
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    int base_len = 3 + (int)(lrand48() % 10);
    char base[32];
    for (int i = 0; i < base_len; i++)
        base[i] = charset[lrand48() % (sizeof(charset) - 1)];
    base[base_len] = '\0';
    snprintf(buf, buf_size, "%s_%d", base, index);
}

/* Generate a random config_t with labels for the given node_count */
static void
gen_random_config(config_t *cfg, int node_count)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->node_count = node_count;
    for (int i = 0; i < node_count; i++)
        gen_random_label(cfg->nodes[i].label, sizeof(cfg->nodes[i].label), i);
}

/* Verify a single trial */
static int
verify_trial(int trial, stats_t *s, config_t *cfg, node_oracle_t *oracle,
             int node_count, uint32_t total_requests, uint32_t total_races,
             uint64_t last_notify_to_gbt_us)
{
    char buf[BUF_SIZE];

    stats_serialize_ctx_t ctx = {
        .stats = s,
        .cfg = cfg,
        .proxy = NULL
    };

    int len = stats_serialize_json(buf, BUF_SIZE, &ctx);
    ASSERT_MSG(len > 0, "trial %d: serialize returned %d", trial, len);
    ASSERT_MSG((size_t)len == strlen(buf), "trial %d: len %d != strlen %zu",
               trial, len, strlen(buf));

    /* Parse JSON */
    yyjson_doc *doc = yyjson_read(buf, (size_t)len, 0);
    ASSERT_MSG(doc != NULL, "trial %d: yyjson_read failed", trial);

    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_MSG(yyjson_is_obj(root), "trial %d: root is not object", trial);

    /* Verify top-level fields */
    yyjson_val *v;

    v = yyjson_obj_get(root, "uptime_seconds");
    ASSERT_MSG(v != NULL && yyjson_is_uint(v),
               "trial %d: uptime_seconds missing or wrong type", trial);

    v = yyjson_obj_get(root, "total_rpc_requests");
    ASSERT_MSG(v != NULL, "trial %d: total_rpc_requests missing", trial);
    ASSERT_MSG(yyjson_get_uint(v) == total_requests,
               "trial %d: total_rpc_requests expected %u got %llu",
               trial, total_requests, (unsigned long long)yyjson_get_uint(v));

    v = yyjson_obj_get(root, "total_gbt_races");
    ASSERT_MSG(v != NULL, "trial %d: total_gbt_races missing", trial);
    ASSERT_MSG(yyjson_get_uint(v) == total_races,
               "trial %d: total_gbt_races expected %u got %llu",
               trial, total_races, (unsigned long long)yyjson_get_uint(v));

    v = yyjson_obj_get(root, "last_notify_to_gbt_us");
    ASSERT_MSG(v != NULL, "trial %d: last_notify_to_gbt_us missing", trial);
    ASSERT_MSG(yyjson_get_uint(v) == last_notify_to_gbt_us,
               "trial %d: last_notify_to_gbt_us expected %llu got %llu",
               trial, (unsigned long long)last_notify_to_gbt_us,
               (unsigned long long)yyjson_get_uint(v));

    /* Verify nodes array */
    yyjson_val *nodes = yyjson_obj_get(root, "nodes");
    ASSERT_MSG(nodes != NULL && yyjson_is_arr(nodes),
               "trial %d: nodes missing or not array", trial);
    ASSERT_MSG((int)yyjson_arr_size(nodes) == node_count,
               "trial %d: nodes array size %zu != %d",
               trial, yyjson_arr_size(nodes), node_count);

    /* Verify each node */
    for (int i = 0; i < node_count; i++) {
        node_oracle_t *no = &oracle[i];
        yyjson_val *node_obj = yyjson_arr_get(nodes, (size_t)i);
        ASSERT_MSG(node_obj != NULL && yyjson_is_obj(node_obj),
                   "trial %d: node[%d] missing or not object", trial, i);

        /* label */
        v = yyjson_obj_get(node_obj, "label");
        ASSERT_MSG(v != NULL && yyjson_is_str(v),
                   "trial %d: node[%d].label missing", trial, i);
        ASSERT_MSG(strcmp(yyjson_get_str(v), cfg->nodes[i].label) == 0,
                   "trial %d: node[%d].label '%s' != '%s'",
                   trial, i, yyjson_get_str(v), cfg->nodes[i].label);

        /* Navigate into race_gbt nested object */
        yyjson_val *gbt_obj = yyjson_obj_get(node_obj, "race_gbt");
        ASSERT_MSG(gbt_obj != NULL && yyjson_is_obj(gbt_obj),
                   "trial %d: node[%d].race_gbt missing or not object", trial, i);

        /* avg_us */
        uint64_t expected_avg = 0;
        if (no->gbt_wins > 0)
            expected_avg = no->gbt_total_time_us / no->gbt_wins;
        v = yyjson_obj_get(gbt_obj, "avg_us");
        ASSERT_MSG(v != NULL, "trial %d: node[%d].race_gbt.avg_us missing", trial, i);
        ASSERT_MSG(yyjson_get_uint(v) == expected_avg,
                   "trial %d: node[%d].race_gbt.avg_us expected %llu got %llu",
                   trial, i, (unsigned long long)expected_avg,
                   (unsigned long long)yyjson_get_uint(v));

        /* last_us */
        v = yyjson_obj_get(gbt_obj, "last_us");
        ASSERT_MSG(v != NULL, "trial %d: node[%d].race_gbt.last_us missing", trial, i);
        ASSERT_MSG(yyjson_get_uint(v) == no->gbt_last_response_us,
                   "trial %d: node[%d].race_gbt.last_us expected %llu got %llu",
                   trial, i, (unsigned long long)no->gbt_last_response_us,
                   (unsigned long long)yyjson_get_uint(v));

        /* wins */
        v = yyjson_obj_get(gbt_obj, "wins");
        ASSERT_MSG(v != NULL, "trial %d: node[%d].race_gbt.wins missing", trial, i);
        ASSERT_MSG(yyjson_get_uint(v) == no->gbt_wins,
                   "trial %d: node[%d].race_gbt.wins expected %u got %llu",
                   trial, i, no->gbt_wins, (unsigned long long)yyjson_get_uint(v));

        /* last_tx_count */
        v = yyjson_obj_get(gbt_obj, "last_tx_count");
        ASSERT_MSG(v != NULL, "trial %d: node[%d].race_gbt.last_tx_count missing", trial, i);
        ASSERT_MSG(yyjson_get_uint(v) == no->gbt_last_tx_count,
                   "trial %d: node[%d].race_gbt.last_tx_count expected %u got %llu",
                   trial, i, no->gbt_last_tx_count,
                   (unsigned long long)yyjson_get_uint(v));

        /* since_notify_us */
        v = yyjson_obj_get(gbt_obj, "since_notify_us");
        ASSERT_MSG(v != NULL, "trial %d: node[%d].race_gbt.since_notify_us missing", trial, i);
        ASSERT_MSG(yyjson_get_uint(v) == no->gbt_last_since_notify_us,
                   "trial %d: node[%d].race_gbt.since_notify_us expected %llu got %llu",
                   trial, i, (unsigned long long)no->gbt_last_since_notify_us,
                   (unsigned long long)yyjson_get_uint(v));

        /* notify_wins */
        v = yyjson_obj_get(node_obj, "notify_wins");
        ASSERT_MSG(v != NULL, "trial %d: node[%d].notify_wins missing", trial, i);
        ASSERT_MSG(yyjson_get_uint(v) == no->notify_wins,
                   "trial %d: node[%d].notify_wins expected %u got %llu",
                   trial, i, no->notify_wins, (unsigned long long)yyjson_get_uint(v));

        /* state — should be "unknown" since proxy is NULL */
        v = yyjson_obj_get(node_obj, "state");
        ASSERT_MSG(v != NULL && yyjson_is_str(v),
                   "trial %d: node[%d].state missing or not string", trial, i);
    }

    yyjson_doc_free(doc);
    return 0;
}

/*
 * Property 15: Statistics JSON Serialization
 *
 * For any valid sequence of record calls, serializing to JSON and
 * deserializing with yyjson shall produce field-equivalent values
 * matching the local oracle.
 *
 * Validates: Requirements 10.4
 */
static void
test_property_stats_json_serialization(long seed)
{
    printf("  property: statistics JSON serialization (seed=%ld, 1000 trials)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int i = 0; i < trials; i++) {
        /* Random node count 1-8 */
        int node_count = 1 + (int)(lrand48() % MAX_NODES);

        stats_t *s = stats_create(node_count);
        if (!s) {
            fprintf(stderr, "  FAIL: trial %d: stats_create returned NULL\n", i);
            tests_run++;
            return;
        }

        config_t cfg;
        gen_random_config(&cfg, node_count);

        /* Oracle tracking */
        node_oracle_t oracle[MAX_NODES];
        memset(oracle, 0, sizeof(oracle));
        uint32_t total_requests = 0;
        uint32_t total_races = 0;
        uint64_t last_notify_to_gbt_us = 0;

        /* Generate random GBT recordings (1-20 per node) */
        for (int n = 0; n < node_count; n++) {
            int num_gbt = 1 + (int)(lrand48() % 20);
            for (int g = 0; g < num_gbt; g++) {
                uint64_t response_us = 1 + (uint64_t)(lrand48() % 500000);
                uint32_t tx_count = (uint32_t)(lrand48() % 50000);
                uint64_t since_notify_us = (uint64_t)(lrand48() % 1000000);

                stats_record_gbt(s, n, response_us, tx_count, since_notify_us);
                stats_record_race_win(s, n);

                oracle[n].gbt_wins++;
                oracle[n].gbt_total_time_us += response_us;
                oracle[n].gbt_last_response_us = response_us;
                oracle[n].gbt_last_tx_count = tx_count;
                oracle[n].gbt_last_since_notify_us = since_notify_us;
                total_races++;
            }
        }

        /* Random notify wins */
        int num_notify_wins = (int)(lrand48() % 30);
        for (int nw = 0; nw < num_notify_wins; nw++) {
            int winner = (int)(lrand48() % node_count);
            stats_record_notify_win(s, winner);
            oracle[winner].notify_wins++;
        }

        /* Random requests */
        total_requests = (uint32_t)(lrand48() % 100000);
        for (uint32_t rq = 0; rq < total_requests; rq++)
            stats_record_request(s);

        /* Random notify-to-gbt latency */
        last_notify_to_gbt_us = (uint64_t)(lrand48() % 1000000);
        stats_record_notify_to_gbt(s, last_notify_to_gbt_us);

        if (verify_trial(i, s, &cfg, oracle, node_count, total_requests,
                         total_races, last_notify_to_gbt_us) != 0) {
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, i);
            stats_destroy(s);
            tests_run++;
            return;
        }

        stats_destroy(s);
        passed++;
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    } else {
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

int
main(int argc, char *argv[])
{
    long seed;
    if (argc > 1) {
        seed = atol(argv[1]);
    } else {
        seed = (long)time(NULL);
    }

    printf("test_stats_json (seed=%ld):\n", seed);

    /* Run property test */
    test_property_stats_json_serialization(seed);

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
