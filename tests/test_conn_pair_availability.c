/* test_conn_pair_availability.c — Property test for conn_pair availability
 *
 * Feature: conn-pair-refactor, Property 1: Availability reflects active connection state
 *
 * For any conn_pair_t in any combination of slot states:
 *   - conn_pair_get_active(pair) SHALL always return non-NULL and point to
 *     &pair->slots[pair->active_idx].
 *   - conn_pair_is_available(pair) SHALL return true if and only if
 *     pair->slots[pair->active_idx].state == CONN_CONNECTED.
 *
 * Validates: Requirements 1.1, 1.2, 6.1
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

#define NUM_TRIALS 200
#define NUM_STATES 5

/* All valid conn_state_t values */
static const conn_state_t ALL_STATES[NUM_STATES] = {
    CONN_DISCONNECTED,  /* 0 */
    CONN_CONNECTING,    /* 1 */
    CONN_CONNECTED,     /* 2 */
    CONN_SENDING,       /* 3 */
    CONN_RECEIVING      /* 4 */
};

/*
 * Property 1: Availability reflects active connection state
 *
 * For each trial:
 *   - Pick a random active_idx (0 or 1)
 *   - Pick random states for both slots
 *   - Set the conn_pair_t fields directly (no init/connect needed)
 *   - Verify conn_pair_get_active ALWAYS returns non-NULL and points to active slot
 *   - Verify conn_pair_is_available returns true iff slots[active_idx].state == CONN_CONNECTED
 */
static int
test_property_availability(unsigned int seed)
{
    printf("  property: availability reflects active connection state "
           "(seed=%u, %d trials)\n", seed, NUM_TRIALS);
    srand(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Set up a conn_pair_t with random state (no real init needed) */
        conn_pair_t pair;
        memset(&pair, 0, sizeof(pair));

        /* Random active_idx: 0 or 1 */
        pair.active_idx = rand() % 2;

        /* Random state for slot 0 */
        pair.slots[0].state = ALL_STATES[rand() % NUM_STATES];

        /* Random state for slot 1 */
        pair.slots[1].state = ALL_STATES[rand() % NUM_STATES];

        /* Determine expected availability */
        bool expected_available =
            (pair.slots[pair.active_idx].state == CONN_CONNECTED);

        /* Test conn_pair_is_available */
        bool actual_available = conn_pair_is_available(&pair);
        if (actual_available != expected_available) {
            fprintf(stderr, "  FAIL trial %d: conn_pair_is_available mismatch\n",
                    trial);
            fprintf(stderr, "    active_idx=%d, slot[0].state=%d, slot[1].state=%d\n",
                    pair.active_idx, pair.slots[0].state, pair.slots[1].state);
            fprintf(stderr, "    expected=%d, got=%d\n",
                    expected_available, actual_available);
            fprintf(stderr, "    (seed=%u)\n", seed);
            return -1;
        }

        /* Test conn_pair_get_active: ALWAYS returns non-NULL */
        upstream_conn_t *active = conn_pair_get_active(&pair);
        if (active == NULL) {
            fprintf(stderr, "  FAIL trial %d: conn_pair_get_active returned NULL\n",
                    trial);
            fprintf(stderr, "    active_idx=%d, slot[0].state=%d, slot[1].state=%d\n",
                    pair.active_idx, pair.slots[0].state, pair.slots[1].state);
            fprintf(stderr, "    (seed=%u)\n", seed);
            return -1;
        }

        /* Verify it always points to the active slot */
        upstream_conn_t *expected_ptr = &pair.slots[pair.active_idx];
        if (active != expected_ptr) {
            fprintf(stderr, "  FAIL trial %d: conn_pair_get_active wrong pointer\n",
                    trial);
            fprintf(stderr, "    active_idx=%d, expected slot %d address\n",
                    pair.active_idx, pair.active_idx);
            fprintf(stderr, "    (seed=%u)\n", seed);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Exhaustive check: verify property holds for ALL combinations.
 * 2 active_idx * 5 slot0 states * 5 slot1 states = 50 combinations.
 */
static int
test_exhaustive_availability(void)
{
    printf("  exhaustive: all 50 state combinations\n");

    int checked = 0;

    for (int active_idx = 0; active_idx <= 1; active_idx++) {
        for (int s0 = 0; s0 < NUM_STATES; s0++) {
            for (int s1 = 0; s1 < NUM_STATES; s1++) {
                conn_pair_t pair;
                memset(&pair, 0, sizeof(pair));

                pair.active_idx = active_idx;
                pair.slots[0].state = ALL_STATES[s0];
                pair.slots[1].state = ALL_STATES[s1];

                bool expected_available =
                    (pair.slots[pair.active_idx].state == CONN_CONNECTED);

                /* conn_pair_is_available */
                bool actual_available = conn_pair_is_available(&pair);
                if (actual_available != expected_available) {
                    fprintf(stderr, "  FAIL exhaustive: conn_pair_is_available\n");
                    fprintf(stderr, "    active_idx=%d, slot[0].state=%d, "
                            "slot[1].state=%d\n",
                            active_idx, ALL_STATES[s0], ALL_STATES[s1]);
                    fprintf(stderr, "    expected=%d, got=%d\n",
                            expected_available, actual_available);
                    return -1;
                }

                /* conn_pair_get_active: ALWAYS returns non-NULL */
                upstream_conn_t *active = conn_pair_get_active(&pair);
                if (active == NULL) {
                    fprintf(stderr, "  FAIL exhaustive: conn_pair_get_active "
                            "returned NULL\n");
                    fprintf(stderr, "    active_idx=%d, slot[0].state=%d, "
                            "slot[1].state=%d\n",
                            active_idx, ALL_STATES[s0], ALL_STATES[s1]);
                    return -1;
                }

                /* Pointer correctness: must always point to active slot */
                if (active != &pair.slots[pair.active_idx]) {
                    fprintf(stderr, "  FAIL exhaustive: wrong pointer\n");
                    fprintf(stderr, "    active_idx=%d\n", active_idx);
                    return -1;
                }

                checked++;
            }
        }
    }

    printf("    %d/%d combinations verified\n", checked, checked);
    return 0;
}

/* Feature: conn-pair-refactor, Property 1: Availability reflects active connection state */
int
main(int argc, char *argv[])
{
    unsigned int seed;
    if (argc > 1) {
        seed = (unsigned int)atol(argv[1]);
    } else {
        seed = (unsigned int)time(NULL);
    }

    printf("test_conn_pair_availability (seed=%u):\n", seed);

    int failures = 0;

    if (test_property_availability(seed) < 0)
        failures++;
    if (test_exhaustive_availability() < 0)
        failures++;

    if (failures == 0) {
        printf("  All property tests passed\n");
        return 0;
    } else {
        printf("  %d property test(s) FAILED\n", failures);
        return 1;
    }
}
