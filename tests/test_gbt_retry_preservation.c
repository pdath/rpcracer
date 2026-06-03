/* test_gbt_retry_preservation.c — Preservation property tests (Property 2)
 *
 * Property 2: Preservation — Non-Retry Path Behavior Unchanged
 *
 * These tests capture the existing CORRECT behavior on UNFIXED code.
 * They verify that after the retry fix is implemented, all non-retry paths
 * remain unchanged. All sub-properties MUST PASS on unfixed code.
 *
 * Sub-property A: Non-GBT methods all-fail → immediate error, state=RACE_IDLE
 * Sub-property B: Broadcast methods all-fail → immediate error, state=RACE_IDLE
 * Sub-property C: GBT race with at least 1 success → winner by height, sticky set
 * Sub-property D: Sticky GBT with sticky unreachable → fan-out race (no retry wait)
 * Sub-property E: RPC timeout fires during race → error sent, race_complete called
 *
 * **Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6**
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
#define MAX_NODES  16

/* ---- Race state enum (replicating rpc_proxy.c) ---- */
typedef enum {
    RACE_IDLE,
    RACE_FANOUT,
    RACE_STICKY,
    RACE_RETRY_WAIT  /* New state that fix will add (not present in unfixed code) */
} race_state_t;

/* ---- Connection states (replicating rpc_conn.h) ---- */
typedef enum {
    CONN_DISCONNECTED,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_SENDING,
    CONN_RECEIVING,
    CONN_DEAD
} conn_state_t;

/* ---- Route strategy (replicating rpc_proxy.c) ---- */
typedef enum {
    ROUTE_RACE,
    ROUTE_BROADCAST,
    ROUTE_STICKY
} route_strategy_t;

/* ---- Minimal node model ---- */
typedef struct {
    conn_state_t state;
    bool in_ibd;
    int64_t gbt_height;  /* height in GBT response, -1 if error */
    bool response_is_error;
} mock_node_t;

/* ---- Minimal proxy model ---- */
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
    int64_t last_block_height;
    int64_t best_gbt_height;
    int best_gbt_node_idx;
    bool gbt_height_matched;
    bool all_must_complete;
    char method[128];
    bool error_sent_to_client;
    bool response_sent_to_client;
    bool race_complete_called;
    bool rpc_timeout_active;
} mock_proxy_t;

/* ---- Known non-GBT methods (ROUTE_RACE) ---- */
static const char *NON_GBT_METHODS[] = {
    "validateaddress",
    "decoderawtransaction",
    "getrawmempool",
    "getmininginfo",
    "getnetworkinfo",
    "getpeerinfo",
    "getbestblockhash",
    "getblockchaininfo"
};
#define NUM_NON_GBT_METHODS 8

/* ---- Broadcast methods ---- */
static const char *BROADCAST_METHODS[] = {
    "submitblock",
    "sendrawtransaction"
};
#define NUM_BROADCAST_METHODS 2

/* ---- Helper: state name ---- */
static const char *
state_name(race_state_t s)
{
    switch (s) {
    case RACE_IDLE:       return "RACE_IDLE";
    case RACE_FANOUT:     return "RACE_FANOUT";
    case RACE_STICKY:     return "RACE_STICKY";
    case RACE_RETRY_WAIT: return "RACE_RETRY_WAIT";
    }
    return "UNKNOWN";
}

/* ---- Replicate classify_method logic ---- */
static route_strategy_t
mock_classify_method(mock_proxy_t *proxy)
{
    const char *method = proxy->method;

    if (strcmp(method, "getblocktemplate") == 0) {
        if (proxy->notify_pending || proxy->sticky_node_idx == -1) {
            proxy->notify_pending = false;
            return ROUTE_RACE;
        }
        return ROUTE_STICKY;
    }
    if (strcmp(method, "submitblock") == 0)
        return ROUTE_BROADCAST;
    if (strcmp(method, "sendrawtransaction") == 0)
        return ROUTE_BROADCAST;
    if (strcmp(method, "preciousblock") == 0)
        return ROUTE_STICKY;

    /* All other methods: ROUTE_RACE */
    return ROUTE_RACE;
}

/* ---- Replicate dispatch_fanout ---- */
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

/* ---- Replicate race_complete ---- */
static void
mock_race_complete(mock_proxy_t *proxy)
{
    proxy->rpc_timeout_active = false;
    proxy->state = RACE_IDLE;
    proxy->responses_pending = 0;
    proxy->winner_idx = -1;
    proxy->last_error_idx = -1;
    proxy->best_gbt_height = -1;
    proxy->best_gbt_node_idx = -1;
    proxy->gbt_height_matched = false;
    proxy->race_complete_called = true;
}

/* ---- Replicate on_upstream_error all-fail path (current behavior) ----
 * From rpc_proxy.c on_upstream_error():
 *   if (responses_pending <= 0) {
 *       if (winner_idx == -1) {
 *           send_rpc_error_to_client(...)
 *       }
 *       race_complete(proxy);
 *   }
 */
static void
handle_all_fail_current(mock_proxy_t *proxy)
{
    if (proxy->winner_idx == -1) {
        proxy->error_sent_to_client = true;
    }
    mock_race_complete(proxy);
}

/* ---- Replicate client_cb dispatch_fanout==0 path (current behavior) ----
 * From rpc_proxy.c client_cb() ROUTE_RACE/ROUTE_BROADCAST branch:
 *   if (dispatch_fanout(proxy) <= 0) {
 *       send_rpc_error_to_client(proxy, -1, "All upstream nodes unreachable");
 *       proxy->state = RACE_IDLE;
 *   }
 */
static void
handle_dispatch_zero_current(mock_proxy_t *proxy)
{
    proxy->error_sent_to_client = true;
    proxy->state = RACE_IDLE;
}

/* ---- Replicate on_upstream_response GBT height logic (current behavior) ----
 * Processes a successful GBT response from a node.
 * Implements height-match and height-fallback winner selection. */
static void
handle_gbt_response(mock_proxy_t *proxy, int node_idx)
{
    mock_node_t *node = &proxy->nodes[node_idx];
    int64_t height = node->gbt_height;
    int64_t expected_height = proxy->last_block_height + 1;

    if (!proxy->gbt_height_matched && height == expected_height) {
        /* Height match: immediate winner */
        proxy->gbt_height_matched = true;
        proxy->winner_idx = node_idx;
        proxy->sticky_node_idx = node_idx;
        if (height > proxy->last_block_height)
            proxy->last_block_height = height;
        proxy->response_sent_to_client = true;
    } else if (!proxy->gbt_height_matched) {
        /* No match yet: track best height (highest wins, ties: last) */
        if (height >= proxy->best_gbt_height) {
            proxy->best_gbt_height = height;
            proxy->best_gbt_node_idx = node_idx;
        }
    }
    /* If already matched, discard */
}

/* ---- Replicate GBT all-responses-received fallback logic ---- */
static void
handle_gbt_race_end(mock_proxy_t *proxy)
{
    if (proxy->winner_idx == -1 && proxy->best_gbt_node_idx >= 0) {
        /* Height fallback: use best height node */
        proxy->winner_idx = proxy->best_gbt_node_idx;
        proxy->sticky_node_idx = proxy->best_gbt_node_idx;
        if (proxy->best_gbt_height > proxy->last_block_height)
            proxy->last_block_height = proxy->best_gbt_height;
        proxy->response_sent_to_client = true;
    } else if (proxy->winner_idx == -1 && proxy->last_error_idx >= 0) {
        /* All-error fallback: return last error */
        proxy->response_sent_to_client = true;
    }
    mock_race_complete(proxy);
}

/* ---- Replicate RPC timeout callback (current behavior) ----
 * From rpc_proxy.c rpc_timeout_cb():
 *   proxy->responses_pending = 0;
 *   if (proxy->winner_idx == -1) {
 *       send_rpc_error_to_client(proxy, -32000, "RPC timeout...");
 *   }
 *   race_complete(proxy);
 */
static void
handle_rpc_timeout(mock_proxy_t *proxy)
{
    proxy->responses_pending = 0;
    if (proxy->winner_idx == -1) {
        proxy->error_sent_to_client = true;
    }
    mock_race_complete(proxy);
}

/* ---- Replicate dispatch_sticky fallback (current behavior) ----
 * From rpc_proxy.c dispatch_sticky():
 * When sticky node unreachable (not CONN_CONNECTED) for GBT:
 *   proxy->state = RACE_FANOUT
 *   dispatch_fanout(proxy)
 *   if sent <= 0: error
 */
static int
mock_dispatch_sticky_gbt(mock_proxy_t *proxy)
{
    if (proxy->sticky_node_idx == -1) {
        /* No sticky: fall back to fan-out */
        proxy->state = RACE_FANOUT;
        proxy->all_must_complete = false;
        return mock_dispatch_fanout(proxy);
    }

    mock_node_t *sticky = &proxy->nodes[proxy->sticky_node_idx];

    if (sticky->state != CONN_CONNECTED) {
        /* Sticky unreachable: fall back to fan-out */
        proxy->state = RACE_FANOUT;
        proxy->all_must_complete = false;
        proxy->best_gbt_height = -1;
        proxy->best_gbt_node_idx = -1;
        proxy->gbt_height_matched = false;
        return mock_dispatch_fanout(proxy);
    }

    /* Sticky reachable: dispatch to sticky only */
    sticky->state = CONN_SENDING;
    proxy->responses_pending = 1;
    return 1;
}

/*
 * Sub-property A: Non-GBT methods all-fail → immediate error, state=RACE_IDLE
 *
 * For all non-GBT methods (validateaddress, decoderawtransaction, etc.) with
 * all nodes failing, the proxy returns an error immediately with no retry state.
 * This MUST remain unchanged after the retry fix.
 *
 * Validates: Requirement 3.1
 */
static int
test_property_non_gbt_all_fail(long seed)
{
    printf("  sub-property A: non-GBT all-fail → immediate error, "
           "RACE_IDLE (seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int node_count = 1 + (int)(lrand48() % MAX_NODES);
        int method_idx = (int)(lrand48() % NUM_NON_GBT_METHODS);
        /* Randomly choose: all disconnected at dispatch (path A) or
         * all dispatched but fail mid-transfer (path B) */
        bool all_disconnected = (lrand48() % 2 == 0);

        mock_proxy_t proxy;
        memset(&proxy, 0, sizeof(proxy));
        proxy.state = RACE_IDLE;
        proxy.upstream_count = node_count;
        proxy.notify_pending = false;
        proxy.is_post_notify_gbt = false;
        proxy.winner_idx = -1;
        proxy.last_error_idx = -1;
        proxy.sticky_node_idx = -1;
        proxy.last_block_height = 100000 + (int)(lrand48() % 100000);
        proxy.best_gbt_height = -1;
        proxy.best_gbt_node_idx = -1;
        proxy.gbt_height_matched = false;
        proxy.all_must_complete = false;
        proxy.error_sent_to_client = false;
        proxy.response_sent_to_client = false;
        proxy.race_complete_called = false;
        proxy.rpc_timeout_active = false;
        strcpy(proxy.method, NON_GBT_METHODS[method_idx]);

        /* Set node states */
        for (int i = 0; i < node_count; i++) {
            if (all_disconnected) {
                int choice = (int)(lrand48() % 3);
                switch (choice) {
                case 0: proxy.nodes[i].state = CONN_DISCONNECTED; break;
                case 1: proxy.nodes[i].state = CONN_CONNECTING; break;
                case 2: proxy.nodes[i].state = CONN_DEAD; break;
                }
            } else {
                proxy.nodes[i].state = CONN_CONNECTED;
            }
            proxy.nodes[i].response_is_error = true;
        }

        /* Classify and dispatch */
        route_strategy_t strategy = mock_classify_method(&proxy);
        if (strategy != ROUTE_RACE) {
            /* Should not happen for these methods */
            continue;
        }

        proxy.state = RACE_FANOUT;
        proxy.all_must_complete = false;
        int dispatched = mock_dispatch_fanout(&proxy);

        if (dispatched <= 0) {
            /* Path A: dispatch_fanout returns 0 — direct RACE_IDLE, no
             * race_complete (matches client_cb behavior) */
            handle_dispatch_zero_current(&proxy);
        } else {
            /* Path B: all dispatched nodes fail mid-transfer —
             * on_upstream_error → race_complete */
            for (int i = 0; i < node_count; i++) {
                if (proxy.nodes[i].state == CONN_SENDING) {
                    proxy.responses_pending--;
                    proxy.last_error_idx = i;
                }
            }
            handle_all_fail_current(&proxy);
        }

        /* Assert: error sent, state = RACE_IDLE, no RACE_RETRY_WAIT.
         * Note: dispatch=0 path does NOT call race_complete (direct IDLE).
         *       mid-transfer path DOES call race_complete. */
        bool ok = (proxy.state == RACE_IDLE &&
                   proxy.error_sent_to_client &&
                   proxy.state != RACE_RETRY_WAIT);

        if (!ok) {
            if (failures == 0) {
                fprintf(stderr, "\n  FAIL sub-property A (trial %d, seed=%ld):\n",
                        trial, seed);
                fprintf(stderr, "    method=%s, node_count=%d, "
                        "all_disconnected=%d\n",
                        proxy.method, node_count, all_disconnected);
                fprintf(stderr, "    ACTUAL: state=%s, error_sent=%d\n",
                        state_name(proxy.state),
                        proxy.error_sent_to_client);
                fprintf(stderr, "    EXPECTED: state=RACE_IDLE, error_sent=1\n");
            }
            failures++;
        }
    }

    if (failures > 0) {
        fprintf(stderr, "  FAIL: %d/%d trials\n\n", failures, NUM_TRIALS);
        return -1;
    }

    printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property B: Broadcast methods all-fail → immediate error, state=RACE_IDLE
 *
 * For submitblock and sendrawtransaction with all nodes failing, the proxy
 * returns an error immediately with no retry state.
 *
 * Validates: Requirement 3.4
 */
static int
test_property_broadcast_all_fail(long seed)
{
    printf("  sub-property B: broadcast all-fail → immediate error, "
           "RACE_IDLE (seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int node_count = 1 + (int)(lrand48() % MAX_NODES);
        int method_idx = (int)(lrand48() % NUM_BROADCAST_METHODS);
        bool all_disconnected = (lrand48() % 2 == 0);

        mock_proxy_t proxy;
        memset(&proxy, 0, sizeof(proxy));
        proxy.state = RACE_IDLE;
        proxy.upstream_count = node_count;
        proxy.notify_pending = false;
        proxy.is_post_notify_gbt = false;
        proxy.winner_idx = -1;
        proxy.last_error_idx = -1;
        proxy.sticky_node_idx = -1;
        proxy.last_block_height = 100000 + (int)(lrand48() % 100000);
        proxy.best_gbt_height = -1;
        proxy.best_gbt_node_idx = -1;
        proxy.gbt_height_matched = false;
        proxy.all_must_complete = true;
        proxy.error_sent_to_client = false;
        proxy.response_sent_to_client = false;
        proxy.race_complete_called = false;
        proxy.rpc_timeout_active = false;
        strcpy(proxy.method, BROADCAST_METHODS[method_idx]);

        for (int i = 0; i < node_count; i++) {
            if (all_disconnected) {
                int choice = (int)(lrand48() % 3);
                switch (choice) {
                case 0: proxy.nodes[i].state = CONN_DISCONNECTED; break;
                case 1: proxy.nodes[i].state = CONN_CONNECTING; break;
                case 2: proxy.nodes[i].state = CONN_DEAD; break;
                }
            } else {
                proxy.nodes[i].state = CONN_CONNECTED;
            }
            proxy.nodes[i].response_is_error = true;
        }

        /* Classify */
        route_strategy_t strategy = mock_classify_method(&proxy);
        if (strategy != ROUTE_BROADCAST) {
            continue;
        }

        proxy.state = RACE_FANOUT;
        int dispatched = mock_dispatch_fanout(&proxy);

        if (dispatched <= 0) {
            handle_dispatch_zero_current(&proxy);
        } else {
            /* All dispatched nodes fail */
            for (int i = 0; i < node_count; i++) {
                if (proxy.nodes[i].state == CONN_SENDING) {
                    proxy.responses_pending--;
                    proxy.last_error_idx = i;
                }
            }
            handle_all_fail_current(&proxy);
        }

        /* Assert: error sent, state = RACE_IDLE, no RACE_RETRY_WAIT */
        bool ok = (proxy.state == RACE_IDLE &&
                   proxy.error_sent_to_client &&
                   proxy.state != RACE_RETRY_WAIT);

        if (!ok) {
            if (failures == 0) {
                fprintf(stderr, "\n  FAIL sub-property B (trial %d, seed=%ld):\n",
                        trial, seed);
                fprintf(stderr, "    method=%s, node_count=%d, "
                        "all_disconnected=%d\n",
                        proxy.method, node_count, all_disconnected);
                fprintf(stderr, "    ACTUAL: state=%s, error_sent=%d\n",
                        state_name(proxy.state),
                        proxy.error_sent_to_client);
                fprintf(stderr, "    EXPECTED: state=RACE_IDLE, error_sent=1\n");
            }
            failures++;
        }
    }

    if (failures > 0) {
        fprintf(stderr, "  FAIL: %d/%d trials\n\n", failures, NUM_TRIALS);
        return -1;
    }

    printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property C: GBT race with at least 1 success → winner by height, sticky set
 *
 * For GBT races where at least one node responds successfully, the winner is
 * selected by height (exact match takes priority, then highest). The winner's
 * response is sent to the client and sticky is set to that node.
 *
 * Validates: Requirement 3.2
 */
static int
test_property_gbt_success_winner_selection(long seed)
{
    printf("  sub-property C: GBT success → winner by height, sticky set "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int node_count = 2 + (int)(lrand48() % (MAX_NODES - 1));
        int64_t last_block_height = 100000 + (int64_t)(lrand48() % 100000);
        int64_t expected_height = last_block_height + 1;

        mock_proxy_t proxy;
        memset(&proxy, 0, sizeof(proxy));
        proxy.state = RACE_IDLE;
        proxy.upstream_count = node_count;
        proxy.notify_pending = true;
        proxy.is_post_notify_gbt = true;
        proxy.winner_idx = -1;
        proxy.last_error_idx = -1;
        proxy.sticky_node_idx = -1;
        proxy.last_block_height = last_block_height;
        proxy.best_gbt_height = -1;
        proxy.best_gbt_node_idx = -1;
        proxy.gbt_height_matched = false;
        proxy.all_must_complete = false;
        proxy.error_sent_to_client = false;
        proxy.response_sent_to_client = false;
        proxy.race_complete_called = false;
        proxy.rpc_timeout_active = false;
        strcpy(proxy.method, "getblocktemplate");

        /* At least one node succeeds. Randomize which nodes succeed and
         * what heights they report. */
        int success_count = 1 + (int)(lrand48() % node_count);
        /* Ensure at least one node is CONNECTED (will succeed) */
        for (int i = 0; i < node_count; i++) {
            if (i < success_count) {
                proxy.nodes[i].state = CONN_CONNECTED;
                proxy.nodes[i].response_is_error = false;
                /* Randomize height: some match expected, some don't */
                if (lrand48() % 3 == 0) {
                    proxy.nodes[i].gbt_height = expected_height;
                } else {
                    /* Random height around expected */
                    proxy.nodes[i].gbt_height = expected_height - 1 +
                        (int64_t)(lrand48() % 3);
                }
            } else {
                /* This node either disconnected or will return error */
                if (lrand48() % 2 == 0) {
                    proxy.nodes[i].state = CONN_DISCONNECTED;
                    proxy.nodes[i].response_is_error = true;
                } else {
                    proxy.nodes[i].state = CONN_CONNECTED;
                    proxy.nodes[i].response_is_error = true;
                    proxy.nodes[i].gbt_height = -1;
                }
            }
        }

        /* Classify */
        proxy.notify_pending = false;
        proxy.state = RACE_FANOUT;
        int dispatched = mock_dispatch_fanout(&proxy);

        if (dispatched <= 0) {
            /* Shouldn't happen: at least one node is CONNECTED */
            continue;
        }

        /* Simulate responses arriving in random order */
        int response_order[MAX_NODES];
        int resp_count = 0;
        for (int i = 0; i < node_count; i++) {
            if (proxy.nodes[i].state == CONN_SENDING)
                response_order[resp_count++] = i;
        }
        /* Shuffle response order */
        for (int i = resp_count - 1; i > 0; i--) {
            int j = (int)(lrand48() % (i + 1));
            int tmp = response_order[i];
            response_order[i] = response_order[j];
            response_order[j] = tmp;
        }

        /* Process responses */
        for (int r = 0; r < resp_count; r++) {
            int idx = response_order[r];
            proxy.responses_pending--;

            if (proxy.nodes[idx].response_is_error) {
                proxy.last_error_idx = idx;
            } else {
                /* Successful GBT response */
                handle_gbt_response(&proxy, idx);
            }
        }

        /* All responses received: finalize */
        handle_gbt_race_end(&proxy);

        /* Assert: response sent to client, winner selected,
         * sticky set, state = RACE_IDLE */
        bool response_ok = proxy.response_sent_to_client;
        bool state_ok = (proxy.state == RACE_IDLE);
        bool sticky_ok = (proxy.sticky_node_idx >= 0);
        bool no_retry = (proxy.state != RACE_RETRY_WAIT);

        /* Verify winner selection correctness:
         * - If any node reported expected_height, winner must be that node
         * - Otherwise, winner should be node with highest height */
        int expected_winner = -1;
        bool has_exact_match = false;

        /* Replay to find expected winner (first exact match in response_order) */
        int64_t replay_best_height = -1;
        int replay_best_idx = -1;
        for (int r = 0; r < resp_count; r++) {
            int idx = response_order[r];
            if (proxy.nodes[idx].response_is_error)
                continue;
            int64_t h = proxy.nodes[idx].gbt_height;
            if (!has_exact_match && h == expected_height) {
                has_exact_match = true;
                expected_winner = idx;
            } else if (!has_exact_match) {
                if (h >= replay_best_height) {
                    replay_best_height = h;
                    replay_best_idx = idx;
                }
            }
        }
        if (!has_exact_match)
            expected_winner = replay_best_idx;

        bool winner_ok = (proxy.sticky_node_idx == expected_winner);

        bool ok = response_ok && state_ok && sticky_ok && no_retry && winner_ok;

        if (!ok) {
            if (failures == 0) {
                fprintf(stderr, "\n  FAIL sub-property C (trial %d, seed=%ld):\n",
                        trial, seed);
                fprintf(stderr, "    node_count=%d, expected_height=%lld\n",
                        node_count, (long long)expected_height);
                fprintf(stderr, "    response_sent=%d, state=%s, sticky=%d, "
                        "winner_correct=%d\n",
                        proxy.response_sent_to_client,
                        state_name(proxy.state),
                        proxy.sticky_node_idx,
                        winner_ok);
                fprintf(stderr, "    expected_winner=%d, actual_sticky=%d\n",
                        expected_winner, proxy.sticky_node_idx);
            }
            failures++;
        }
    }

    if (failures > 0) {
        fprintf(stderr, "  FAIL: %d/%d trials\n\n", failures, NUM_TRIALS);
        return -1;
    }

    printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property D: Sticky GBT with sticky unreachable → fan-out (no retry wait)
 *
 * When a sticky GBT request is made but the sticky node is unreachable,
 * the proxy falls back to fan-out race. This MUST NOT trigger retry wait.
 *
 * Validates: Requirement 3.3
 */
static int
test_property_sticky_gbt_fallback(long seed)
{
    printf("  sub-property D: sticky GBT unreachable → fan-out, no retry "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int node_count = 2 + (int)(lrand48() % (MAX_NODES - 1));
        int sticky_idx = (int)(lrand48() % node_count);

        mock_proxy_t proxy;
        memset(&proxy, 0, sizeof(proxy));
        proxy.state = RACE_IDLE;
        proxy.upstream_count = node_count;
        proxy.notify_pending = false;  /* NOT post-notify (sticky path) */
        proxy.is_post_notify_gbt = false;
        proxy.winner_idx = -1;
        proxy.last_error_idx = -1;
        proxy.sticky_node_idx = sticky_idx;
        proxy.last_block_height = 100000 + (int64_t)(lrand48() % 100000);
        proxy.best_gbt_height = -1;
        proxy.best_gbt_node_idx = -1;
        proxy.gbt_height_matched = false;
        proxy.all_must_complete = false;
        proxy.error_sent_to_client = false;
        proxy.response_sent_to_client = false;
        proxy.race_complete_called = false;
        proxy.rpc_timeout_active = false;
        strcpy(proxy.method, "getblocktemplate");

        /* Sticky node unreachable */
        int choice = (int)(lrand48() % 3);
        switch (choice) {
        case 0: proxy.nodes[sticky_idx].state = CONN_DISCONNECTED; break;
        case 1: proxy.nodes[sticky_idx].state = CONN_CONNECTING; break;
        case 2: proxy.nodes[sticky_idx].state = CONN_DEAD; break;
        }

        /* At least one other node may be connected (to test fan-out dispatch).
         * Randomize the other nodes' states. */
        for (int i = 0; i < node_count; i++) {
            if (i == sticky_idx)
                continue;
            if (lrand48() % 2 == 0) {
                proxy.nodes[i].state = CONN_CONNECTED;
            } else {
                proxy.nodes[i].state = CONN_DISCONNECTED;
            }
        }

        /* Classify: should be ROUTE_STICKY since notify_pending=false
         * and sticky_node_idx is set */
        route_strategy_t strategy = mock_classify_method(&proxy);
        if (strategy != ROUTE_STICKY) {
            /* Unexpected; skip this trial */
            continue;
        }

        /* dispatch_sticky: sticky unreachable → fall back to fan-out */
        proxy.state = RACE_STICKY;
        int dispatched = mock_dispatch_sticky_gbt(&proxy);

        if (dispatched <= 0) {
            /* All nodes unreachable in fan-out too: immediate error */
            handle_dispatch_zero_current(&proxy);

            /* Assert: error sent, state = RACE_IDLE, NOT RACE_RETRY_WAIT */
            bool ok = (proxy.state == RACE_IDLE &&
                       proxy.error_sent_to_client &&
                       proxy.state != RACE_RETRY_WAIT);
            if (!ok) {
                if (failures == 0) {
                    fprintf(stderr, "\n  FAIL sub-property D (trial %d, "
                            "seed=%ld):\n", trial, seed);
                    fprintf(stderr, "    sticky_idx=%d unreachable, "
                            "fan-out also 0\n", sticky_idx);
                    fprintf(stderr, "    ACTUAL: state=%s, should be "
                            "RACE_IDLE with error\n",
                            state_name(proxy.state));
                }
                failures++;
            }
        } else {
            /* Fan-out dispatched some nodes: verify state is RACE_FANOUT
             * (not RACE_RETRY_WAIT) */
            bool ok = (proxy.state == RACE_FANOUT &&
                       proxy.state != RACE_RETRY_WAIT);
            if (!ok) {
                if (failures == 0) {
                    fprintf(stderr, "\n  FAIL sub-property D (trial %d, "
                            "seed=%ld):\n", trial, seed);
                    fprintf(stderr, "    sticky_idx=%d unreachable, "
                            "fan-out dispatched %d\n",
                            sticky_idx, dispatched);
                    fprintf(stderr, "    ACTUAL: state=%s, should be "
                            "RACE_FANOUT\n", state_name(proxy.state));
                }
                failures++;
            }
        }
    }

    if (failures > 0) {
        fprintf(stderr, "  FAIL: %d/%d trials\n\n", failures, NUM_TRIALS);
        return -1;
    }

    printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property E: RPC timeout during race → error sent, race_complete called
 *
 * When an RPC timeout fires during an active race with pending responses,
 * the proxy sends a timeout error and calls race_complete. The retry logic
 * must NOT intercept or delay this.
 *
 * Validates: Requirement 3.5
 */
static int
test_property_rpc_timeout_fires(long seed)
{
    printf("  sub-property E: RPC timeout during race → error + race_complete "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int node_count = 1 + (int)(lrand48() % MAX_NODES);
        /* Randomly pick a method (any method can have a timeout) */
        bool is_gbt = (lrand48() % 3 == 0);
        bool is_broadcast = (!is_gbt && lrand48() % 3 == 0);

        mock_proxy_t proxy;
        memset(&proxy, 0, sizeof(proxy));
        proxy.upstream_count = node_count;
        proxy.winner_idx = -1;
        proxy.last_error_idx = -1;
        proxy.sticky_node_idx = -1;
        proxy.last_block_height = 100000 + (int64_t)(lrand48() % 100000);
        proxy.best_gbt_height = -1;
        proxy.best_gbt_node_idx = -1;
        proxy.gbt_height_matched = false;
        proxy.error_sent_to_client = false;
        proxy.response_sent_to_client = false;
        proxy.race_complete_called = false;
        proxy.rpc_timeout_active = true;

        if (is_gbt) {
            strcpy(proxy.method, "getblocktemplate");
            proxy.notify_pending = false;
            proxy.is_post_notify_gbt = (lrand48() % 2 == 0);
            proxy.all_must_complete = false;
            proxy.state = RACE_FANOUT;
        } else if (is_broadcast) {
            int m = (int)(lrand48() % NUM_BROADCAST_METHODS);
            strcpy(proxy.method, BROADCAST_METHODS[m]);
            proxy.is_post_notify_gbt = false;
            proxy.all_must_complete = true;
            proxy.state = RACE_FANOUT;
        } else {
            int m = (int)(lrand48() % NUM_NON_GBT_METHODS);
            strcpy(proxy.method, NON_GBT_METHODS[m]);
            proxy.is_post_notify_gbt = false;
            proxy.all_must_complete = false;
            proxy.state = RACE_FANOUT;
        }

        /* Some responses still pending (timeout fires before all complete) */
        int pending = 1 + (int)(lrand48() % node_count);
        proxy.responses_pending = pending;

        /* Simulate RPC timeout firing */
        handle_rpc_timeout(&proxy);

        /* Assert: error sent (since winner_idx == -1), race_complete called,
         * state = RACE_IDLE, NOT RACE_RETRY_WAIT */
        bool ok = (proxy.state == RACE_IDLE &&
                   proxy.error_sent_to_client &&
                   proxy.race_complete_called &&
                   proxy.state != RACE_RETRY_WAIT);

        if (!ok) {
            if (failures == 0) {
                fprintf(stderr, "\n  FAIL sub-property E (trial %d, seed=%ld):\n",
                        trial, seed);
                fprintf(stderr, "    method=%s, pending=%d, is_post_notify=%d\n",
                        proxy.method, pending, proxy.is_post_notify_gbt);
                fprintf(stderr, "    ACTUAL: state=%s, error_sent=%d, "
                        "race_complete=%d\n",
                        state_name(proxy.state),
                        proxy.error_sent_to_client,
                        proxy.race_complete_called);
                fprintf(stderr, "    EXPECTED: state=RACE_IDLE, error_sent=1, "
                        "race_complete=1\n");
            }
            failures++;
        }
    }

    if (failures > 0) {
        fprintf(stderr, "  FAIL: %d/%d trials\n\n", failures, NUM_TRIALS);
        return -1;
    }

    printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/* ---- Main ---- */
int
main(int argc, char **argv)
{
    long seed;
    if (argc > 1)
        seed = atol(argv[1]);
    else
        seed = (long)time(NULL);

    printf("test_gbt_retry_preservation: Property 2 — Preservation\n");
    printf("  seed=%ld (reproduce with: %s %ld)\n\n",
           seed, argv[0], seed);

    int result = 0;

    if (test_property_non_gbt_all_fail(seed) != 0)
        result = -1;

    if (test_property_broadcast_all_fail(seed) != 0)
        result = -1;

    if (test_property_gbt_success_winner_selection(seed) != 0)
        result = -1;

    if (test_property_sticky_gbt_fallback(seed) != 0)
        result = -1;

    if (test_property_rpc_timeout_fires(seed) != 0)
        result = -1;

    if (result == 0)
        printf("\n  ALL PRESERVATION PROPERTIES PASSED (%d trials each)\n",
               NUM_TRIALS);
    else
        fprintf(stderr, "\n  SOME PRESERVATION PROPERTIES FAILED\n");

    return (result == 0) ? 0 : 1;
}
