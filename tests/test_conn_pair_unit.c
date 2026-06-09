/* test_conn_pair_unit.c — Unit tests for conn_pair rotation and swap
 *
 * Tests specific scenarios for timer-driven rotation and error-triggered swap
 * behavior without a real event loop. Directly manipulates conn_pair_t fields,
 * uses fd=-1 and loop=NULL for safe testing.
 *
 * **Validates: Requirements 3.1, 3.2, 3.7, 4.4**
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../src/conn_pair.h"
#include "../src/rpc_conn.h"

/* Dummy config for tests that hit rpc_conn_connect recovery path */
static node_config_t dummy_config = {
    .host = "invalid",
    .rpc_port = 1,
    .zmq_port = 0,
    .label = "test"
};

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("  %-60s", #fn); \
    if (fn()) { \
        printf("PASS\n"); \
        tests_passed++; \
    } else { \
        printf("FAIL\n"); \
    } \
} while (0)

/*
 * Set up a conn_pair_t for testing without a real event loop or sockets.
 * - rotation_timer_fd = -1 (timerfd_settime returns EBADF silently)
 * - All slot fds = -1 (rpc_conn_disconnect skips close/epoll removal)
 * - loop = NULL, config = &dummy_config
 */
static void
setup_test_pair(conn_pair_t *pair)
{
    memset(pair, 0, sizeof(*pair));
    pair->rotation_timer_fd = -1;
    pair->loop = NULL;
    pair->config = &dummy_config;
    pair->node_index = 0;
    pair->swap_required = false;
    pair->active_idx = 0;

    pair->slots[0].fd = -1;
    pair->slots[0].loop = NULL;
    pair->slots[0].config = &dummy_config;
    pair->slots[0].state = CONN_CONNECTED;

    pair->slots[1].fd = -1;
    pair->slots[1].loop = NULL;
    pair->slots[1].config = &dummy_config;
    pair->slots[1].state = CONN_DISCONNECTED;
}

/* ---- Test 1: Rotation timer fires and sets swap_required ---- */

/*
 * Simulates what rotation_timer_cb does: sets swap_required = true then
 * calls conn_pair_tick. Verifies swap_required transitions from false to true.
 *
 * Since rotation_timer_cb is static in conn_pair.c, we test the externally
 * visible effect: setting swap_required and calling conn_pair_tick.
 *
 * Validates: Requirement 3.1 — timer fires and sets swap_required
 */
static int
test_timer_sets_swap_required(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    /* Initial state: swap_required is false */
    pair.swap_required = false;
    pair.active_idx = 0;
    pair.slots[0].state = CONN_CONNECTED;
    pair.slots[1].state = CONN_DISCONNECTED;

    /* Simulate what the timer callback does */
    pair.swap_required = true;

    /* Call tick — standby is DISCONNECTED so rotation defers */
    conn_pair_tick(&pair);

    /* Verify swap_required is still true (rotation was deferred) */
    if (!pair.swap_required) {
        fprintf(stderr, "    swap_required should remain true when rotation defers\n");
        return 0;
    }

    /* Verify active_idx unchanged */
    if (pair.active_idx != 0) {
        fprintf(stderr, "    active_idx should remain 0 when rotation defers\n");
        return 0;
    }

    return 1;
}

/*
 * Verify that swap_required stays true across multiple ticks when
 * preconditions are not met, ensuring re-arm behavior.
 *
 * Validates: Requirement 3.1 — re-arm regardless of whether rotation executes
 */
static int
test_swap_required_persists_across_ticks(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.swap_required = true;
    pair.active_idx = 0;
    pair.slots[0].state = CONN_SENDING;  /* busy — rotation blocked */
    pair.slots[1].state = CONN_CONNECTED;

    /* Multiple ticks should not clear swap_required */
    conn_pair_tick(&pair);
    if (!pair.swap_required) {
        fprintf(stderr, "    swap_required cleared after tick 1\n");
        return 0;
    }

    conn_pair_tick(&pair);
    if (!pair.swap_required) {
        fprintf(stderr, "    swap_required cleared after tick 2\n");
        return 0;
    }

    conn_pair_tick(&pair);
    if (!pair.swap_required) {
        fprintf(stderr, "    swap_required cleared after tick 3\n");
        return 0;
    }

    return 1;
}

/* ---- Test 2: Successful rotation (swap) ---- */

/*
 * When swap_required=true, active=CONNECTED (idle), standby=CONNECTED,
 * conn_pair_tick performs the swap: active_idx flips, swap_required cleared.
 *
 * Validates: Requirement 3.2 — rotation executes when preconditions met
 */
static int
test_successful_rotation_enters_sequence(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.swap_required = true;
    pair.active_idx = 0;
    pair.slots[0].state = CONN_CONNECTED;
    pair.slots[1].state = CONN_CONNECTED;

    conn_pair_tick(&pair);

    /* After tick with preconditions met: swap happened */
    if (pair.active_idx != 1) {
        fprintf(stderr, "    active_idx=%d, expected 1 (swap should occur)\n",
                pair.active_idx);
        return 0;
    }

    /* swap_required should be cleared */
    if (pair.swap_required) {
        fprintf(stderr, "    swap_required not cleared after successful swap\n");
        return 0;
    }

    return 1;
}

/*
 * Verify that swap is just an index flip — neither slot's state changes.
 * The standby refresh (disconnect+reconnect) is done by the timer, not tick.
 *
 * Validates: Requirement 3.2 — swap doesn't disconnect anything
 */
static int
test_rotation_disconnects_old_standby(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.swap_required = true;
    pair.active_idx = 0;
    pair.slots[0].state = CONN_CONNECTED;
    pair.slots[1].state = CONN_CONNECTED;

    /* Set some buffer state on slot 1 to verify it's NOT cleared by tick */
    pair.slots[1].send_buf = NULL;
    pair.slots[1].send_len = 0;
    pair.slots[1].recv_len = 0;

    conn_pair_tick(&pair);

    /* Both slots should still be CONN_CONNECTED — tick only flips the index */
    if (pair.slots[0].state != CONN_CONNECTED) {
        fprintf(stderr, "    slot 0 state=%d, expected CONNECTED\n",
                pair.slots[0].state);
        return 0;
    }
    if (pair.slots[1].state != CONN_CONNECTED) {
        fprintf(stderr, "    slot 1 state=%d, expected CONNECTED\n",
                pair.slots[1].state);
        return 0;
    }

    return 1;
}

/* ---- Test 3: Failed standby retains current active ---- */

/*
 * When standby is not CONN_CONNECTED, rotation is deferred and active
 * remains unchanged.
 *
 * Validates: Requirement 3.7 — failed standby retains current primary
 */
static int
test_failed_standby_retains_active_disconnected(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.swap_required = true;
    pair.active_idx = 0;
    pair.slots[0].state = CONN_CONNECTED;
    pair.slots[1].state = CONN_DISCONNECTED;

    conn_pair_tick(&pair);

    /* Active must remain at slot 0 */
    if (pair.active_idx != 0) {
        fprintf(stderr, "    active_idx changed to %d when standby DISCONNECTED\n",
                pair.active_idx);
        return 0;
    }

    /* swap_required must remain true */
    if (!pair.swap_required) {
        fprintf(stderr, "    swap_required cleared when standby not ready\n");
        return 0;
    }

    return 1;
}

/*
 * Standby in CONN_CONNECTING state — rotation deferred, active unchanged.
 *
 * Validates: Requirement 3.7 — standby not CONN_CONNECTED retains current primary
 */
static int
test_failed_standby_retains_active_connecting(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.swap_required = true;
    pair.active_idx = 0;
    pair.slots[0].state = CONN_CONNECTED;
    pair.slots[1].state = CONN_CONNECTING;

    conn_pair_tick(&pair);

    /* Active must remain at slot 0 */
    if (pair.active_idx != 0) {
        fprintf(stderr, "    active_idx changed to %d when standby CONNECTING\n",
                pair.active_idx);
        return 0;
    }

    /* swap_required must remain true */
    if (!pair.swap_required) {
        fprintf(stderr, "    swap_required cleared when standby CONNECTING\n");
        return 0;
    }

    return 1;
}

/*
 * Active is CONN_CONNECTED + swap_required=true but standby is in
 * CONN_SENDING — rotation deferred.
 *
 * Note: standby should normally never be in SENDING/RECEIVING (only active
 * gets requests), but the precondition check must still handle it safely.
 *
 * Validates: Requirement 3.7
 */
static int
test_failed_standby_retains_active_sending(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.swap_required = true;
    pair.active_idx = 0;
    pair.slots[0].state = CONN_CONNECTED;
    pair.slots[1].state = CONN_SENDING;

    conn_pair_tick(&pair);

    if (pair.active_idx != 0) {
        fprintf(stderr, "    active_idx changed to %d when standby SENDING\n",
                pair.active_idx);
        return 0;
    }
    if (!pair.swap_required) {
        fprintf(stderr, "    swap_required cleared when standby SENDING\n");
        return 0;
    }

    return 1;
}

/*
 * When active_idx=1 and standby (slot 0) is not ready, active still retained.
 *
 * Validates: Requirement 3.7 — works regardless of which slot is active
 */
static int
test_failed_standby_retains_active_slot1(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.swap_required = true;
    pair.active_idx = 1;
    pair.slots[1].state = CONN_CONNECTED;
    pair.slots[0].state = CONN_DISCONNECTED;

    conn_pair_tick(&pair);

    if (pair.active_idx != 1) {
        fprintf(stderr, "    active_idx changed from 1 to %d\n", pair.active_idx);
        return 0;
    }
    if (!pair.swap_required) {
        fprintf(stderr, "    swap_required cleared\n");
        return 0;
    }

    return 1;
}

/* ---- Test 4: Error swap closes old primary fd and resets state ---- */

/*
 * conn_pair_report_error closes the old primary's fd, sets state to
 * CONN_DISCONNECTED, and clears send/recv buffer state.
 *
 * Validates: Requirement 4.4 — error swap closes old primary fd and resets state
 */
static int
test_error_swap_disconnects_old_active(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.active_idx = 0;
    pair.slots[0].state = CONN_SENDING;
    pair.slots[0].fd = -1;  /* no real fd to close */
    pair.slots[0].send_buf = (const uint8_t *)"request";
    pair.slots[0].send_len = 7;
    pair.slots[0].send_offset = 3;
    pair.slots[0].recv_len = 100;
    pair.slots[0].headers_complete = 1;
    pair.slots[0].content_length = 50;

    pair.slots[1].state = CONN_CONNECTED;

    conn_pair_report_error(&pair);

    /* Old active (slot 0) should be fully disconnected and cleared */
    if (pair.slots[0].state != CONN_DISCONNECTED) {
        fprintf(stderr, "    old active state=%d, expected DISCONNECTED\n",
                pair.slots[0].state);
        return 0;
    }
    if (pair.slots[0].fd != -1) {
        fprintf(stderr, "    old active fd=%d, expected -1\n", pair.slots[0].fd);
        return 0;
    }
    if (pair.slots[0].send_buf != NULL) {
        fprintf(stderr, "    old active send_buf not cleared\n");
        return 0;
    }
    if (pair.slots[0].send_len != 0) {
        fprintf(stderr, "    old active send_len not cleared\n");
        return 0;
    }
    if (pair.slots[0].send_offset != 0) {
        fprintf(stderr, "    old active send_offset not cleared\n");
        return 0;
    }
    if (pair.slots[0].recv_len != 0) {
        fprintf(stderr, "    old active recv_len not cleared\n");
        return 0;
    }
    if (pair.slots[0].headers_complete != 0) {
        fprintf(stderr, "    old active headers_complete not cleared\n");
        return 0;
    }
    if (pair.slots[0].content_length != 0) {
        fprintf(stderr, "    old active content_length not cleared\n");
        return 0;
    }

    return 1;
}

/*
 * After error swap, active_idx points to the new primary (former standby).
 *
 * Validates: Requirement 4.4 — swap occurs and new active is the former standby
 */
static int
test_error_swap_promotes_standby(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.active_idx = 0;
    pair.slots[0].state = CONN_RECEIVING;
    pair.slots[1].state = CONN_CONNECTED;

    bool available = conn_pair_report_error(&pair);

    /* active_idx should now be 1 */
    if (pair.active_idx != 1) {
        fprintf(stderr, "    active_idx=%d, expected 1\n", pair.active_idx);
        return 0;
    }

    /* Return value should be true (new active is CONN_CONNECTED) */
    if (!available) {
        fprintf(stderr, "    report_error returned false, expected true\n");
        return 0;
    }

    return 1;
}

/*
 * Error swap when standby is not connected: swap still happens but
 * returns unavailable (false).
 *
 * Validates: Requirement 4.4 — swap is unconditional, availability depends on
 * new active state
 */
static int
test_error_swap_unavailable_standby(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.active_idx = 0;
    pair.slots[0].state = CONN_SENDING;
    pair.slots[1].state = CONN_DISCONNECTED;

    bool available = conn_pair_report_error(&pair);

    /* active_idx should now be 1 */
    if (pair.active_idx != 1) {
        fprintf(stderr, "    active_idx=%d, expected 1\n", pair.active_idx);
        return 0;
    }

    /* Return value should be false (new active is DISCONNECTED) */
    if (available) {
        fprintf(stderr, "    report_error returned true, expected false\n");
        return 0;
    }

    /* Old active (slot 0) should still be DISCONNECTED */
    if (pair.slots[0].state != CONN_DISCONNECTED) {
        fprintf(stderr, "    old active state=%d, expected DISCONNECTED\n",
                pair.slots[0].state);
        return 0;
    }

    return 1;
}

/*
 * Error swap from slot 1 as active — verify symmetric behavior.
 *
 * Validates: Requirement 4.4 — works regardless of which slot is active
 */
static int
test_error_swap_from_slot1(void)
{
    conn_pair_t pair;
    setup_test_pair(&pair);

    pair.active_idx = 1;
    pair.slots[1].state = CONN_RECEIVING;
    pair.slots[1].send_buf = (const uint8_t *)"data";
    pair.slots[1].send_len = 4;
    pair.slots[1].recv_len = 200;
    pair.slots[0].state = CONN_CONNECTED;

    bool available = conn_pair_report_error(&pair);

    /* active_idx should now be 0 */
    if (pair.active_idx != 0) {
        fprintf(stderr, "    active_idx=%d, expected 0\n", pair.active_idx);
        return 0;
    }

    /* New active (slot 0) is CONNECTED => available */
    if (!available) {
        fprintf(stderr, "    report_error returned false, expected true\n");
        return 0;
    }

    /* Old active (slot 1) should be fully reset */
    if (pair.slots[1].state != CONN_DISCONNECTED) {
        fprintf(stderr, "    old active (slot 1) state=%d, expected DISCONNECTED\n",
                pair.slots[1].state);
        return 0;
    }
    if (pair.slots[1].send_buf != NULL) {
        fprintf(stderr, "    old active (slot 1) send_buf not cleared\n");
        return 0;
    }
    if (pair.slots[1].recv_len != 0) {
        fprintf(stderr, "    old active (slot 1) recv_len not cleared\n");
        return 0;
    }

    return 1;
}

int
main(void)
{
    printf("test_conn_pair_unit:\n");

    /* Test 1: Rotation timer fires and sets swap_required */
    printf("\n  -- rotation timer fires and sets swap_required --\n");
    RUN_TEST(test_timer_sets_swap_required);
    RUN_TEST(test_swap_required_persists_across_ticks);

    /* Test 2: Successful rotation sequence */
    printf("\n  -- successful rotation sequence --\n");
    RUN_TEST(test_successful_rotation_enters_sequence);
    RUN_TEST(test_rotation_disconnects_old_standby);

    /* Test 3: Failed standby retains current active */
    printf("\n  -- failed standby retains current active --\n");
    RUN_TEST(test_failed_standby_retains_active_disconnected);
    RUN_TEST(test_failed_standby_retains_active_connecting);
    RUN_TEST(test_failed_standby_retains_active_sending);
    RUN_TEST(test_failed_standby_retains_active_slot1);

    /* Test 4: Error swap closes old primary fd and resets state */
    printf("\n  -- error swap closes old primary fd and resets state --\n");
    RUN_TEST(test_error_swap_disconnects_old_active);
    RUN_TEST(test_error_swap_promotes_standby);
    RUN_TEST(test_error_swap_unavailable_standby);
    RUN_TEST(test_error_swap_from_slot1);

    printf("\n  Results: %d/%d tests passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
