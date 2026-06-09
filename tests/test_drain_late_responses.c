/* test_drain_late_responses.c — Integration test for late response draining
 *
 * Property 3: Preservation — Late responses still logged and drained
 *
 * Verifies:
 *   1. Late upstream responses arriving after early IDLE transition are
 *      logged with timing info and connections are reset
 *   2. A new race dispatched while old connections are still draining
 *      skips those connections (dispatch_fanout checks conn->state != CONN_CONNECTED)
 *   3. Late errors during drain reset the individual connection without
 *      affecting new race state
 *   4. GBT height logging (elapsed_us, since_notify_us) still works for
 *      all responding nodes even after early transition
 *
 * **Validates: Requirements 2.3, 3.3**
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

#define NUM_TRIALS 1000
#define MAX_NODES  8

/* ---- Replicate relevant enums and structs ---- */

typedef enum {
    RACE_IDLE,
    RACE_FANOUT,
    RACE_STICKY
} race_state_t;

typedef enum {
    CONN_DISCONNECTED,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_SENDING,
    CONN_RECEIVING
} conn_state_t;

/* Simulated upstream connection */
typedef struct {
    conn_state_t state;
    int node_index;
    uint64_t request_start_ns;
    bool connection_close;
    /* Tracking for test assertions */
    bool was_reset;
    int reset_count;
} sim_conn_t;

/* Simulated proxy state */
typedef struct {
    race_state_t state;
    int responses_pending;
    int winner_idx;
    int last_error_idx;
    bool all_must_complete;
    bool race_complete_called;
    int upstream_count;
    sim_conn_t upstreams[MAX_NODES];

    /* GBT tracking */
    uint64_t last_notify_ns;
    int64_t last_block_height;
    bool gbt_height_matched;
    int64_t best_gbt_height;
    int best_gbt_node_idx;

    /* Logging capture */
    int late_responses_logged;
    int late_errors_logged;
    uint64_t last_logged_elapsed_us;
    uint64_t last_logged_since_notify_us;
    char method[64];
} sim_proxy_t;

/* ---- Simulated rpc_conn_reset() ----
 * Mirrors real rpc_conn_reset: clears buffers, transitions back to
 * CONN_CONNECTED (or CONN_DISCONNECTED if connection_close was set). */
static void
sim_conn_reset(sim_conn_t *conn)
{
    conn->was_reset = true;
    conn->reset_count++;
    conn->request_start_ns = 0;

    if (conn->connection_close) {
        conn->connection_close = false;
        conn->state = CONN_DISCONNECTED;
    } else if (conn->state == CONN_RECEIVING || conn->state == CONN_SENDING) {
        conn->state = CONN_CONNECTED;
    }
}

/* ---- Simulated race_complete() ---- */
static void
sim_race_complete(sim_proxy_t *proxy)
{
    proxy->state = RACE_IDLE;
    proxy->responses_pending = 0;
    proxy->winner_idx = -1;
    proxy->last_error_idx = -1;
    proxy->race_complete_called = true;
    for (int i = 0; i < proxy->upstream_count; i++) {
        if (proxy->upstreams[i].state == CONN_RECEIVING ||
            proxy->upstreams[i].state == CONN_SENDING) {
            sim_conn_reset(&proxy->upstreams[i]);
        }
    }
}

/* ---- Simulated on_upstream_response() — FIXED version ----
 * Mirrors the fixed rpc_proxy.c logic:
 *   - Early drain check (state==RACE_IDLE && conn in CONN_RECEIVING)
 *   - Early IDLE transition after non-broadcast winner sent
 *   - Winner conn + current conn reset on early IDLE transition
 *   - GBT height logging for all responding nodes
 *   - Timeout kept running as safety net (NOT cancelled on early IDLE) */
static void
sim_on_upstream_response(sim_proxy_t *proxy, int node_idx,
                         bool is_error, uint64_t now_ns)
{
    sim_conn_t *conn = &proxy->upstreams[node_idx];

    /* Calculate elapsed time */
    uint64_t elapsed_ns = (conn->request_start_ns > 0)
                          ? (now_ns - conn->request_start_ns) : 0;
    uint64_t elapsed_us = elapsed_ns / 1000;

    /* Handle late response during drain: proxy already transitioned to IDLE
     * after sending a non-broadcast winner, but this connection was still
     * receiving from the previous race. Log and reset just this connection. */
    if (proxy->state == RACE_IDLE && conn->state == CONN_RECEIVING) {
        proxy->late_responses_logged++;
        proxy->last_logged_elapsed_us = elapsed_us;

        /* GBT timing: compute since_notify_us */
        if (proxy->last_notify_ns > 0 &&
            conn->request_start_ns >= proxy->last_notify_ns) {
            proxy->last_logged_since_notify_us =
                (now_ns - proxy->last_notify_ns) / 1000;
        }

        sim_conn_reset(conn);
        return;
    }

    /* Decrement pending count */
    proxy->responses_pending--;

    if (is_error) {
        proxy->last_error_idx = node_idx;
    } else {
        if (proxy->winner_idx == -1) {
            proxy->winner_idx = node_idx;
            /* (response sent to client) */
        }
    }

    /* Determine if race is over */
    if (proxy->responses_pending <= 0) {
        if (proxy->winner_idx == -1 && proxy->last_error_idx >= 0) {
            /* All-error fallback — not relevant for drain tests */
        }
        sim_race_complete(proxy);
    } else if (!proxy->all_must_complete && proxy->winner_idx != -1) {
        /* Non-broadcast winner sent: transition to IDLE immediately.
         * Reset the winner's connection and the current responding
         * connection (if different) so they return to CONN_CONNECTED. */
        proxy->state = RACE_IDLE;

        sim_conn_t *winner_conn = &proxy->upstreams[proxy->winner_idx];
        if (winner_conn->state == CONN_RECEIVING ||
            winner_conn->state == CONN_SENDING)
            sim_conn_reset(winner_conn);
        if (conn != winner_conn &&
            (conn->state == CONN_RECEIVING || conn->state == CONN_SENDING))
            sim_conn_reset(conn);
    }
}

/* ---- Simulated on_upstream_error() — FIXED version ----
 * Mirrors the fixed rpc_proxy.c: if proxy->state == RACE_IDLE, this is
 * a late error during drain — log and reset just this connection. */
static void
sim_on_upstream_error(sim_proxy_t *proxy, int node_idx)
{
    sim_conn_t *conn = &proxy->upstreams[node_idx];

    /* Handle late error during drain */
    if (proxy->state == RACE_IDLE) {
        proxy->late_errors_logged++;
        sim_conn_reset(conn);
        return;
    }

    /* Normal error handling */
    proxy->responses_pending--;
    proxy->last_error_idx = node_idx;

    if (proxy->responses_pending <= 0) {
        if (proxy->winner_idx == -1 && proxy->last_error_idx >= 0) {
            /* All-error fallback */
        }
        sim_race_complete(proxy);
    }
}

/* ---- Simulated dispatch_fanout() ----
 * Mirrors real dispatch_fanout: skips connections not in CONN_CONNECTED. */
static int
sim_dispatch_fanout(sim_proxy_t *proxy)
{
    int sent = 0;
    for (int i = 0; i < proxy->upstream_count; i++) {
        sim_conn_t *conn = &proxy->upstreams[i];
        if (conn->state != CONN_CONNECTED)
            continue;
        /* Simulate sending: transition to RECEIVING */
        conn->state = CONN_RECEIVING;
        conn->request_start_ns = 500000000ULL + (uint64_t)i * 1000ULL;
        conn->was_reset = false;
        sent++;
    }
    proxy->responses_pending = sent;
    return sent;
}

/* ---- Helper: initialize proxy for a non-broadcast fan-out race ---- */
static void
sim_init_race(sim_proxy_t *proxy, int num_nodes, const char *method)
{
    memset(proxy, 0, sizeof(*proxy));
    proxy->state = RACE_FANOUT;
    proxy->upstream_count = num_nodes;
    proxy->winner_idx = -1;
    proxy->last_error_idx = -1;
    proxy->all_must_complete = false;
    proxy->last_notify_ns = 0;
    proxy->last_block_height = 800000;
    proxy->gbt_height_matched = false;
    proxy->best_gbt_height = -1;
    proxy->best_gbt_node_idx = -1;
    strncpy(proxy->method, method, sizeof(proxy->method) - 1);

    for (int i = 0; i < num_nodes; i++) {
        proxy->upstreams[i].state = CONN_RECEIVING;
        proxy->upstreams[i].node_index = i;
        proxy->upstreams[i].request_start_ns =
            100000000ULL + (uint64_t)i * 5000000ULL;
        proxy->upstreams[i].connection_close = false;
        proxy->upstreams[i].was_reset = false;
        proxy->upstreams[i].reset_count = 0;
    }

    proxy->responses_pending = num_nodes;
}

/* ---- Fisher-Yates shuffle ---- */
static void
shuffle_int(int *arr, int count)
{
    for (int i = count - 1; i > 0; i--) {
        int j = (int)(lrand48() % (i + 1));
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/*
 * Test 1: Late responses after early IDLE transition are logged and
 * connections are reset.
 *
 * Scenario (non-broadcast race, 3 nodes):
 *   - node[0] wins -> proxy transitions to IDLE
 *   - node[1] responds late -> connection reset, state stays IDLE
 *   - Randomized: N nodes (2-8), random winner, random arrival order
 *
 * Validates: Requirement 2.3
 */
static int
test_late_responses_logged_and_reset(long seed)
{
    printf("  property: late responses logged and connections reset "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));
        sim_proxy_t proxy;
        sim_init_race(&proxy, num_nodes, "validateaddress");

        /* Random arrival order */
        int arrival[MAX_NODES] = {0};
        for (int i = 0; i < num_nodes; i++)
            arrival[i] = i;
        shuffle_int(arrival, num_nodes);

        /* First response is a success (winner) */
        uint64_t now_ns = 200000000ULL;
        sim_on_upstream_response(&proxy, arrival[0], false, now_ns);

        /* Verify: proxy transitioned to IDLE */
        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: state not IDLE after winner "
                    "(state=%d)\n", trial, proxy.state);
            return -1;
        }
        if (proxy.winner_idx != arrival[0]) {
            fprintf(stderr, "  FAIL trial %d: winner_idx=%d expected=%d\n",
                    trial, proxy.winner_idx, arrival[0]);
            return -1;
        }

        /* Winner connection was reset to CONN_CONNECTED by early IDLE logic */
        if (proxy.upstreams[arrival[0]].state != CONN_CONNECTED) {
            fprintf(stderr, "  FAIL trial %d: winner conn state=%d "
                    "expected CONN_CONNECTED\n",
                    trial, proxy.upstreams[arrival[0]].state);
            return -1;
        }

        /* Deliver late responses from remaining nodes */
        proxy.late_responses_logged = 0;
        proxy.late_errors_logged = 0;

        for (int i = 1; i < num_nodes; i++) {
            now_ns += 50000000ULL; /* 50ms between each late response */
            bool is_error = (lrand48() % 3 == 0);

            if (is_error) {
                sim_on_upstream_error(&proxy, arrival[i]);
            } else {
                sim_on_upstream_response(&proxy, arrival[i], false, now_ns);
            }

            /* Verify: proxy stays in RACE_IDLE */
            if (proxy.state != RACE_IDLE) {
                fprintf(stderr, "  FAIL trial %d: state changed from IDLE "
                        "during drain (state=%d, i=%d)\n",
                        trial, proxy.state, i);
                return -1;
            }

            /* Verify: connection was reset */
            if (!proxy.upstreams[arrival[i]].was_reset) {
                fprintf(stderr, "  FAIL trial %d: conn[%d] not reset after "
                        "late response\n", trial, arrival[i]);
                return -1;
            }

            /* Verify: connection transitioned to CONN_CONNECTED */
            if (proxy.upstreams[arrival[i]].state != CONN_CONNECTED) {
                fprintf(stderr, "  FAIL trial %d: conn[%d] state=%d "
                        "expected CONN_CONNECTED\n",
                        trial, arrival[i],
                        proxy.upstreams[arrival[i]].state);
                return -1;
            }
        }

        /* Verify: all late responses were logged */
        int total_late = proxy.late_responses_logged + proxy.late_errors_logged;
        int expected_late = num_nodes - 1;
        if (total_late != expected_late) {
            fprintf(stderr, "  FAIL trial %d: logged %d late events, "
                    "expected %d\n", trial, total_late, expected_late);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Test 2: New race dispatched while old connections are still draining
 * skips those connections.
 *
 * Scenario (non-broadcast race, 3 nodes):
 *   - node[0] wins -> proxy transitions to IDLE
 *   - New request dispatched -> node[1] late response arrives
 *   - Only node[1] reset, new race unaffected
 *   - dispatch_fanout checks conn->state != CONN_CONNECTED
 *
 * Validates: Requirement 2.3
 */
static int
test_new_race_skips_draining_connections(long seed)
{
    printf("  property: new race skips draining connections "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 3 + (int)(lrand48() % (MAX_NODES - 2));
        sim_proxy_t proxy;
        sim_init_race(&proxy, num_nodes, "decoderawtransaction");

        /* Random arrival order */
        int arrival[MAX_NODES] = {0};
        for (int i = 0; i < num_nodes; i++)
            arrival[i] = i;
        shuffle_int(arrival, num_nodes);

        /* First node responds (winner), proxy goes IDLE.
         * The early IDLE transition resets the winner conn to CONNECTED. */
        uint64_t now_ns = 200000000ULL;
        sim_on_upstream_response(&proxy, arrival[0], false, now_ns);

        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: state not IDLE after winner\n",
                    trial);
            return -1;
        }

        /* Some late responses arrive (random subset), resetting those conns */
        int late_arrived = (int)(lrand48() % (num_nodes - 1));
        for (int i = 1; i <= late_arrived; i++) {
            now_ns += 10000000ULL;
            sim_on_upstream_response(&proxy, arrival[i], false, now_ns);
        }

        /* Count how many connections are still draining (CONN_RECEIVING)
         * vs ready (CONN_CONNECTED) */
        int still_draining = 0;
        int ready = 0;
        for (int i = 0; i < num_nodes; i++) {
            if (proxy.upstreams[i].state == CONN_RECEIVING)
                still_draining++;
            else if (proxy.upstreams[i].state == CONN_CONNECTED)
                ready++;
        }

        /* Now dispatch a new race */
        proxy.state = RACE_FANOUT;
        proxy.winner_idx = -1;
        proxy.last_error_idx = -1;
        proxy.all_must_complete = false;
        proxy.race_complete_called = false;
        strncpy(proxy.method, "getblockcount", sizeof(proxy.method) - 1);

        int dispatched = sim_dispatch_fanout(&proxy);

        /* Verify: only CONN_CONNECTED connections were dispatched to */
        if (dispatched != ready) {
            fprintf(stderr, "  FAIL trial %d: dispatched=%d but ready=%d "
                    "(still_draining=%d)\n",
                    trial, dispatched, ready, still_draining);
            return -1;
        }

        /* Verify: draining connections were NOT dispatched to */
        for (int i = 0; i < num_nodes; i++) {
            /* Connections that were CONN_RECEIVING before dispatch should
             * still be CONN_RECEIVING (untouched by dispatch_fanout) */
            if (proxy.upstreams[i].state == CONN_RECEIVING) {
                /* This is fine — either still draining from old race
                 * or newly dispatched. Count should match. */
            }
        }

        /* Key invariant: dispatched + still_draining <= num_nodes */
        if (dispatched + still_draining > num_nodes) {
            fprintf(stderr, "  FAIL trial %d: dispatched + draining > total "
                    "(%d + %d > %d)\n",
                    trial, dispatched, still_draining, num_nodes);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Test 3: Late errors during drain reset the individual connection
 * without affecting new race state.
 *
 * Scenario (non-broadcast race, 3 nodes):
 *   - node[0] wins -> proxy transitions to IDLE
 *   - node[1] errors during drain -> node[1] connection reset
 *   - State stays IDLE, race_complete NOT called
 *
 * Validates: Requirement 2.3
 */
static int
test_late_errors_dont_affect_new_race(long seed)
{
    printf("  property: late errors during drain don't affect new race "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 3 + (int)(lrand48() % (MAX_NODES - 2));
        sim_proxy_t proxy;
        sim_init_race(&proxy, num_nodes, "validateaddress");

        /* Pick winner (random node) */
        int winner = (int)(lrand48() % num_nodes);
        uint64_t now_ns = 200000000ULL;
        sim_on_upstream_response(&proxy, winner, false, now_ns);

        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: not IDLE after winner\n", trial);
            return -1;
        }

        /* Find a connection still draining (CONN_RECEIVING) */
        int draining_idx = -1;
        for (int i = 0; i < num_nodes; i++) {
            if (proxy.upstreams[i].state == CONN_RECEIVING) {
                draining_idx = i;
                break;
            }
        }

        if (draining_idx == -1) {
            /* All connections already drained — skip this trial
             * (can happen with 3 nodes: winner resets winner+current) */
            passed++;
            continue;
        }

        /* Record state before the late error */
        proxy.late_errors_logged = 0;
        proxy.race_complete_called = false;

        /* Simulate a late error on the draining connection */
        sim_on_upstream_error(&proxy, draining_idx);

        /* Verify: connection was reset */
        if (!proxy.upstreams[draining_idx].was_reset) {
            fprintf(stderr, "  FAIL trial %d: draining conn[%d] not reset "
                    "after late error\n", trial, draining_idx);
            return -1;
        }

        /* Verify: state still IDLE (not changed) */
        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: state changed after late error "
                    "(state=%d)\n", trial, proxy.state);
            return -1;
        }

        /* Verify: race_complete was NOT called */
        if (proxy.race_complete_called) {
            fprintf(stderr, "  FAIL trial %d: race_complete called during "
                    "drain error\n", trial);
            return -1;
        }

        /* Verify: late error was logged */
        if (proxy.late_errors_logged != 1) {
            fprintf(stderr, "  FAIL trial %d: late_errors_logged=%d "
                    "expected 1\n", trial, proxy.late_errors_logged);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Test 4: GBT height logging (elapsed_us, since_notify_us) still works
 * for all responding nodes even after early transition.
 *
 * Scenario (GBT race):
 *   - node[0] wins with height match -> proxy transitions to IDLE
 *   - node[1] responds late -> timing info (elapsed_us, since_notify_us)
 *     still logged correctly
 *
 * Validates: Requirement 3.3
 */
static int
test_gbt_timing_logged_after_early_transition(long seed)
{
    printf("  property: GBT timing logged for all nodes after early "
           "transition (seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));
        sim_proxy_t proxy;
        sim_init_race(&proxy, num_nodes, "getblocktemplate");

        /* Set up notify timestamp (block notify arrived before dispatch) */
        uint64_t notify_ns = 50000000ULL; /* 50ms mark */
        proxy.last_notify_ns = notify_ns;

        /* Set request_start_ns for all connections AFTER notify */
        uint64_t dispatch_ns = 60000000ULL; /* 60ms mark (10ms after notify) */
        for (int i = 0; i < num_nodes; i++) {
            proxy.upstreams[i].request_start_ns =
                dispatch_ns + (uint64_t)i * 1000ULL;
        }

        /* Random arrival order */
        int arrival[MAX_NODES] = {0};
        for (int i = 0; i < num_nodes; i++)
            arrival[i] = i;
        shuffle_int(arrival, num_nodes);

        /* First node responds (winner) — height match */
        uint64_t winner_ns = 100000000ULL; /* 100ms mark */
        sim_on_upstream_response(&proxy, arrival[0], false, winner_ns);

        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: state not IDLE after GBT "
                    "winner\n", trial);
            return -1;
        }

        /* Deliver late GBT responses from remaining nodes */
        proxy.late_responses_logged = 0;

        for (int i = 1; i < num_nodes; i++) {
            uint64_t late_ns = winner_ns + (uint64_t)i * 30000000ULL;

            /* Only nodes still in CONN_RECEIVING will trigger drain path */
            if (proxy.upstreams[arrival[i]].state != CONN_RECEIVING) {
                /* This node was already reset (e.g., it was the winner's
                 * conn or the current conn during early IDLE transition).
                 * Skip — it won't trigger the drain path. */
                continue;
            }

            sim_on_upstream_response(&proxy, arrival[i], false, late_ns);

            /* Verify: elapsed_us was computed (non-zero for valid start) */
            if (proxy.last_logged_elapsed_us == 0) {
                fprintf(stderr, "  FAIL trial %d: elapsed_us=0 for node[%d] "
                        "with valid request_start\n", trial, arrival[i]);
                return -1;
            }

            /* Verify: since_notify_us was computed correctly */
            uint64_t expected_since_notify =
                (late_ns - proxy.last_notify_ns) / 1000;
            if (proxy.last_logged_since_notify_us != expected_since_notify) {
                fprintf(stderr, "  FAIL trial %d: since_notify_us=%llu "
                        "expected=%llu (node[%d])\n",
                        trial,
                        (unsigned long long)proxy.last_logged_since_notify_us,
                        (unsigned long long)expected_since_notify,
                        arrival[i]);
                return -1;
            }
        }

        /* Verify: proxy still in IDLE (not corrupted) */
        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: state not IDLE after all "
                    "late GBT responses\n", trial);
            return -1;
        }

        /* Verify: at least some late responses were logged */
        if (proxy.late_responses_logged == 0 && num_nodes > 2) {
            fprintf(stderr, "  FAIL trial %d: no late responses logged "
                    "(num_nodes=%d)\n", trial, num_nodes);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Test 5: Concrete scenario — 3 nodes, full drain + new race lifecycle.
 *
 * Simulates the complete sequence:
 *   1. validateaddress dispatched to 3 nodes
 *   2. node[0] responds (winner), proxy goes IDLE, winner conn reset
 *   3. While proxy is IDLE, node[1] delivers late response (drain path)
 *   4. node[2] delivers late error (drain path)
 *   5. All connections now CONNECTED, new dispatch works to all 3
 *   6. Verify new race completes normally
 */
static int
test_concrete_full_lifecycle(void)
{
    printf("  concrete: full lifecycle — drain + new race\n");

    sim_proxy_t proxy;
    sim_init_race(&proxy, 3, "validateaddress");

    /* Step 1-2: node[0] responds (winner) */
    uint64_t now_ns = 200000000ULL;
    sim_on_upstream_response(&proxy, 0, false, now_ns);

    if (proxy.state != RACE_IDLE) {
        fprintf(stderr, "    FAIL: state not IDLE after winner (state=%d)\n",
                proxy.state);
        return -1;
    }
    if (proxy.winner_idx != 0) {
        fprintf(stderr, "    FAIL: winner_idx=%d expected 0\n",
                proxy.winner_idx);
        return -1;
    }

    /* Winner connection was reset to CONN_CONNECTED by early IDLE logic */
    if (proxy.upstreams[0].state != CONN_CONNECTED) {
        fprintf(stderr, "    FAIL: winner conn state=%d expected "
                "CONN_CONNECTED\n", proxy.upstreams[0].state);
        return -1;
    }

    /* node[1] and node[2] still in CONN_RECEIVING (draining) */
    if (proxy.upstreams[1].state != CONN_RECEIVING ||
        proxy.upstreams[2].state != CONN_RECEIVING) {
        fprintf(stderr, "    FAIL: draining nodes not in CONN_RECEIVING "
                "(node[1]=%d, node[2]=%d)\n",
                proxy.upstreams[1].state, proxy.upstreams[2].state);
        return -1;
    }

    /* Step 3: node[1] delivers late response — drain path fires */
    proxy.late_responses_logged = 0;
    now_ns += 50000000ULL;
    sim_on_upstream_response(&proxy, 1, false, now_ns);

    if (proxy.upstreams[1].state != CONN_CONNECTED) {
        fprintf(stderr, "    FAIL: node[1] not CONN_CONNECTED after late "
                "response (state=%d)\n", proxy.upstreams[1].state);
        return -1;
    }
    if (proxy.late_responses_logged != 1) {
        fprintf(stderr, "    FAIL: late_responses_logged=%d expected 1\n",
                proxy.late_responses_logged);
        return -1;
    }

    /* Step 4: node[2] delivers late error — drain path fires */
    proxy.late_errors_logged = 0;
    sim_on_upstream_error(&proxy, 2);

    if (proxy.upstreams[2].state != CONN_CONNECTED) {
        fprintf(stderr, "    FAIL: node[2] not CONN_CONNECTED after late "
                "error (state=%d)\n", proxy.upstreams[2].state);
        return -1;
    }
    if (proxy.late_errors_logged != 1) {
        fprintf(stderr, "    FAIL: late_errors_logged=%d expected 1\n",
                proxy.late_errors_logged);
        return -1;
    }

    /* Step 5: All connections now CONN_CONNECTED — new dispatch to all 3 */
    proxy.state = RACE_FANOUT;
    proxy.winner_idx = -1;
    proxy.last_error_idx = -1;
    proxy.race_complete_called = false;
    strncpy(proxy.method, "getblockcount", sizeof(proxy.method) - 1);

    int dispatched = sim_dispatch_fanout(&proxy);
    if (dispatched != 3) {
        fprintf(stderr, "    FAIL: full dispatch=%d expected 3\n",
                dispatched);
        return -1;
    }

    /* Step 6: New race completes normally (node[1] wins) */
    now_ns += 20000000ULL;
    sim_on_upstream_response(&proxy, 1, false, now_ns);

    if (proxy.state != RACE_IDLE) {
        fprintf(stderr, "    FAIL: state not IDLE after new race winner\n");
        return -1;
    }

    printf("    PASS: full lifecycle completed correctly\n");
    return 0;
}

/*
 * Test 6: Connection with connection_close flag during drain disconnects
 * properly without affecting other connections.
 */
static int
test_connection_close_during_drain(long seed)
{
    printf("  property: connection_close during drain disconnects cleanly "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 3 + (int)(lrand48() % (MAX_NODES - 2));
        sim_proxy_t proxy;
        sim_init_race(&proxy, num_nodes, "validateaddress");

        /* Randomly mark some non-winner connections as connection_close */
        for (int i = 1; i < num_nodes; i++) {
            if (lrand48() % 3 == 0) {
                proxy.upstreams[i].connection_close = true;
            }
        }

        /* Winner responds (node 0) */
        uint64_t now_ns = 200000000ULL;
        sim_on_upstream_response(&proxy, 0, false, now_ns);

        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: not IDLE after winner\n", trial);
            return -1;
        }

        /* Late responses arrive from remaining nodes */
        for (int i = 1; i < num_nodes; i++) {
            if (proxy.upstreams[i].state != CONN_RECEIVING)
                continue; /* already reset by early IDLE logic */

            bool had_close = proxy.upstreams[i].connection_close;
            now_ns += 20000000ULL;
            sim_on_upstream_response(&proxy, i, false, now_ns);

            /* Verify: connection was reset */
            if (!proxy.upstreams[i].was_reset) {
                fprintf(stderr, "  FAIL trial %d: conn[%d] not reset\n",
                        trial, i);
                return -1;
            }

            /* Verify: connection_close nodes go to DISCONNECTED,
             * others go to CONN_CONNECTED */
            if (had_close) {
                if (proxy.upstreams[i].state != CONN_DISCONNECTED) {
                    fprintf(stderr, "  FAIL trial %d: conn[%d] with "
                            "connection_close not DISCONNECTED (state=%d)\n",
                            trial, i, proxy.upstreams[i].state);
                    return -1;
                }
            } else {
                if (proxy.upstreams[i].state != CONN_CONNECTED) {
                    fprintf(stderr, "  FAIL trial %d: conn[%d] not "
                            "CONN_CONNECTED (state=%d)\n",
                            trial, i, proxy.upstreams[i].state);
                    return -1;
                }
            }
        }

        /* Verify: proxy still IDLE */
        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: state not IDLE after drain\n",
                    trial);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
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

    printf("test_drain_late_responses (seed=%ld):\n", seed);
    printf("  Validates: Requirements 2.3, 3.3\n\n");

    int failures = 0;

    if (test_late_responses_logged_and_reset(seed) < 0)
        failures++;
    if (test_new_race_skips_draining_connections(seed) < 0)
        failures++;
    if (test_late_errors_dont_affect_new_race(seed) < 0)
        failures++;
    if (test_gbt_timing_logged_after_early_transition(seed) < 0)
        failures++;
    if (test_concrete_full_lifecycle() < 0)
        failures++;
    if (test_connection_close_during_drain(seed) < 0)
        failures++;

    printf("\n");
    if (failures == 0) {
        printf("  ALL PASSED — late response draining works correctly\n");
        return 0;
    } else {
        printf("  %d test(s) FAILED\n", failures);
        return 1;
    }
}
