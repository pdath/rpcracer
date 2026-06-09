/* test_broadcast_targets.c — Property test for broadcast targeting
 *
 * Feature: conn-pair-refactor, Property 8: Broadcast targets exactly available nodes
 *
 * For any set of conn_pairs with varying availability, a broadcast dispatch SHALL
 * send the request to every conn_pair where conn_pair_is_available() returns true,
 * and SHALL skip every conn_pair where it returns false.
 *
 * Validates: Requirements 8.1, 8.2
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand/rand),
 * minimum 200 trials, seed printed for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include "../src/conn_pair.h"

#define NUM_TRIALS     200
#define MAX_TEST_NODES 16
#define NUM_STATES     5

/* All valid conn_state_t values */
static const conn_state_t ALL_STATES[NUM_STATES] = {
    CONN_DISCONNECTED,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_SENDING,
    CONN_RECEIVING
};

/*
 * Simulate the broadcast targeting logic from dispatch_fanout:
 *
 *   for (int i = 0; i < pair_count; i++) {
 *       if (!conn_pair_is_available(&pairs[i])) continue;  // skip unavailable
 *       upstream_conn_t *conn = conn_pair_get_active(&pairs[i]);
 *       // send to conn ...
 *   }
 *
 * Property 8: The set of pairs targeted by this loop must be EXACTLY the set
 * where conn_pair_is_available returns true (i.e., active slot is CONN_CONNECTED).
 */
static int
test_property_broadcast_targets(unsigned int seed)
{
    printf("  property: broadcast targets exactly available nodes "
           "(seed=%u, %d trials)\n", seed, NUM_TRIALS);
    srand(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Random number of nodes: 1..MAX_TEST_NODES */
        int pair_count = 1 + (rand() % MAX_TEST_NODES);

        /* Set up conn_pair_t array with random states */
        conn_pair_t pairs[MAX_TEST_NODES];
        memset(pairs, 0, sizeof(pairs));

        /* Track expected availability for each pair */
        bool expected_available[MAX_TEST_NODES];

        for (int i = 0; i < pair_count; i++) {
            /* Random active_idx: 0 or 1 */
            pairs[i].active_idx = rand() % 2;

            /* Random state for both slots */
            pairs[i].slots[0].state = ALL_STATES[rand() % NUM_STATES];
            pairs[i].slots[1].state = ALL_STATES[rand() % NUM_STATES];

            /* Expected: available iff active slot is CONN_CONNECTED */
            expected_available[i] =
                (pairs[i].slots[pairs[i].active_idx].state == CONN_CONNECTED);
        }

        /* Simulate broadcast dispatch using conn_pair_is_available */
        bool actually_targeted[MAX_TEST_NODES];
        memset(actually_targeted, 0, sizeof(actually_targeted));
        int sent_count = 0;

        for (int i = 0; i < pair_count; i++) {
            if (conn_pair_is_available(&pairs[i])) {
                actually_targeted[i] = true;
                sent_count++;
            }
        }

        /* Verify: targeted set matches exactly the available set */
        for (int i = 0; i < pair_count; i++) {
            if (actually_targeted[i] != expected_available[i]) {
                fprintf(stderr, "  FAIL trial %d: pair[%d] targeting mismatch\n",
                        trial, i);
                fprintf(stderr, "    active_idx=%d, slot[0].state=%d, slot[1].state=%d\n",
                        pairs[i].active_idx,
                        pairs[i].slots[0].state,
                        pairs[i].slots[1].state);
                fprintf(stderr, "    expected_targeted=%d, actually_targeted=%d\n",
                        expected_available[i], actually_targeted[i]);
                fprintf(stderr, "    (seed=%u, pair_count=%d)\n", seed, pair_count);
                return -1;
            }
        }

        /* Also verify that conn_pair_get_active always returns non-NULL */
        for (int i = 0; i < pair_count; i++) {
            upstream_conn_t *active = conn_pair_get_active(&pairs[i]);
            if (active == NULL) {
                fprintf(stderr, "  FAIL trial %d: pair[%d] conn_pair_get_active "
                        "returned NULL\n", trial, i);
                fprintf(stderr, "    (seed=%u)\n", seed);
                return -1;
            }
            /* Verify pointer points to active slot */
            if (active != &pairs[i].slots[pairs[i].active_idx]) {
                fprintf(stderr, "  FAIL trial %d: pair[%d] conn_pair_get_active "
                        "wrong pointer\n", trial, i);
                fprintf(stderr, "    (seed=%u)\n", seed);
                return -1;
            }
        }

        /* Verify count: sent_count should equal number of available pairs */
        int expected_count = 0;
        for (int i = 0; i < pair_count; i++) {
            if (expected_available[i])
                expected_count++;
        }
        if (sent_count != expected_count) {
            fprintf(stderr, "  FAIL trial %d: sent_count mismatch\n", trial);
            fprintf(stderr, "    expected=%d, got=%d\n", expected_count, sent_count);
            fprintf(stderr, "    (seed=%u, pair_count=%d)\n", seed, pair_count);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Edge case: all nodes unavailable — broadcast targets zero nodes.
 */
static int
test_all_unavailable(void)
{
    printf("  edge case: all nodes unavailable\n");

    conn_pair_t pairs[MAX_TEST_NODES];
    memset(pairs, 0, sizeof(pairs));

    /* Set all active slots to non-CONNECTED states */
    conn_state_t non_connected[] = {
        CONN_DISCONNECTED, CONN_CONNECTING, CONN_SENDING, CONN_RECEIVING
    };

    for (int i = 0; i < MAX_TEST_NODES; i++) {
        pairs[i].active_idx = i % 2;
        pairs[i].slots[pairs[i].active_idx].state = non_connected[i % 4];
        pairs[i].slots[1 - pairs[i].active_idx].state = CONN_CONNECTED;
    }

    /* Simulate broadcast using conn_pair_is_available */
    int sent_count = 0;
    for (int i = 0; i < MAX_TEST_NODES; i++) {
        if (conn_pair_is_available(&pairs[i]))
            sent_count++;
    }

    if (sent_count != 0) {
        fprintf(stderr, "  FAIL: all-unavailable should target 0, got %d\n",
                sent_count);
        return -1;
    }

    printf("    verified: 0/%d nodes targeted\n", MAX_TEST_NODES);
    return 0;
}

/*
 * Edge case: all nodes available — broadcast targets all nodes.
 */
static int
test_all_available(void)
{
    printf("  edge case: all nodes available\n");

    conn_pair_t pairs[MAX_TEST_NODES];
    memset(pairs, 0, sizeof(pairs));

    for (int i = 0; i < MAX_TEST_NODES; i++) {
        pairs[i].active_idx = i % 2;
        pairs[i].slots[pairs[i].active_idx].state = CONN_CONNECTED;
        pairs[i].slots[1 - pairs[i].active_idx].state = CONN_DISCONNECTED;
    }

    /* Simulate broadcast using conn_pair_is_available */
    int sent_count = 0;
    for (int i = 0; i < MAX_TEST_NODES; i++) {
        if (conn_pair_is_available(&pairs[i]))
            sent_count++;
    }

    if (sent_count != MAX_TEST_NODES) {
        fprintf(stderr, "  FAIL: all-available should target %d, got %d\n",
                MAX_TEST_NODES, sent_count);
        return -1;
    }

    printf("    verified: %d/%d nodes targeted\n", MAX_TEST_NODES, MAX_TEST_NODES);
    return 0;
}

/*
 * Edge case: single node — either targeted or not based on state.
 */
static int
test_single_node(void)
{
    printf("  edge case: single node available/unavailable\n");

    /* Single available node */
    conn_pair_t pair;
    memset(&pair, 0, sizeof(pair));
    pair.active_idx = 0;
    pair.slots[0].state = CONN_CONNECTED;

    if (!conn_pair_is_available(&pair)) {
        fprintf(stderr, "  FAIL: single available node not targeted\n");
        return -1;
    }

    /* Verify conn_pair_get_active still returns non-NULL */
    upstream_conn_t *conn = conn_pair_get_active(&pair);
    if (conn == NULL) {
        fprintf(stderr, "  FAIL: conn_pair_get_active returned NULL\n");
        return -1;
    }

    /* Single unavailable node */
    pair.slots[0].state = CONN_DISCONNECTED;
    if (conn_pair_is_available(&pair)) {
        fprintf(stderr, "  FAIL: single unavailable node was targeted\n");
        return -1;
    }

    /* conn_pair_get_active should still return non-NULL even when unavailable */
    conn = conn_pair_get_active(&pair);
    if (conn == NULL) {
        fprintf(stderr, "  FAIL: conn_pair_get_active returned NULL for "
                "unavailable node\n");
        return -1;
    }

    printf("    verified: single node correctly targeted/skipped\n");
    return 0;
}

/* Feature: conn-pair-refactor, Property 8: Broadcast targets exactly available nodes */
int
main(int argc, char *argv[])
{
    unsigned int seed;
    if (argc > 1) {
        seed = (unsigned int)atol(argv[1]);
    } else {
        seed = (unsigned int)time(NULL);
    }

    printf("test_broadcast_targets (seed=%u):\n", seed);

    int failures = 0;

    if (test_property_broadcast_targets(seed) < 0)
        failures++;
    if (test_all_unavailable() < 0)
        failures++;
    if (test_all_available() < 0)
        failures++;
    if (test_single_node() < 0)
        failures++;

    if (failures == 0) {
        printf("  All property tests passed\n");
        return 0;
    } else {
        printf("  %d property test(s) FAILED\n", failures);
        return 1;
    }
}
