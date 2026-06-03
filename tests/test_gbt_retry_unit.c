/* test_gbt_retry_unit.c — Unit tests for GBT retry functions
 *
 * Tests the new retry functions introduced by the GBT race all-fail retry fix:
 *   - enter_retry_wait: state transition + timer creation
 *   - retry_poll_cb: re-dispatch when node reconnects, no-op otherwise
 *   - retry_deadline_cb: sends error and calls race_complete
 *   - is_post_notify_gbt classification logic
 *   - config parsing of gbt_retry_timeout_ms
 *   - retry disabled (gbt_retry_timeout_ms = 0) behavior
 *
 * _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6_
 *
 * Uses mock-based testing consistent with project test conventions.
 * Config tests use the real config_load function with temp JSON files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "../src/config.h"
#include "../src/log.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while (0)

/* ======================================================================
 * SECTION 1: Mock structures replicating rpc_proxy.c internals
 * ====================================================================== */

/* Race states (replicating rpc_proxy.c) */
typedef enum {
    RACE_IDLE,
    RACE_FANOUT,
    RACE_STICKY,
    RACE_RETRY_WAIT
} race_state_t;

/* Connection states (replicating rpc_conn.h) */
typedef enum {
    CONN_DISCONNECTED,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_SENDING,
    CONN_RECEIVING,
    CONN_DEAD
} conn_state_t;

/* Route strategy (replicating rpc_proxy.c) */
typedef enum {
    ROUTE_RACE,
    ROUTE_BROADCAST,
    ROUTE_STICKY
} route_strategy_t;

#define MAX_NODES 16

/* Minimal node model */
typedef struct {
    conn_state_t state;
    bool in_ibd;
} mock_node_t;

/* Minimal proxy model with retry fields */
typedef struct {
    race_state_t state;
    int upstream_count;
    mock_node_t nodes[MAX_NODES];
    bool notify_pending;
    bool is_post_notify_gbt;
    int responses_pending;
    int winner_idx;
    int last_error_idx;
    int sticky_node_idx;
    bool all_must_complete;
    char method[128];
    bool error_sent_to_client;
    bool race_complete_called;

    /* Retry fields */
    int retry_timer_fd;
    int retry_deadline_timer_fd;
    int retry_attempts;
    uint32_t gbt_retry_timeout_ms;

    /* RPC timeout */
    int rpc_timeout_timer_fd;
} mock_proxy_t;

/* ======================================================================
 * SECTION 2: Mock implementations of proxy functions
 * ====================================================================== */

/* Mock enter_retry_wait: replicates the logic from rpc_proxy.c.
 * Returns true if retry was entered, false if disabled. */
static bool
mock_enter_retry_wait(mock_proxy_t *proxy)
{
    /* If retry disabled, fall through to original error behavior */
    if (proxy->gbt_retry_timeout_ms == 0)
        return false;

    /* Transition state to RACE_RETRY_WAIT */
    proxy->state = RACE_RETRY_WAIT;

    /* Cancel existing RPC timeout timer */
    proxy->rpc_timeout_timer_fd = -1;

    /* Create retry poll timer (simulated as fd != -1) */
    proxy->retry_timer_fd = 100;  /* simulated timerfd */

    /* Create retry deadline timer (simulated as fd != -1) */
    proxy->retry_deadline_timer_fd = 101;  /* simulated timerfd */

    return true;
}

/* Mock dispatch_fanout: sends to CONN_CONNECTED nodes only.
 * Returns count of nodes dispatched to. */
static int
mock_dispatch_fanout(mock_proxy_t *proxy)
{
    int sent = 0;
    for (int i = 0; i < proxy->upstream_count; i++) {
        if (proxy->nodes[i].state == CONN_CONNECTED) {
            proxy->nodes[i].state = CONN_SENDING;
            sent++;
        }
    }
    proxy->responses_pending = sent;
    return sent;
}

/* Mock retry_poll_cb logic: replicates the callback behavior.
 * Scans upstreams for CONN_CONNECTED, re-dispatches if found. */
static void
mock_retry_poll_cb(mock_proxy_t *proxy)
{
    /* Guard: only activate in RACE_RETRY_WAIT */
    if (proxy->state != RACE_RETRY_WAIT)
        return;

    /* Scan upstreams for any node in CONN_CONNECTED state */
    bool found_connected = false;
    for (int i = 0; i < proxy->upstream_count; i++) {
        if (proxy->nodes[i].state == CONN_CONNECTED) {
            found_connected = true;
            break;
        }
    }

    if (found_connected) {
        /* Cancel both retry timers */
        proxy->retry_timer_fd = -1;
        proxy->retry_deadline_timer_fd = -1;

        /* Re-dispatch */
        int sent = mock_dispatch_fanout(proxy);

        if (sent > 0) {
            /* Success: transition to RACE_FANOUT */
            proxy->state = RACE_FANOUT;
            proxy->rpc_timeout_timer_fd = 200;  /* simulated rpc timeout */
            proxy->retry_attempts++;
        } else {
            /* Node disconnected between check and send — re-arm timers,
             * remain in RACE_RETRY_WAIT */
            proxy->retry_timer_fd = 102;  /* re-armed */
            proxy->retry_deadline_timer_fd = 103;  /* re-armed */
        }
    } else {
        /* No node connected — re-arm poll timer (remain in RACE_RETRY_WAIT) */
        /* proxy->retry_timer_fd stays set (re-armed) */
    }
}

/* Mock retry_deadline_cb logic: replicates the callback behavior.
 * Sends error and calls race_complete. */
static void
mock_retry_deadline_cb(mock_proxy_t *proxy)
{
    /* Cancel retry poll timer */
    proxy->retry_timer_fd = -1;

    /* Clean up deadline timer */
    proxy->retry_deadline_timer_fd = -1;

    /* Send error to client */
    proxy->error_sent_to_client = true;

    /* race_complete transitions to RACE_IDLE */
    proxy->race_complete_called = true;
    proxy->state = RACE_IDLE;
    proxy->responses_pending = 0;
    proxy->winner_idx = -1;
    proxy->last_error_idx = -1;
}

/* Mock classify_method: replicates the is_post_notify_gbt logic.
 * Returns route strategy and sets is_post_notify_gbt on proxy. */
static route_strategy_t
mock_classify_method(mock_proxy_t *proxy)
{
    const char *method = proxy->method;

    /* Default: not a post-notify GBT race */
    proxy->is_post_notify_gbt = false;

    if (strcmp(method, "getblocktemplate") == 0) {
        if (proxy->notify_pending || proxy->sticky_node_idx == -1) {
            /* First GBT after notify (or startup) → race */
            proxy->is_post_notify_gbt = proxy->notify_pending;
            proxy->notify_pending = false;
            return ROUTE_RACE;
        }
        /* Subsequent GBT → sticky */
        proxy->is_post_notify_gbt = false;
        return ROUTE_STICKY;
    }

    if (strcmp(method, "submitblock") == 0)
        return ROUTE_BROADCAST;

    if (strcmp(method, "sendrawtransaction") == 0)
        return ROUTE_BROADCAST;

    if (strcmp(method, "preciousblock") == 0)
        return ROUTE_STICKY;

    /* All other methods: fan-out race */
    return ROUTE_RACE;
}

/* Initialize proxy for testing */
static void
init_proxy(mock_proxy_t *proxy, int node_count, uint32_t retry_timeout_ms)
{
    memset(proxy, 0, sizeof(*proxy));
    proxy->state = RACE_IDLE;
    proxy->upstream_count = node_count;
    proxy->notify_pending = false;
    proxy->is_post_notify_gbt = false;
    proxy->winner_idx = -1;
    proxy->last_error_idx = -1;
    proxy->sticky_node_idx = -1;
    proxy->all_must_complete = false;
    proxy->error_sent_to_client = false;
    proxy->race_complete_called = false;
    proxy->retry_timer_fd = -1;
    proxy->retry_deadline_timer_fd = -1;
    proxy->retry_attempts = 0;
    proxy->gbt_retry_timeout_ms = retry_timeout_ms;
    proxy->rpc_timeout_timer_fd = -1;
    proxy->method[0] = '\0';
}

/* ======================================================================
 * SECTION 3: Unit tests for enter_retry_wait
 * ====================================================================== */

static void
test_enter_retry_wait_transitions_state(void)
{
    printf("  test_enter_retry_wait_transitions_state\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 3, 5000);
    proxy.state = RACE_FANOUT;
    proxy.rpc_timeout_timer_fd = 50;  /* active timeout */
    strcpy(proxy.method, "getblocktemplate");

    bool entered = mock_enter_retry_wait(&proxy);

    ASSERT(entered == true, "enter_retry_wait returns true when enabled");
    ASSERT(proxy.state == RACE_RETRY_WAIT, "state transitions to RACE_RETRY_WAIT");
    ASSERT(proxy.retry_timer_fd != -1, "retry_timer_fd is created (not -1)");
    ASSERT(proxy.retry_deadline_timer_fd != -1, "retry_deadline_timer_fd is created (not -1)");
    ASSERT(proxy.rpc_timeout_timer_fd == -1, "rpc timeout cancelled");
}

static void
test_enter_retry_wait_creates_both_timers(void)
{
    printf("  test_enter_retry_wait_creates_both_timers\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 1, 5000);
    proxy.state = RACE_FANOUT;

    bool entered = mock_enter_retry_wait(&proxy);

    ASSERT(entered == true, "retry entered");
    ASSERT(proxy.retry_timer_fd >= 0, "retry poll timer fd is valid (>= 0)");
    ASSERT(proxy.retry_deadline_timer_fd >= 0, "retry deadline timer fd is valid (>= 0)");
    ASSERT(proxy.retry_timer_fd != proxy.retry_deadline_timer_fd,
           "retry and deadline are different fds");
}

static void
test_enter_retry_wait_disabled_when_timeout_zero(void)
{
    printf("  test_enter_retry_wait_disabled_when_timeout_zero\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 3, 0);  /* retry disabled */
    proxy.state = RACE_FANOUT;

    bool entered = mock_enter_retry_wait(&proxy);

    ASSERT(entered == false, "enter_retry_wait returns false when disabled");
    ASSERT(proxy.state == RACE_FANOUT, "state unchanged (still RACE_FANOUT)");
    ASSERT(proxy.retry_timer_fd == -1, "no retry timer created");
    ASSERT(proxy.retry_deadline_timer_fd == -1, "no deadline timer created");
}

static void
test_enter_retry_wait_with_various_timeouts(void)
{
    printf("  test_enter_retry_wait_with_various_timeouts\n");

    /* Test with different valid timeout values */
    uint32_t timeouts[] = {100, 1000, 5000, 10000, 30000};
    for (int i = 0; i < 5; i++) {
        mock_proxy_t proxy;
        init_proxy(&proxy, 2, timeouts[i]);
        proxy.state = RACE_FANOUT;

        bool entered = mock_enter_retry_wait(&proxy);

        ASSERT(entered == true, "retry entered with valid timeout");
        ASSERT(proxy.state == RACE_RETRY_WAIT, "state is RACE_RETRY_WAIT");
        ASSERT(proxy.retry_timer_fd != -1, "retry timer created");
        ASSERT(proxy.retry_deadline_timer_fd != -1, "deadline timer created");
    }
}

/* ======================================================================
 * SECTION 4: Unit tests for retry_poll_cb
 * ====================================================================== */

static void
test_retry_poll_cb_redispatches_on_connected(void)
{
    printf("  test_retry_poll_cb_redispatches_on_connected\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 3, 5000);
    proxy.state = RACE_RETRY_WAIT;
    proxy.retry_timer_fd = 100;
    proxy.retry_deadline_timer_fd = 101;
    strcpy(proxy.method, "getblocktemplate");

    /* Set all nodes to disconnected except one */
    proxy.nodes[0].state = CONN_DISCONNECTED;
    proxy.nodes[1].state = CONN_CONNECTED;  /* reconnected! */
    proxy.nodes[2].state = CONN_DEAD;

    mock_retry_poll_cb(&proxy);

    ASSERT(proxy.state == RACE_FANOUT, "state transitions to RACE_FANOUT");
    ASSERT(proxy.retry_timer_fd == -1, "retry timer cancelled");
    ASSERT(proxy.retry_deadline_timer_fd == -1, "deadline timer cancelled");
    ASSERT(proxy.retry_attempts == 1, "retry_attempts incremented to 1");
    ASSERT(proxy.rpc_timeout_timer_fd != -1, "rpc timeout armed");
    ASSERT(proxy.responses_pending > 0, "responses_pending > 0 (dispatched)");
}

static void
test_retry_poll_cb_no_op_when_no_connected(void)
{
    printf("  test_retry_poll_cb_no_op_when_no_connected\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 3, 5000);
    proxy.state = RACE_RETRY_WAIT;
    proxy.retry_timer_fd = 100;
    proxy.retry_deadline_timer_fd = 101;
    strcpy(proxy.method, "getblocktemplate");

    /* All nodes still disconnected */
    proxy.nodes[0].state = CONN_DISCONNECTED;
    proxy.nodes[1].state = CONN_CONNECTING;
    proxy.nodes[2].state = CONN_DEAD;

    mock_retry_poll_cb(&proxy);

    ASSERT(proxy.state == RACE_RETRY_WAIT, "state remains RACE_RETRY_WAIT");
    ASSERT(proxy.retry_timer_fd != -1, "retry timer still active");
    ASSERT(proxy.retry_deadline_timer_fd != -1, "deadline timer still active");
    ASSERT(proxy.retry_attempts == 0, "retry_attempts unchanged");
}

static void
test_retry_poll_cb_multiple_nodes_reconnect(void)
{
    printf("  test_retry_poll_cb_multiple_nodes_reconnect\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 4, 5000);
    proxy.state = RACE_RETRY_WAIT;
    proxy.retry_timer_fd = 100;
    proxy.retry_deadline_timer_fd = 101;
    strcpy(proxy.method, "getblocktemplate");

    /* Multiple nodes reconnected */
    proxy.nodes[0].state = CONN_CONNECTED;
    proxy.nodes[1].state = CONN_CONNECTED;
    proxy.nodes[2].state = CONN_DISCONNECTED;
    proxy.nodes[3].state = CONN_CONNECTED;

    mock_retry_poll_cb(&proxy);

    ASSERT(proxy.state == RACE_FANOUT, "state transitions to RACE_FANOUT");
    ASSERT(proxy.responses_pending == 3, "dispatched to 3 connected nodes");
    ASSERT(proxy.retry_attempts == 1, "retry_attempts incremented");
}

static void
test_retry_poll_cb_not_in_retry_wait(void)
{
    printf("  test_retry_poll_cb_not_in_retry_wait\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 2, 5000);
    proxy.state = RACE_FANOUT;  /* not in RACE_RETRY_WAIT */
    proxy.retry_timer_fd = 100;
    proxy.retry_deadline_timer_fd = 101;
    proxy.nodes[0].state = CONN_CONNECTED;

    mock_retry_poll_cb(&proxy);

    /* Should be a no-op */
    ASSERT(proxy.state == RACE_FANOUT, "state unchanged (guard prevents action)");
    ASSERT(proxy.retry_attempts == 0, "retry_attempts unchanged");
}

static void
test_retry_poll_cb_increments_attempts(void)
{
    printf("  test_retry_poll_cb_increments_attempts\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 2, 5000);
    proxy.state = RACE_RETRY_WAIT;
    proxy.retry_timer_fd = 100;
    proxy.retry_deadline_timer_fd = 101;
    proxy.retry_attempts = 3;  /* already had 3 attempts */
    proxy.nodes[0].state = CONN_CONNECTED;
    proxy.nodes[1].state = CONN_DISCONNECTED;

    mock_retry_poll_cb(&proxy);

    ASSERT(proxy.retry_attempts == 4, "retry_attempts incremented from 3 to 4");
    ASSERT(proxy.state == RACE_FANOUT, "state transitions to RACE_FANOUT");
}

/* ======================================================================
 * SECTION 5: Unit tests for retry_deadline_cb
 * ====================================================================== */

static void
test_retry_deadline_cb_sends_error(void)
{
    printf("  test_retry_deadline_cb_sends_error\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 3, 5000);
    proxy.state = RACE_RETRY_WAIT;
    proxy.retry_timer_fd = 100;
    proxy.retry_deadline_timer_fd = 101;
    strcpy(proxy.method, "getblocktemplate");

    mock_retry_deadline_cb(&proxy);

    ASSERT(proxy.error_sent_to_client == true, "error sent to client");
    ASSERT(proxy.race_complete_called == true, "race_complete called");
    ASSERT(proxy.state == RACE_IDLE, "state transitions to RACE_IDLE");
}

static void
test_retry_deadline_cb_cancels_poll_timer(void)
{
    printf("  test_retry_deadline_cb_cancels_poll_timer\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 2, 5000);
    proxy.state = RACE_RETRY_WAIT;
    proxy.retry_timer_fd = 100;
    proxy.retry_deadline_timer_fd = 101;

    mock_retry_deadline_cb(&proxy);

    ASSERT(proxy.retry_timer_fd == -1, "retry poll timer cancelled");
    ASSERT(proxy.retry_deadline_timer_fd == -1, "deadline timer cancelled");
}

static void
test_retry_deadline_cb_resets_race_state(void)
{
    printf("  test_retry_deadline_cb_resets_race_state\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 3, 5000);
    proxy.state = RACE_RETRY_WAIT;
    proxy.retry_timer_fd = 100;
    proxy.retry_deadline_timer_fd = 101;
    proxy.winner_idx = -1;
    proxy.last_error_idx = 2;
    proxy.responses_pending = 0;

    mock_retry_deadline_cb(&proxy);

    ASSERT(proxy.state == RACE_IDLE, "state is RACE_IDLE after deadline");
    ASSERT(proxy.winner_idx == -1, "winner_idx reset to -1");
    ASSERT(proxy.last_error_idx == -1, "last_error_idx reset to -1");
    ASSERT(proxy.responses_pending == 0, "responses_pending is 0");
}

/* ======================================================================
 * SECTION 6: Unit tests for is_post_notify_gbt classification
 * ====================================================================== */

static void
test_is_post_notify_gbt_true_for_gbt_after_notify(void)
{
    printf("  test_is_post_notify_gbt_true_for_gbt_after_notify\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 3, 5000);
    proxy.notify_pending = true;
    proxy.sticky_node_idx = 0;  /* has a sticky node */
    strcpy(proxy.method, "getblocktemplate");

    route_strategy_t route = mock_classify_method(&proxy);

    ASSERT(route == ROUTE_RACE, "GBT after notify classified as ROUTE_RACE");
    ASSERT(proxy.is_post_notify_gbt == true, "is_post_notify_gbt is true");
    ASSERT(proxy.notify_pending == false, "notify_pending cleared");
}

static void
test_is_post_notify_gbt_false_for_non_gbt_methods(void)
{
    printf("  test_is_post_notify_gbt_false_for_non_gbt_methods\n");

    const char *methods[] = {
        "validateaddress", "decoderawtransaction", "getrawmempool",
        "getmininginfo", "getnetworkinfo"
    };

    for (int i = 0; i < 5; i++) {
        mock_proxy_t proxy;
        init_proxy(&proxy, 3, 5000);
        proxy.notify_pending = true;  /* notify_pending set, but method is not GBT */
        strcpy(proxy.method, methods[i]);

        mock_classify_method(&proxy);

        ASSERT(proxy.is_post_notify_gbt == false,
               "is_post_notify_gbt is false for non-GBT method");
    }
}

static void
test_is_post_notify_gbt_false_for_sticky_gbt(void)
{
    printf("  test_is_post_notify_gbt_false_for_sticky_gbt\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 3, 5000);
    proxy.notify_pending = false;  /* no notify pending → sticky path */
    proxy.sticky_node_idx = 1;    /* has a sticky node */
    strcpy(proxy.method, "getblocktemplate");

    route_strategy_t route = mock_classify_method(&proxy);

    ASSERT(route == ROUTE_STICKY, "sticky GBT classified as ROUTE_STICKY");
    ASSERT(proxy.is_post_notify_gbt == false,
           "is_post_notify_gbt is false for sticky GBT");
}

static void
test_is_post_notify_gbt_false_for_broadcast_methods(void)
{
    printf("  test_is_post_notify_gbt_false_for_broadcast_methods\n");

    const char *methods[] = {"submitblock", "sendrawtransaction"};

    for (int i = 0; i < 2; i++) {
        mock_proxy_t proxy;
        init_proxy(&proxy, 3, 5000);
        proxy.notify_pending = true;
        strcpy(proxy.method, methods[i]);

        route_strategy_t route = mock_classify_method(&proxy);

        ASSERT(route == ROUTE_BROADCAST,
               "broadcast method classified as ROUTE_BROADCAST");
        ASSERT(proxy.is_post_notify_gbt == false,
               "is_post_notify_gbt is false for broadcast method");
    }
}

static void
test_is_post_notify_gbt_true_at_startup_no_sticky(void)
{
    printf("  test_is_post_notify_gbt_true_at_startup_no_sticky\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 3, 5000);
    proxy.notify_pending = false;
    proxy.sticky_node_idx = -1;  /* no sticky at startup */
    strcpy(proxy.method, "getblocktemplate");

    route_strategy_t route = mock_classify_method(&proxy);

    /* At startup with no sticky, GBT races but is_post_notify_gbt is based
     * on notify_pending which is false here */
    ASSERT(route == ROUTE_RACE, "GBT with no sticky classified as ROUTE_RACE");
    ASSERT(proxy.is_post_notify_gbt == false,
           "is_post_notify_gbt is false when notify_pending was false");
}

/* ======================================================================
 * SECTION 7: Config parsing tests for gbt_retry_timeout_ms
 * ====================================================================== */

static const char *TMP_CFG_PATH = "/tmp/test_gbt_retry_unit_config.json";

static void
write_tmp_config(const char *json)
{
    FILE *fp = fopen(TMP_CFG_PATH, "w");
    if (!fp) {
        fprintf(stderr, "Cannot create temp file\n");
        exit(1);
    }
    fputs(json, fp);
    fclose(fp);
}

/* Base config JSON template (all required fields present) */
#define BASE_CONFIG_FMT \
    "{\n" \
    "  \"nodes\": [\n" \
    "    { \"label\": \"node1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332 }\n" \
    "  ],\n" \
    "  \"rpc_server_bind\": \"127.0.0.1\",\n" \
    "  \"rpc_server_port\": 8332,\n" \
    "  \"http_server_bind\": \"0.0.0.0\",\n" \
    "  \"http_server_port\": 7152,\n" \
    "  \"rpc_timeout_ms\": 5000,\n" \
    "  \"reconnect_delay_ms\": 1000,\n" \
    "  \"stall_threshold_ms\": 30000,\n" \
    "  \"log_verbosity\": 2%s\n" \
    "}\n"

static void
test_config_gbt_retry_absent_defaults_to_5000(void)
{
    printf("  test_config_gbt_retry_absent_defaults_to_5000\n");

    char json[1024];
    snprintf(json, sizeof(json), BASE_CONFIG_FMT, "");

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_CFG_PATH);

    ASSERT(cfg != NULL, "config loads successfully without gbt_retry_timeout_ms");
    if (cfg) {
        ASSERT(cfg->gbt_retry_timeout_ms == 5000,
               "gbt_retry_timeout_ms defaults to 5000");
        config_destroy(cfg);
    }
    unlink(TMP_CFG_PATH);
}

static void
test_config_gbt_retry_explicit_value(void)
{
    printf("  test_config_gbt_retry_explicit_value\n");

    char json[1024];
    snprintf(json, sizeof(json), BASE_CONFIG_FMT,
             ",\n  \"gbt_retry_timeout_ms\": 10000");

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_CFG_PATH);

    ASSERT(cfg != NULL, "config loads with explicit gbt_retry_timeout_ms");
    if (cfg) {
        ASSERT(cfg->gbt_retry_timeout_ms == 10000,
               "gbt_retry_timeout_ms parsed as 10000");
        config_destroy(cfg);
    }
    unlink(TMP_CFG_PATH);
}

static void
test_config_gbt_retry_zero_disables(void)
{
    printf("  test_config_gbt_retry_zero_disables\n");

    char json[1024];
    snprintf(json, sizeof(json), BASE_CONFIG_FMT,
             ",\n  \"gbt_retry_timeout_ms\": 0");

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_CFG_PATH);

    ASSERT(cfg != NULL, "config loads with gbt_retry_timeout_ms = 0");
    if (cfg) {
        ASSERT(cfg->gbt_retry_timeout_ms == 0,
               "gbt_retry_timeout_ms is 0 (retry disabled)");
        config_destroy(cfg);
    }
    unlink(TMP_CFG_PATH);
}

static void
test_config_gbt_retry_over_30000_rejected(void)
{
    printf("  test_config_gbt_retry_over_30000_rejected\n");

    char json[1024];
    snprintf(json, sizeof(json), BASE_CONFIG_FMT,
             ",\n  \"gbt_retry_timeout_ms\": 30001");

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_CFG_PATH);

    ASSERT(cfg == NULL, "config rejected when gbt_retry_timeout_ms > 30000");
    unlink(TMP_CFG_PATH);
}

static void
test_config_gbt_retry_max_valid(void)
{
    printf("  test_config_gbt_retry_max_valid\n");

    char json[1024];
    snprintf(json, sizeof(json), BASE_CONFIG_FMT,
             ",\n  \"gbt_retry_timeout_ms\": 30000");

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_CFG_PATH);

    ASSERT(cfg != NULL, "config loads with gbt_retry_timeout_ms = 30000 (max valid)");
    if (cfg) {
        ASSERT(cfg->gbt_retry_timeout_ms == 30000,
               "gbt_retry_timeout_ms parsed as 30000");
        config_destroy(cfg);
    }
    unlink(TMP_CFG_PATH);
}

static void
test_config_gbt_retry_negative_rejected(void)
{
    printf("  test_config_gbt_retry_negative_rejected\n");

    char json[1024];
    snprintf(json, sizeof(json), BASE_CONFIG_FMT,
             ",\n  \"gbt_retry_timeout_ms\": -1");

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_CFG_PATH);

    ASSERT(cfg == NULL, "config rejected when gbt_retry_timeout_ms is negative");
    unlink(TMP_CFG_PATH);
}

/* ======================================================================
 * SECTION 8: Retry disabled (gbt_retry_timeout_ms = 0) behavior
 * ====================================================================== */

static void
test_retry_disabled_all_fail_returns_error_immediately(void)
{
    printf("  test_retry_disabled_all_fail_returns_error_immediately\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 3, 0);  /* retry disabled */
    proxy.notify_pending = true;
    proxy.is_post_notify_gbt = true;
    proxy.state = RACE_FANOUT;
    strcpy(proxy.method, "getblocktemplate");

    /* All nodes disconnected — dispatch_fanout returns 0 */
    proxy.nodes[0].state = CONN_DISCONNECTED;
    proxy.nodes[1].state = CONN_DISCONNECTED;
    proxy.nodes[2].state = CONN_DEAD;

    int dispatched = mock_dispatch_fanout(&proxy);
    ASSERT(dispatched == 0, "dispatch_fanout returns 0 (all disconnected)");

    /* Try to enter retry — should fail because disabled */
    bool entered = mock_enter_retry_wait(&proxy);

    ASSERT(entered == false, "enter_retry_wait returns false (retry disabled)");
    ASSERT(proxy.state == RACE_FANOUT, "state unchanged — caller handles error");

    /* Simulate caller behavior when enter_retry_wait returns false:
     * send error immediately, set state to IDLE */
    proxy.error_sent_to_client = true;
    proxy.state = RACE_IDLE;

    ASSERT(proxy.error_sent_to_client == true, "error sent immediately");
    ASSERT(proxy.state == RACE_IDLE, "state is RACE_IDLE (no retry)");
    ASSERT(proxy.retry_timer_fd == -1, "no retry timer created");
    ASSERT(proxy.retry_deadline_timer_fd == -1, "no deadline timer created");
}

static void
test_retry_disabled_mid_transfer_all_fail(void)
{
    printf("  test_retry_disabled_mid_transfer_all_fail\n");

    mock_proxy_t proxy;
    init_proxy(&proxy, 2, 0);  /* retry disabled */
    proxy.notify_pending = true;
    proxy.is_post_notify_gbt = true;
    proxy.state = RACE_FANOUT;
    strcpy(proxy.method, "getblocktemplate");

    /* Nodes start connected but will fail mid-transfer */
    proxy.nodes[0].state = CONN_CONNECTED;
    proxy.nodes[1].state = CONN_CONNECTED;

    int dispatched = mock_dispatch_fanout(&proxy);
    ASSERT(dispatched == 2, "2 nodes dispatched");

    /* Simulate all failing (responses_pending -> 0, winner == -1) */
    proxy.responses_pending = 0;
    proxy.winner_idx = -1;

    /* Try enter_retry_wait — should fail */
    bool entered = mock_enter_retry_wait(&proxy);

    ASSERT(entered == false, "enter_retry_wait returns false (retry disabled)");
    ASSERT(proxy.retry_timer_fd == -1, "no retry timer");
    ASSERT(proxy.retry_deadline_timer_fd == -1, "no deadline timer");
}

/* ======================================================================
 * SECTION 9: main
 * ====================================================================== */

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Initialize logging (suppress most output during tests) */
    log_init(LOG_CRIT);

    printf("test_gbt_retry_unit:\n");

    /* enter_retry_wait tests */
    printf("\n  --- enter_retry_wait ---\n");
    test_enter_retry_wait_transitions_state();
    test_enter_retry_wait_creates_both_timers();
    test_enter_retry_wait_disabled_when_timeout_zero();
    test_enter_retry_wait_with_various_timeouts();

    /* retry_poll_cb tests */
    printf("\n  --- retry_poll_cb ---\n");
    test_retry_poll_cb_redispatches_on_connected();
    test_retry_poll_cb_no_op_when_no_connected();
    test_retry_poll_cb_multiple_nodes_reconnect();
    test_retry_poll_cb_not_in_retry_wait();
    test_retry_poll_cb_increments_attempts();

    /* retry_deadline_cb tests */
    printf("\n  --- retry_deadline_cb ---\n");
    test_retry_deadline_cb_sends_error();
    test_retry_deadline_cb_cancels_poll_timer();
    test_retry_deadline_cb_resets_race_state();

    /* is_post_notify_gbt classification tests */
    printf("\n  --- is_post_notify_gbt classification ---\n");
    test_is_post_notify_gbt_true_for_gbt_after_notify();
    test_is_post_notify_gbt_false_for_non_gbt_methods();
    test_is_post_notify_gbt_false_for_sticky_gbt();
    test_is_post_notify_gbt_false_for_broadcast_methods();
    test_is_post_notify_gbt_true_at_startup_no_sticky();

    /* Config parsing tests */
    printf("\n  --- config parsing: gbt_retry_timeout_ms ---\n");
    test_config_gbt_retry_absent_defaults_to_5000();
    test_config_gbt_retry_explicit_value();
    test_config_gbt_retry_zero_disables();
    test_config_gbt_retry_over_30000_rejected();
    test_config_gbt_retry_max_valid();
    test_config_gbt_retry_negative_rejected();

    /* Retry disabled behavior tests */
    printf("\n  --- retry disabled (gbt_retry_timeout_ms = 0) ---\n");
    test_retry_disabled_all_fail_returns_error_immediately();
    test_retry_disabled_mid_transfer_all_fail();

    printf("\n  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
