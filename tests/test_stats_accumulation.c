/* test_stats_accumulation.c — Property test for statistics accumulation (Property 14)
 *
 * Property 14: Statistics Accumulation
 * For any sequence of (node_index, response_time_us, tx_count) tuples fed to
 * the statistics collector, the per-node gbt_wins shall equal the number of
 * tuples for that node, gbt_total_time_us shall equal the sum of response
 * times for that node, and gbt_last_tx_count shall equal the tx_count from
 * the most recent tuple for that node.
 *
 * Validates: Requirements 10.1, 10.2, 10.3
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand48/lrand48),
 * 1000 trials, seed printed for reproducibility, seed accepted via argv[1].
 *
 * Verification is done through JSON serialization (opaque struct).
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
#define NUM_TRIALS 1000
#define MAX_RECORDINGS 50
#define MIN_RECORDINGS 10

/* Expected per-node accumulation state (oracle) */
typedef struct {
    uint32_t gbt_wins;
    uint64_t gbt_total_time_us;
    uint32_t gbt_last_tx_count;
    uint64_t gbt_last_response_us;
    uint64_t gbt_last_since_notify_us;
} expected_node_t;

/*
 * Property 14: Statistics Accumulation
 *
 * For each trial:
 *   - Create stats with random node_count (1-8)
 *   - Generate a random sequence of 10-50 GBT recordings
 *   - After all recordings, serialize to JSON and verify per-node:
 *     - count matches expected count
 *     - avg_us matches expected total / count
 *     - last_us matches last response time
 *     - last_tx_count matches last tx_count recorded for that node
 *
 * Validates: Requirements 10.1, 10.2, 10.3
 */
static int
test_property_stats_accumulation(long seed)
{
    printf("  property: statistics accumulation (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Random node count 1-8 */
        int node_count = 1 + (int)(lrand48() % MAX_NODES);

        /* Create stats via public API */
        stats_t *s = stats_create(node_count);
        if (!s) {
            fprintf(stderr, "  FAIL trial %d: stats_create returned NULL\n", trial);
            return -1;
        }

        /* Initialize oracle */
        expected_node_t expected[MAX_NODES];
        memset(expected, 0, sizeof(expected));

        /* Random number of recordings: 10-50 */
        int num_recordings = MIN_RECORDINGS +
                             (int)(lrand48() % (MAX_RECORDINGS - MIN_RECORDINGS + 1));

        /* Generate and apply recordings */
        for (int r = 0; r < num_recordings; r++) {
            int node_idx = (int)(lrand48() % node_count);
            uint64_t response_time_us = 1 + (uint64_t)(lrand48() % 10000000);
            uint32_t tx_count = (uint32_t)(lrand48() % 100001);
            uint64_t since_notify_us = (uint64_t)(lrand48() % 1000000);

            stats_record_gbt(s, node_idx, response_time_us, tx_count, since_notify_us);
            stats_record_race_win(s, node_idx);

            expected[node_idx].gbt_wins++;
            expected[node_idx].gbt_total_time_us += response_time_us;
            expected[node_idx].gbt_last_tx_count = tx_count;
            expected[node_idx].gbt_last_response_us = response_time_us;
            expected[node_idx].gbt_last_since_notify_us = since_notify_us;
        }

        /* Serialize to JSON and verify */
        config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.node_count = node_count;
        for (int i = 0; i < node_count; i++)
            snprintf(cfg.nodes[i].label, sizeof(cfg.nodes[i].label), "n%d", i);

        stats_serialize_ctx_t ctx = { .stats = s, .cfg = &cfg, .proxy = NULL };
        char buf[BUF_SIZE];
        int len = stats_serialize_json(buf, BUF_SIZE, &ctx);
        if (len <= 0) {
            fprintf(stderr, "  FAIL trial %d: serialize returned %d\n", trial, len);
            stats_destroy(s);
            return -1;
        }

        yyjson_doc *doc = yyjson_read(buf, (size_t)len, 0);
        if (!doc) {
            fprintf(stderr, "  FAIL trial %d: yyjson_read failed\n", trial);
            stats_destroy(s);
            return -1;
        }

        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *nodes = yyjson_obj_get(root, "nodes");

        for (int n = 0; n < node_count; n++) {
            yyjson_val *node_obj = yyjson_arr_get(nodes, (size_t)n);
            yyjson_val *gbt_obj = yyjson_obj_get(node_obj, "race_gbt");

            uint64_t json_wins = yyjson_get_uint(yyjson_obj_get(gbt_obj, "wins"));
            uint64_t json_last_us = yyjson_get_uint(yyjson_obj_get(gbt_obj, "last_us"));
            uint64_t json_last_tx = yyjson_get_uint(yyjson_obj_get(gbt_obj, "last_tx_count"));
            uint64_t json_avg_us = yyjson_get_uint(yyjson_obj_get(gbt_obj, "avg_us"));

            /* Check gbt_wins */
            if (json_wins != expected[n].gbt_wins) {
                fprintf(stderr, "  FAIL trial %d: node %d wins mismatch: "
                        "got %llu, expected %u\n",
                        trial, n, (unsigned long long)json_wins,
                        expected[n].gbt_wins);
                fprintf(stderr, "  (seed=%ld, trial=%d, node_count=%d, "
                        "num_recordings=%d)\n",
                        seed, trial, node_count, num_recordings);
                yyjson_doc_free(doc);
                stats_destroy(s);
                return -1;
            }

            /* Check avg_us = total / wins */
            uint64_t expected_avg = 0;
            if (expected[n].gbt_wins > 0)
                expected_avg = expected[n].gbt_total_time_us / expected[n].gbt_wins;
            if (json_avg_us != expected_avg) {
                fprintf(stderr, "  FAIL trial %d: node %d avg_us mismatch: "
                        "got %llu, expected %llu\n",
                        trial, n, (unsigned long long)json_avg_us,
                        (unsigned long long)expected_avg);
                fprintf(stderr, "  (seed=%ld, trial=%d, node_count=%d)\n",
                        seed, trial, node_count);
                yyjson_doc_free(doc);
                stats_destroy(s);
                return -1;
            }

            /* Check last_us */
            if (json_last_us != expected[n].gbt_last_response_us) {
                fprintf(stderr, "  FAIL trial %d: node %d last_us mismatch: "
                        "got %llu, expected %llu\n",
                        trial, n, (unsigned long long)json_last_us,
                        (unsigned long long)expected[n].gbt_last_response_us);
                fprintf(stderr, "  (seed=%ld, trial=%d, node_count=%d)\n",
                        seed, trial, node_count);
                yyjson_doc_free(doc);
                stats_destroy(s);
                return -1;
            }

            /* Check gbt_last_tx_count */
            if (json_last_tx != expected[n].gbt_last_tx_count) {
                fprintf(stderr, "  FAIL trial %d: node %d last_tx_count mismatch: "
                        "got %llu, expected %u\n",
                        trial, n, (unsigned long long)json_last_tx,
                        expected[n].gbt_last_tx_count);
                fprintf(stderr, "  (seed=%ld, trial=%d, node_count=%d)\n",
                        seed, trial, node_count);
                yyjson_doc_free(doc);
                stats_destroy(s);
                return -1;
            }
        }

        yyjson_doc_free(doc);
        stats_destroy(s);
        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Out-of-bounds node indices are safely ignored.
 * Generate recordings with invalid node indices (negative, >= node_count);
 * verify they do not corrupt any per-node stats.
 */
static int
test_property_oob_ignored(long seed)
{
    printf("  property: out-of-bounds node indices ignored (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int node_count = 1 + (int)(lrand48() % MAX_NODES);

        stats_t *s = stats_create(node_count);
        if (!s) {
            fprintf(stderr, "  FAIL trial %d: stats_create returned NULL\n", trial);
            return -1;
        }

        /* Apply some valid recordings first */
        expected_node_t expected[MAX_NODES];
        memset(expected, 0, sizeof(expected));

        int num_valid = 5 + (int)(lrand48() % 10);
        for (int r = 0; r < num_valid; r++) {
            int node_idx = (int)(lrand48() % node_count);
            uint64_t response_time_us = 1 + (uint64_t)(lrand48() % 1000000);
            uint32_t tx_count = (uint32_t)(lrand48() % 50000);

            stats_record_gbt(s, node_idx, response_time_us, tx_count, 0);
            stats_record_race_win(s, node_idx);
            expected[node_idx].gbt_wins++;
            expected[node_idx].gbt_total_time_us += response_time_us;
            expected[node_idx].gbt_last_tx_count = tx_count;
            expected[node_idx].gbt_last_response_us = response_time_us;
        }

        /* Now apply out-of-bounds recordings */
        int num_oob = 5 + (int)(lrand48() % 10);
        for (int r = 0; r < num_oob; r++) {
            int bad_idx;
            if (lrand48() % 2 == 0)
                bad_idx = -(1 + (int)(lrand48() % 100));
            else
                bad_idx = node_count + (int)(lrand48() % 100);

            uint64_t response_time_us = 1 + (uint64_t)(lrand48() % 1000000);
            uint32_t tx_count = (uint32_t)(lrand48() % 50000);
            stats_record_gbt(s, bad_idx, response_time_us, tx_count, 0);
        }

        /* Serialize and verify per-node stats are unchanged */
        config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.node_count = node_count;
        for (int i = 0; i < node_count; i++)
            snprintf(cfg.nodes[i].label, sizeof(cfg.nodes[i].label), "n%d", i);

        stats_serialize_ctx_t ctx = { .stats = s, .cfg = &cfg, .proxy = NULL };
        char buf[BUF_SIZE];
        int len = stats_serialize_json(buf, BUF_SIZE, &ctx);
        if (len <= 0) {
            fprintf(stderr, "  FAIL trial %d: serialize returned %d\n", trial, len);
            stats_destroy(s);
            return -1;
        }

        yyjson_doc *doc = yyjson_read(buf, (size_t)len, 0);
        if (!doc) {
            fprintf(stderr, "  FAIL trial %d: yyjson_read failed\n", trial);
            stats_destroy(s);
            return -1;
        }

        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *nodes = yyjson_obj_get(root, "nodes");

        for (int n = 0; n < node_count; n++) {
            yyjson_val *node_obj = yyjson_arr_get(nodes, (size_t)n);
            yyjson_val *gbt_obj = yyjson_obj_get(node_obj, "race_gbt");

            uint64_t json_wins = yyjson_get_uint(yyjson_obj_get(gbt_obj, "wins"));
            uint64_t json_last_tx = yyjson_get_uint(yyjson_obj_get(gbt_obj, "last_tx_count"));

            if (json_wins != expected[n].gbt_wins) {
                fprintf(stderr, "  FAIL trial %d: node %d wins corrupted "
                        "by OOB: got %llu, expected %u\n",
                        trial, n, (unsigned long long)json_wins,
                        expected[n].gbt_wins);
                fprintf(stderr, "  (seed=%ld, trial=%d, node_count=%d)\n",
                        seed, trial, node_count);
                yyjson_doc_free(doc);
                stats_destroy(s);
                return -1;
            }

            if (json_last_tx != expected[n].gbt_last_tx_count) {
                fprintf(stderr, "  FAIL trial %d: node %d last_tx_count corrupted "
                        "by OOB: got %llu, expected %u\n",
                        trial, n, (unsigned long long)json_last_tx,
                        expected[n].gbt_last_tx_count);
                fprintf(stderr, "  (seed=%ld, trial=%d, node_count=%d)\n",
                        seed, trial, node_count);
                yyjson_doc_free(doc);
                stats_destroy(s);
                return -1;
            }

            /* Verify avg matches expected total/wins */
            uint64_t json_avg = yyjson_get_uint(yyjson_obj_get(gbt_obj, "avg_us"));
            uint64_t expected_avg = 0;
            if (expected[n].gbt_wins > 0)
                expected_avg = expected[n].gbt_total_time_us / expected[n].gbt_wins;
            if (json_avg != expected_avg) {
                fprintf(stderr, "  FAIL trial %d: node %d avg_us corrupted "
                        "by OOB: got %llu, expected %llu\n",
                        trial, n, (unsigned long long)json_avg,
                        (unsigned long long)expected_avg);
                fprintf(stderr, "  (seed=%ld, trial=%d, node_count=%d)\n",
                        seed, trial, node_count);
                yyjson_doc_free(doc);
                stats_destroy(s);
                return -1;
            }
        }

        yyjson_doc_free(doc);
        stats_destroy(s);
        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Node isolation — recordings for one node do not affect
 * any other node's statistics.
 */
static int
test_property_node_isolation(long seed)
{
    printf("  property: node isolation (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int node_count = 2 + (int)(lrand48() % (MAX_NODES - 1));

        stats_t *s = stats_create(node_count);
        if (!s) {
            fprintf(stderr, "  FAIL trial %d: stats_create returned NULL\n", trial);
            return -1;
        }

        /* Pick one target node to record to */
        int target_node = (int)(lrand48() % node_count);

        int num_recordings = 10 + (int)(lrand48() % 20);
        for (int r = 0; r < num_recordings; r++) {
            uint64_t response_time_us = 1 + (uint64_t)(lrand48() % 5000000);
            uint32_t tx_count = (uint32_t)(lrand48() % 80000);
            stats_record_gbt(s, target_node, response_time_us, tx_count, 0);
            stats_record_race_win(s, target_node);
        }

        /* Serialize and verify all other nodes are untouched */
        config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.node_count = node_count;
        for (int i = 0; i < node_count; i++)
            snprintf(cfg.nodes[i].label, sizeof(cfg.nodes[i].label), "n%d", i);

        stats_serialize_ctx_t ctx = { .stats = s, .cfg = &cfg, .proxy = NULL };
        char buf[BUF_SIZE];
        int len = stats_serialize_json(buf, BUF_SIZE, &ctx);
        if (len <= 0) {
            fprintf(stderr, "  FAIL trial %d: serialize returned %d\n", trial, len);
            stats_destroy(s);
            return -1;
        }

        yyjson_doc *doc = yyjson_read(buf, (size_t)len, 0);
        if (!doc) {
            fprintf(stderr, "  FAIL trial %d: yyjson_read failed\n", trial);
            stats_destroy(s);
            return -1;
        }

        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *nodes = yyjson_obj_get(root, "nodes");

        for (int n = 0; n < node_count; n++) {
            if (n == target_node)
                continue;

            yyjson_val *node_obj = yyjson_arr_get(nodes, (size_t)n);
            yyjson_val *gbt_obj = yyjson_obj_get(node_obj, "race_gbt");

            uint64_t json_wins = yyjson_get_uint(yyjson_obj_get(gbt_obj, "wins"));
            uint64_t json_avg = yyjson_get_uint(yyjson_obj_get(gbt_obj, "avg_us"));
            uint64_t json_last_tx = yyjson_get_uint(yyjson_obj_get(gbt_obj, "last_tx_count"));

            if (json_wins != 0 || json_avg != 0 || json_last_tx != 0) {
                fprintf(stderr, "  FAIL trial %d: node %d affected by "
                        "recordings to node %d\n", trial, n, target_node);
                fprintf(stderr, "  (seed=%ld, trial=%d, node_count=%d)\n",
                        seed, trial, node_count);
                fprintf(stderr, "  node %d: wins=%llu avg=%llu last_tx=%llu\n",
                        n, (unsigned long long)json_wins,
                        (unsigned long long)json_avg,
                        (unsigned long long)json_last_tx);
                yyjson_doc_free(doc);
                stats_destroy(s);
                return -1;
            }
        }

        yyjson_doc_free(doc);
        stats_destroy(s);
        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/* Feature: rpcrace, Property 14: Statistics Accumulation */
int
main(int argc, char *argv[])
{
    long seed;
    if (argc > 1) {
        seed = atol(argv[1]);
    } else {
        seed = (long)time(NULL);
    }

    printf("test_stats_accumulation (seed=%ld):\n", seed);

    int failures = 0;

    if (test_property_stats_accumulation(seed) < 0)
        failures++;
    if (test_property_oob_ignored(seed) < 0)
        failures++;
    if (test_property_node_isolation(seed) < 0)
        failures++;

    if (failures == 0) {
        printf("  All property tests passed\n");
        return 0;
    } else {
        printf("  %d property test(s) FAILED\n", failures);
        return 1;
    }
}
