/* test_conn_pair_error_swap.c — Property test for error-triggered swap
 *
 * Feature: conn-pair-refactor, Property 2: Error-triggered swap is unconditional
 *
 * For any conn_pair_t and any standby connection state (CONN_DISCONNECTED,
 * CONN_CONNECTING, or CONN_CONNECTED), calling conn_pair_report_error(pair)
 * SHALL swap active_idx such that the former standby slot becomes the new
 * active and the former active becomes the new standby.
 *
 * **Validates: Requirements 1.3, 4.1, 9.3**
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand/rand),
 * minimum 200 trials, seed printed for reproducibility, seed accepted
 * via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include "../src/conn_pair.h"
#include "../src/rpc_conn.h"

#define NUM_TRIALS 200

/* Possible standby states to test */
static const conn_state_t STANDBY_STATES[] = {
    CONN_DISCONNECTED,  /* 0 */
    CONN_CONNECTING,    /* 1 */
    CONN_CONNECTED      /* 2 */
};
#define NUM_STANDBY_STATES 3

static const char *
state_name(conn_state_t s)
{
    switch (s) {
    case CONN_DISCONNECTED: return "CONN_DISCONNECTED";
    case CONN_CONNECTING:   return "CONN_CONNECTING";
    case CONN_CONNECTED:    return "CONN_CONNECTED";
    case CONN_SENDING:      return "CONN_SENDING";
    case CONN_RECEIVING:    return "CONN_RECEIVING";
    default:                return "UNKNOWN";
    }
}

/*
 * Set up a conn_pair_t for testing without needing real sockets or event loop.
 * - rotation_timer_fd = -1 so arm_rotation_timer is a no-op (timerfd_settime
 *   returns EBADF silently)
 * - All slot fds = -1 so rpc_conn_disconnect skips close/epoll removal
 * - Directly manipulate slots[].state and active_idx
 */
static void
setup_test_pair(conn_pair_t *pair)
{
    memset(pair, 0, sizeof(*pair));
    pair->rotation_timer_fd = -1;
    pair->loop = NULL;
    pair->config = &(node_config_t){ .host = "test", .rpc_port = 1, .label = "test" };
    pair->node_index = 0;
    pair->swap_required = false;
    pair->active_idx = 0;

    /* Set both slot fds to -1 so disconnect is a no-op */
    pair->slots[0].fd = -1;
    pair->slots[1].fd = -1;

    /* Default states */
    pair->slots[0].state = CONN_CONNECTED;
    pair->slots[1].state = CONN_DISCONNECTED;
}

/*
 * Property 2: Error-triggered swap is unconditional
 *
 * For each trial:
 *   1. Pick a random initial active_idx (0 or 1)
 *   2. Pick a random state for the active slot (any valid state)
 *   3. Pick a random state for the standby slot from
 *      {CONN_DISCONNECTED, CONN_CONNECTING, CONN_CONNECTED}
 *   4. Call conn_pair_report_error
 *   5. Verify active_idx has flipped to the opposite value
 *
 * The swap must happen regardless of what state the standby is in.
 */
static int
test_property_error_swap_unconditional(long seed)
{
    printf("  property: error-triggered swap is unconditional "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand((unsigned int)seed);

    int failures = 0;

    for (int i = 0; i < NUM_TRIALS; i++) {
        conn_pair_t pair;
        setup_test_pair(&pair);

        /* Random initial active_idx: 0 or 1 */
        int initial_active = rand() % 2;
        pair.active_idx = initial_active;

        int standby_idx = 1 - initial_active;

        /* Random state for the active slot — pick from all valid states */
        conn_state_t active_states[] = {
            CONN_DISCONNECTED, CONN_CONNECTING, CONN_CONNECTED,
            CONN_SENDING, CONN_RECEIVING
        };
        conn_state_t active_state = active_states[rand() % 5];
        pair.slots[initial_active].state = active_state;

        /* Random standby state from the three specified states */
        conn_state_t standby_state = STANDBY_STATES[rand() % NUM_STANDBY_STATES];
        pair.slots[standby_idx].state = standby_state;

        /* Ensure fds are -1 so disconnect doesn't try real close */
        pair.slots[0].fd = -1;
        pair.slots[1].fd = -1;

        /* Call report_error */
        bool result = conn_pair_report_error(&pair);

        /* Verify: active_idx must now be the opposite of what it was */
        int expected_active = 1 - initial_active;
        if (pair.active_idx != expected_active) {
            fprintf(stderr, "  FAIL trial %d (seed=%ld):\n", i, seed);
            fprintf(stderr, "    initial_active=%d, standby_state=%s, "
                    "active_state=%s\n",
                    initial_active, state_name(standby_state),
                    state_name(active_state));
            fprintf(stderr, "    expected active_idx=%d, got active_idx=%d\n",
                    expected_active, pair.active_idx);
            failures++;
            if (failures >= 5) {
                fprintf(stderr, "  Too many failures, stopping early\n");
                break;
            }
            continue;
        }

        /* Also verify the return value matches standby availability */
        bool expected_result = (standby_state == CONN_CONNECTED);
        if (result != expected_result) {
            fprintf(stderr, "  FAIL trial %d (seed=%ld):\n", i, seed);
            fprintf(stderr, "    initial_active=%d, standby_state=%s\n",
                    initial_active, state_name(standby_state));
            fprintf(stderr, "    expected return=%s, got return=%s\n",
                    expected_result ? "true" : "false",
                    result ? "true" : "false");
            failures++;
            if (failures >= 5) {
                fprintf(stderr, "  Too many failures, stopping early\n");
                break;
            }
        }
    }

    if (failures == 0) {
        printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    } else {
        fprintf(stderr, "    %d/%d trials FAILED\n", failures, NUM_TRIALS);
    }
    return failures;
}

/*
 * Sub-property: Repeated errors keep toggling active_idx.
 *
 * Calling conn_pair_report_error N times in a row should toggle active_idx
 * each time, regardless of the intermediate states.
 */
static int
test_property_repeated_errors_toggle(long seed)
{
    printf("  property: repeated errors toggle active_idx each time "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand((unsigned int)seed);

    int failures = 0;

    for (int i = 0; i < NUM_TRIALS; i++) {
        conn_pair_t pair;
        setup_test_pair(&pair);

        /* Random initial active */
        int initial_active = rand() % 2;
        pair.active_idx = initial_active;

        /* Random number of consecutive errors (2-10) */
        int num_errors = 2 + (rand() % 9);

        int current_active = initial_active;

        for (int e = 0; e < num_errors; e++) {
            /* Set random standby state before each error */
            int standby_idx = 1 - pair.active_idx;
            conn_state_t standby_state = STANDBY_STATES[rand() % NUM_STANDBY_STATES];
            pair.slots[standby_idx].state = standby_state;

            /* Ensure fds stay -1 */
            pair.slots[0].fd = -1;
            pair.slots[1].fd = -1;

            int expected_next = 1 - current_active;

            conn_pair_report_error(&pair);

            if (pair.active_idx != expected_next) {
                fprintf(stderr, "  FAIL trial %d error %d (seed=%ld):\n",
                        i, e, seed);
                fprintf(stderr, "    before active_idx=%d, expected=%d, got=%d\n",
                        current_active, expected_next, pair.active_idx);
                failures++;
                if (failures >= 5)
                    goto done_repeated;
                break;
            }

            current_active = pair.active_idx;
        }

        /* After N errors, active_idx should be:
         * initial_active if N is even, 1-initial_active if N is odd */
        int expected_final = (num_errors % 2 == 0)
            ? initial_active : (1 - initial_active);

        if (pair.active_idx != expected_final) {
            fprintf(stderr, "  FAIL trial %d (seed=%ld): after %d errors, "
                    "expected active_idx=%d, got=%d\n",
                    i, seed, num_errors, expected_final, pair.active_idx);
            failures++;
            if (failures >= 5)
                break;
        }
    }

done_repeated:
    if (failures == 0) {
        printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    } else {
        fprintf(stderr, "    %d/%d trials FAILED\n", failures, NUM_TRIALS);
    }
    return failures;
}

/*
 * Sub-property: Error swap disconnects the old active slot.
 *
 * After conn_pair_report_error, the former active slot (now standby)
 * must be in CONN_DISCONNECTED state (rpc_conn_disconnect was called on it).
 */
static int
test_property_error_disconnects_old_active(long seed)
{
    printf("  property: error swap disconnects old active slot "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand((unsigned int)seed);

    int failures = 0;

    for (int i = 0; i < NUM_TRIALS; i++) {
        conn_pair_t pair;
        setup_test_pair(&pair);

        /* Random initial active */
        int initial_active = rand() % 2;
        pair.active_idx = initial_active;

        /* Set active to a non-disconnected state */
        conn_state_t active_states[] = {
            CONN_CONNECTED, CONN_SENDING, CONN_RECEIVING, CONN_CONNECTING
        };
        pair.slots[initial_active].state = active_states[rand() % 4];

        /* Random standby state */
        int standby_idx = 1 - initial_active;
        pair.slots[standby_idx].state = STANDBY_STATES[rand() % NUM_STANDBY_STATES];

        /* Ensure fds are -1 */
        pair.slots[0].fd = -1;
        pair.slots[1].fd = -1;

        conn_pair_report_error(&pair);

        /* The old active (now at initial_active index) should be DISCONNECTED */
        if (pair.slots[initial_active].state != CONN_DISCONNECTED) {
            fprintf(stderr, "  FAIL trial %d (seed=%ld):\n", i, seed);
            fprintf(stderr, "    old active slot %d state=%s, expected "
                    "CONN_DISCONNECTED\n",
                    initial_active,
                    state_name(pair.slots[initial_active].state));
            failures++;
            if (failures >= 5) {
                fprintf(stderr, "  Too many failures, stopping early\n");
                break;
            }
        }
    }

    if (failures == 0) {
        printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    } else {
        fprintf(stderr, "    %d/%d trials FAILED\n", failures, NUM_TRIALS);
    }
    return failures;
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

    printf("test_conn_pair_error_swap (seed=%ld):\n", seed);

    int total_failures = 0;

    total_failures += test_property_error_swap_unconditional(seed);
    total_failures += test_property_repeated_errors_toggle(seed);
    total_failures += test_property_error_disconnects_old_active(seed);

    if (total_failures == 0) {
        printf("  ALL PASSED\n");
        return 0;
    } else {
        fprintf(stderr, "  TOTAL FAILURES: %d\n", total_failures);
        return 1;
    }
}
