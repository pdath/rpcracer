/* test_conn_pair_rotation.c — Property test for timer-driven rotation preconditions
 *
 * Feature: conn-pair-refactor, Property 3: Timer-driven rotation respects preconditions
 *
 * **Validates: Requirements 3.2, 3.3, 9.1, 9.2, 9.4**
 *
 * Property: For any conn_pair_t state, a timer-driven rotation SHALL execute
 * if and only if: (a) swap_required is true, AND (b) the active connection is
 * in CONN_CONNECTED state (idle), AND (c) the standby connection is in
 * CONN_CONNECTED state. When any precondition is not met, the rotation SHALL
 * be deferred and swap_required SHALL remain true.
 *
 * Test approach: Only test the deferred cases (preconditions NOT met). When
 * any precondition is not satisfied, conn_pair_tick must not change active_idx
 * and swap_required must retain its value. The "rotation executes" case requires
 * a valid event_loop and is tested separately in unit tests (task 7.3).
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand/rand), minimum 200
 * iterations, seed printed for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <assert.h>

/* Include conn_pair and rpc_conn headers for types */
#include "../src/conn_pair.h"

#define NUM_TRIALS 200

/* Dummy node config for tests that hit the rpc_conn_connect recovery path.
 * Uses an invalid host so inet_pton fails and connect returns -1 immediately. */
static node_config_t dummy_config = {
    .host = "invalid",
    .rpc_port = 1,
    .zmq_port = 0,
    .label = "test"
};

/* All possible conn_state_t values */
static const conn_state_t ALL_STATES[] = {
    CONN_DISCONNECTED,  /* 0 */
    CONN_CONNECTING,    /* 1 */
    CONN_CONNECTED,     /* 2 */
    CONN_SENDING,       /* 3 */
    CONN_RECEIVING      /* 4 */
};
#define NUM_STATES 5

/* Pick a random conn_state_t */
static conn_state_t
random_state(void)
{
    return ALL_STATES[rand() % NUM_STATES];
}

/* Pick a random conn_state_t that is NOT CONN_CONNECTED */
static conn_state_t
random_non_connected_state(void)
{
    conn_state_t s;
    do {
        s = random_state();
    } while (s == CONN_CONNECTED);
    return s;
}

/* Initialize a conn_pair_t for testing without a real event loop.
 * Sets up minimal fields needed for conn_pair_tick to evaluate preconditions.
 * Uses dummy_config so rpc_conn_connect calls (recovery path) don't segfault
 * on config->label access — inet_pton("invalid") fails, returning -1 cleanly. */
static void
init_test_pair(conn_pair_t *pair, bool swap_required, int active_idx,
               conn_state_t active_state, conn_state_t standby_state)
{
    memset(pair, 0, sizeof(*pair));
    pair->active_idx = active_idx;
    pair->swap_required = swap_required;
    pair->rotation_timer_fd = -1;
    pair->loop = NULL;
    pair->config = &dummy_config;
    pair->node_index = 0;

    int standby_idx = 1 - active_idx;
    pair->slots[active_idx].state = active_state;
    pair->slots[active_idx].fd = -1;
    pair->slots[active_idx].loop = NULL;
    pair->slots[active_idx].config = &dummy_config;

    pair->slots[standby_idx].state = standby_state;
    pair->slots[standby_idx].fd = -1;
    pair->slots[standby_idx].loop = NULL;
    pair->slots[standby_idx].config = &dummy_config;
}

/*
 * Property P1: When swap_required is false, conn_pair_tick is a no-op.
 *
 * For any active_state and standby_state, if swap_required == false,
 * conn_pair_tick shall not change active_idx or swap_required.
 */
static int
test_no_swap_required(long seed)
{
    printf("  property: swap_required=false => tick is no-op "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand((unsigned int)seed);

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        conn_state_t active_state = random_state();
        conn_state_t standby_state = random_state();
        int active_idx = rand() % 2;

        conn_pair_t pair;
        init_test_pair(&pair, false, active_idx, active_state, standby_state);

        int orig_active_idx = pair.active_idx;
        bool orig_swap_required = pair.swap_required;
        conn_state_t orig_active_state = pair.slots[active_idx].state;
        conn_state_t orig_standby_state = pair.slots[1 - active_idx].state;

        conn_pair_tick(&pair);

        /* Verify no state changed */
        if (pair.active_idx != orig_active_idx) {
            fprintf(stderr, "  FAIL trial %d: active_idx changed from %d to %d "
                    "(swap_required=false, active=%d, standby=%d)\n",
                    trial, orig_active_idx, pair.active_idx,
                    active_state, standby_state);
            return -1;
        }
        if (pair.swap_required != orig_swap_required) {
            fprintf(stderr, "  FAIL trial %d: swap_required changed from %d to %d "
                    "(swap_required=false, active=%d, standby=%d)\n",
                    trial, orig_swap_required, pair.swap_required,
                    active_state, standby_state);
            return -1;
        }
        if (pair.slots[active_idx].state != orig_active_state) {
            fprintf(stderr, "  FAIL trial %d: active state changed from %d to %d\n",
                    trial, orig_active_state, pair.slots[active_idx].state);
            return -1;
        }
        if (pair.slots[1 - active_idx].state != orig_standby_state) {
            fprintf(stderr, "  FAIL trial %d: standby state changed from %d to %d\n",
                    trial, orig_standby_state, pair.slots[1 - active_idx].state);
            return -1;
        }
    }

    printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/*
 * Property P2: When active is NOT CONN_CONNECTED, rotation is deferred.
 *
 * For any non-CONNECTED active state and any standby state, if swap_required
 * is true, conn_pair_tick shall not change active_idx and swap_required shall
 * remain true.
 *
 * Note: conn_pair_tick may attempt rpc_conn_connect on the standby if standby
 * is CONN_DISCONNECTED and active is not connected (recovery path). Since we
 * have loop=NULL, rpc_conn_connect will fail silently. The key invariant is
 * that active_idx and swap_required are preserved.
 */
static int
test_active_not_connected_defers(long seed)
{
    printf("  property: active!=CONNECTED => rotation deferred "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand((unsigned int)seed);

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        conn_state_t active_state = random_non_connected_state();
        conn_state_t standby_state = random_state();
        int active_idx = rand() % 2;

        conn_pair_t pair;
        init_test_pair(&pair, true, active_idx, active_state, standby_state);

        int orig_active_idx = pair.active_idx;

        conn_pair_tick(&pair);

        /* active_idx must not change */
        if (pair.active_idx != orig_active_idx) {
            fprintf(stderr, "  FAIL trial %d: active_idx changed from %d to %d "
                    "(active=%d, standby=%d)\n",
                    trial, orig_active_idx, pair.active_idx,
                    active_state, standby_state);
            return -1;
        }
        /* swap_required must remain true (rotation was deferred) */
        if (!pair.swap_required) {
            fprintf(stderr, "  FAIL trial %d: swap_required cleared "
                    "(active=%d, standby=%d)\n",
                    trial, active_state, standby_state);
            return -1;
        }
    }

    printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/*
 * Property P3: When active is CONN_CONNECTED but standby is NOT CONN_CONNECTED,
 * rotation is deferred.
 *
 * For any non-CONNECTED standby state, if swap_required is true and active is
 * CONN_CONNECTED, conn_pair_tick shall not change active_idx and swap_required
 * shall remain true.
 *
 * Note: when standby is CONN_DISCONNECTED, conn_pair_tick may attempt
 * rpc_conn_connect on it (which will fail with loop=NULL). The key invariant
 * is that active_idx and swap_required are preserved.
 */
static int
test_standby_not_connected_defers(long seed)
{
    printf("  property: standby!=CONNECTED => rotation deferred "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand((unsigned int)seed);

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        conn_state_t standby_state = random_non_connected_state();
        int active_idx = rand() % 2;

        conn_pair_t pair;
        init_test_pair(&pair, true, active_idx, CONN_CONNECTED, standby_state);

        int orig_active_idx = pair.active_idx;

        conn_pair_tick(&pair);

        /* active_idx must not change */
        if (pair.active_idx != orig_active_idx) {
            fprintf(stderr, "  FAIL trial %d: active_idx changed from %d to %d "
                    "(active=CONNECTED, standby=%d)\n",
                    trial, orig_active_idx, pair.active_idx, standby_state);
            return -1;
        }
        /* swap_required must remain true */
        if (!pair.swap_required) {
            fprintf(stderr, "  FAIL trial %d: swap_required cleared "
                    "(active=CONNECTED, standby=%d)\n",
                    trial, standby_state);
            return -1;
        }
    }

    printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/*
 * Property P4: When active is busy (SENDING or RECEIVING), rotation is deferred
 * even if standby is CONN_CONNECTED.
 *
 * This tests the specific case where standby IS ready but active is busy.
 * swap_required must remain true and active_idx must not change.
 */
static int
test_active_busy_defers(long seed)
{
    printf("  property: active busy (SENDING/RECEIVING) => rotation deferred "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand((unsigned int)seed);

    conn_state_t busy_states[] = { CONN_SENDING, CONN_RECEIVING };

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        conn_state_t active_state = busy_states[rand() % 2];
        int active_idx = rand() % 2;

        conn_pair_t pair;
        init_test_pair(&pair, true, active_idx, active_state, CONN_CONNECTED);

        int orig_active_idx = pair.active_idx;

        conn_pair_tick(&pair);

        /* active_idx must not change */
        if (pair.active_idx != orig_active_idx) {
            fprintf(stderr, "  FAIL trial %d: active_idx changed from %d to %d "
                    "(active=%d, standby=CONNECTED)\n",
                    trial, orig_active_idx, pair.active_idx, active_state);
            return -1;
        }
        /* swap_required must remain true */
        if (!pair.swap_required) {
            fprintf(stderr, "  FAIL trial %d: swap_required cleared "
                    "(active=%d, standby=CONNECTED)\n",
                    trial, active_state);
            return -1;
        }
    }

    printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/*
 * Property P5: Comprehensive random tuple generation.
 *
 * Generate random (swap_required, active_state, standby_state) tuples.
 * For all cases where preconditions are NOT fully met, verify:
 *   - active_idx does not change
 *   - swap_required retains its original value
 *
 * Preconditions for rotation to execute:
 *   swap_required == true AND active == CONN_CONNECTED AND standby == CONN_CONNECTED
 *
 * This property covers ALL deferred cases in a single comprehensive test.
 */
static int
test_comprehensive_precondition_check(long seed)
{
    printf("  property: comprehensive precondition check — deferred cases "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand((unsigned int)seed);

    int tested = 0;

    for (int trial = 0; trial < NUM_TRIALS * 5; trial++) {
        bool swap_required = (rand() % 2 == 0);
        conn_state_t active_state = random_state();
        conn_state_t standby_state = random_state();
        int active_idx = rand() % 2;

        /* Skip the case where ALL preconditions are met — that requires
         * a valid loop pointer for rpc_conn_disconnect/connect calls */
        if (swap_required && active_state == CONN_CONNECTED &&
            standby_state == CONN_CONNECTED) {
            continue;
        }

        conn_pair_t pair;
        init_test_pair(&pair, swap_required, active_idx, active_state, standby_state);

        int orig_active_idx = pair.active_idx;
        bool orig_swap_required = pair.swap_required;

        conn_pair_tick(&pair);

        /* active_idx must not change when preconditions are not met */
        if (pair.active_idx != orig_active_idx) {
            fprintf(stderr, "  FAIL trial %d: active_idx changed from %d to %d "
                    "(swap_required=%d, active=%d, standby=%d)\n",
                    trial, orig_active_idx, pair.active_idx,
                    swap_required, active_state, standby_state);
            return -1;
        }
        /* swap_required must retain its original value */
        if (pair.swap_required != orig_swap_required) {
            fprintf(stderr, "  FAIL trial %d: swap_required changed from %d to %d "
                    "(swap_required=%d, active=%d, standby=%d)\n",
                    trial, orig_swap_required, pair.swap_required,
                    swap_required, active_state, standby_state);
            return -1;
        }

        tested++;
        if (tested >= NUM_TRIALS)
            break;
    }

    if (tested < NUM_TRIALS) {
        fprintf(stderr, "  WARNING: only %d/%d deferred cases generated\n",
                tested, NUM_TRIALS);
    }

    printf("    %d/%d trials passed\n", tested, tested);
    return 0;
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

    printf("test_conn_pair_rotation (seed=%ld):\n", seed);
    printf("  /* Feature: conn-pair-refactor, Property 3: "
           "Timer-driven rotation respects preconditions */\n\n");

    int failures = 0;

    if (test_no_swap_required(seed) < 0)
        failures++;
    if (test_active_not_connected_defers(seed) < 0)
        failures++;
    if (test_standby_not_connected_defers(seed) < 0)
        failures++;
    if (test_active_busy_defers(seed) < 0)
        failures++;
    if (test_comprehensive_precondition_check(seed) < 0)
        failures++;

    printf("\n");
    if (failures == 0) {
        printf("  All rotation precondition property tests PASSED\n");
        return 0;
    } else {
        printf("  %d rotation precondition property test(s) FAILED\n", failures);
        return 1;
    }
}
