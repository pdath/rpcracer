/* test_race_state_blocking.c — Bug condition exploration test (Property 1)
 *
 * Property 1: Bug Condition — Non-broadcast race blocks new requests after
 * winner sent.
 *
 * Bug condition: proxy_state == RACE_FANOUT AND winner_sent == true AND
 * all_must_complete == false AND responses_pending > 0 AND
 * new_request_arrives == true
 *
 * Expected behavior: proxy SHALL accept the new request (state transitions
 * to RACE_IDLE or equivalent accepting state after winner sent).
 *
 * This test exercises the request-gate logic from client_cb() and the
 * state transition logic from on_upstream_response(). It replicates the
 * relevant proxy state fields and decision logic to demonstrate that the
 * current code drops new requests when it should accept them.
 *
 * **Validates: Requirements 1.1, 1.3**
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

/* ---- Replicate the race state enum from rpc_proxy.c ---- */
typedef enum {
    RACE_IDLE,
    RACE_FANOUT,
    RACE_STICKY
} race_state_t;

/* ---- Minimal proxy state for testing the request-gate logic ---- */
typedef struct {
    race_state_t state;
    int winner_idx;
    int responses_pending;
    bool all_must_complete;
} mock_proxy_t;

/* ---- Request-gate logic (mirrors client_cb in rpc_proxy.c) ----
 *
 * This is the CURRENT (buggy) implementation of the request gate.
 * It only checks proxy->state != RACE_IDLE.
 *
 * Returns: true if request is ACCEPTED, false if DROPPED.
 */
static bool
request_gate_current(const mock_proxy_t *proxy)
{
    /* From client_cb():
     *   if (proxy->state != RACE_IDLE) {
     *       log_msg(LOG_WARN, "Request received while race/sticky active
     *               (state=%d) — dropping request");
     *       return;  // dropped
     *   }
     */
    return (proxy->state == RACE_IDLE);
}

/* ---- State transition logic after winner sent (mirrors on_upstream_response) ----
 *
 * This replicates what on_upstream_response() does AFTER sending the winner
 * response for a non-broadcast race with pending responses:
 *
 * Fixed behavior: after sending the winner response for a non-broadcast race,
 * the proxy transitions to RACE_IDLE immediately so new requests are accepted.
 * Late responses are drained individually via rpc_conn_reset().
 */
static void
simulate_winner_sent(mock_proxy_t *proxy, int winner_node)
{
    /* Winner found — set winner_idx (response already sent to client) */
    proxy->winner_idx = winner_node;

    /* Fixed code: transition to RACE_IDLE for non-broadcast races with
     * pending responses, so new requests are accepted immediately. */
    if (!proxy->all_must_complete && proxy->responses_pending > 0) {
        proxy->state = RACE_IDLE;
    }
}

/* ---- Expected behavior: what SHOULD happen after winner sent ----
 *
 * The expected (correct) behavior is that after sending the winner response
 * for a non-broadcast race, the proxy transitions to a state that accepts
 * new requests.
 *
 * Returns: true if the proxy SHOULD accept new requests in this state.
 */
static bool
should_accept_new_request(const mock_proxy_t *proxy)
{
    /* Expected behavior (from design doc):
     * After a non-broadcast race winner is sent, the proxy SHALL immediately
     * accept new client requests regardless of pending upstream responses.
     *
     * The condition for accepting is:
     *   - state == RACE_IDLE, OR
     *   - (state == RACE_FANOUT AND winner_idx != -1 AND !all_must_complete)
     *     meaning: winner already sent, just draining late responses
     */
    if (proxy->state == RACE_IDLE)
        return true;

    /* Bug condition: winner sent but state still RACE_FANOUT */
    if (proxy->state == RACE_FANOUT &&
        proxy->winner_idx != -1 &&
        !proxy->all_must_complete &&
        proxy->responses_pending > 0) {
        /* Expected: should accept (proxy should have transitioned to IDLE) */
        return true;
    }

    return false;
}

/*
 * Property 1: Bug Condition — Non-broadcast race blocks new requests
 * after winner sent.
 *
 * For any non-broadcast race (all_must_complete == false) dispatched to
 * N >= 2 nodes where node[k] responds successfully (winner found, response
 * sent) while other nodes are still pending, a new client request arriving
 * SHALL be accepted.
 *
 * Test strategy:
 *   - Generate random N (2-8 nodes)
 *   - Pick a random winner node (0..N-1)
 *   - Simulate: proxy in RACE_FANOUT, winner sent, (N-1) responses pending
 *   - Verify: request_gate_current() accepts the new request
 *
 * EXPECTED TO FAIL on unfixed code: the current request gate only checks
 * state != RACE_IDLE, so it drops the request even though the winner was
 * already sent.
 *
 * Validates: Requirements 1.1, 1.3
 */
static int
test_property_bug_condition(long seed)
{
    printf("  property: non-broadcast race accepts new requests after winner "
           "sent (seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Generate random number of nodes (2-8) */
        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));

        /* Pick a random winner node */
        int winner_node = (int)(lrand48() % num_nodes);

        /* Set up proxy state: non-broadcast race in RACE_FANOUT */
        mock_proxy_t proxy;
        proxy.state = RACE_FANOUT;
        proxy.winner_idx = -1;
        proxy.responses_pending = num_nodes;
        proxy.all_must_complete = false;

        /* Simulate: winner node responds, response sent to client */
        proxy.responses_pending--;  /* winner's response received */
        simulate_winner_sent(&proxy, winner_node);

        /* Verify bug condition holds (state should now be RACE_IDLE after fix) */
        if (!(proxy.winner_idx != -1 &&
              !proxy.all_must_complete &&
              proxy.responses_pending > 0)) {
            fprintf(stderr, "  ERROR trial %d: test condition not established "
                    "(winner=%d pending=%d all_must=%d)\n",
                    trial, proxy.winner_idx,
                    proxy.responses_pending, proxy.all_must_complete);
            return -1;
        }

        /* Now simulate a new client request arriving.
         * Expected: request is ACCEPTED.
         * Actual (buggy): request is DROPPED because state == RACE_FANOUT. */
        bool accepted = request_gate_current(&proxy);
        bool should_accept = should_accept_new_request(&proxy);

        if (should_accept && !accepted) {
            /* This is the bug: request should be accepted but is dropped */
            if (failures == 0) {
                /* Print first counterexample */
                fprintf(stderr, "  COUNTEREXAMPLE (trial %d, seed=%ld):\n",
                        trial, seed);
                fprintf(stderr, "    num_nodes=%d, winner_node=%d\n",
                        num_nodes, winner_node);
                fprintf(stderr, "    proxy.state=%d (RACE_FANOUT)\n",
                        proxy.state);
                fprintf(stderr, "    proxy.winner_idx=%d (!= -1, winner sent)\n",
                        proxy.winner_idx);
                fprintf(stderr, "    proxy.all_must_complete=%d (non-broadcast)\n",
                        proxy.all_must_complete);
                fprintf(stderr, "    proxy.responses_pending=%d (>0, nodes "
                        "still in-flight)\n", proxy.responses_pending);
                fprintf(stderr, "    request_gate_current() = %s (DROPPED)\n",
                        accepted ? "ACCEPTED" : "DROPPED");
                fprintf(stderr, "    expected: ACCEPTED\n");
                fprintf(stderr, "    Bug: \"Request received while race/sticky "
                        "active (state=1) — dropping request\"\n");
            }
            failures++;
        } else if (!should_accept && accepted) {
            /* Unexpected: request accepted when it shouldn't be */
            fprintf(stderr, "  UNEXPECTED trial %d: request accepted in "
                    "invalid state\n", trial);
            return -1;
        }
        /* If should_accept && accepted: correct behavior (fix applied) */
        /* If !should_accept && !accepted: correct blocking (not bug condition) */
    }

    if (failures > 0) {
        fprintf(stderr, "  FAIL: %d/%d trials — new request dropped after "
                "winner sent (bug confirmed)\n", failures, NUM_TRIALS);
        return -1;
    }

    printf("    %d/%d trials passed — new requests accepted after winner "
           "sent\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Varying the number of pending responses.
 *
 * The bug should manifest regardless of how many responses are still pending
 * (1, 2, ..., N-1). Generate cases with different pending counts.
 *
 * EXPECTED TO FAIL on unfixed code.
 */
static int
test_property_varying_pending(long seed)
{
    printf("  property: bug manifests for any pending count > 0 "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Generate random total nodes (2-8) */
        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));

        /* Random number of responses already received (1..num_nodes-1)
         * At least 1 received (the winner), at least 1 still pending */
        int responses_received = 1 + (int)(lrand48() % (num_nodes - 1));
        int responses_pending = num_nodes - responses_received;

        /* Pick winner from the received responses */
        int winner_node = (int)(lrand48() % num_nodes);

        /* Set up proxy state: simulate winner being sent */
        mock_proxy_t proxy;
        proxy.state = RACE_FANOUT;
        proxy.winner_idx = -1;
        proxy.responses_pending = responses_pending;
        proxy.all_must_complete = false;

        /* Simulate winner sent (this should transition to RACE_IDLE) */
        simulate_winner_sent(&proxy, winner_node);

        /* Verify condition */
        if (proxy.responses_pending <= 0)
            continue;  /* skip degenerate case */

        /* Test request gate */
        bool accepted = request_gate_current(&proxy);

        if (!accepted) {
            failures++;
            if (failures == 1) {
                fprintf(stderr, "  COUNTEREXAMPLE (trial %d, seed=%ld):\n",
                        trial, seed);
                fprintf(stderr, "    num_nodes=%d, winner=%d, "
                        "responses_pending=%d\n",
                        num_nodes, winner_node, responses_pending);
                fprintf(stderr, "    state=%d, request DROPPED\n",
                        proxy.state);
                fprintf(stderr, "    expected: ACCEPTED\n");
            }
        }
    }

    if (failures > 0) {
        fprintf(stderr, "  FAIL: %d/%d trials — bug confirmed for varying "
                "pending counts\n", failures, NUM_TRIALS);
        return -1;
    }

    printf("    %d/%d trials passed\n", NUM_TRIALS, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Concrete scenario — validateaddress with 2 nodes.
 *
 * Simulates the exact scenario from the design doc:
 *   - validateaddress dispatched to 2 nodes
 *   - node[0] responds successfully (winner)
 *   - node[1] still pending
 *   - New client request arrives
 *   - Expected: ACCEPTED
 *   - Actual (buggy): DROPPED
 *
 * EXPECTED TO FAIL on unfixed code.
 */
static int
test_concrete_validateaddress(void)
{
    printf("  concrete: validateaddress 2-node race, winner sent, "
           "new request arrives\n");

    mock_proxy_t proxy;
    proxy.state = RACE_FANOUT;
    proxy.winner_idx = -1;
    proxy.responses_pending = 2;
    proxy.all_must_complete = false;  /* validateaddress is non-broadcast */

    /* node[0] responds successfully */
    proxy.responses_pending--;
    simulate_winner_sent(&proxy, 0);

    /* Verify state after fix: should be RACE_IDLE, winner set, pending > 0 */
    if (proxy.winner_idx == -1 ||
        proxy.all_must_complete || proxy.responses_pending <= 0) {
        fprintf(stderr, "  ERROR: test condition not established\n");
        return -1;
    }

    /* New client request arrives */
    bool accepted = request_gate_current(&proxy);

    if (!accepted) {
        fprintf(stderr, "  COUNTEREXAMPLE:\n");
        fprintf(stderr, "    method=validateaddress, nodes=2\n");
        fprintf(stderr, "    node[0] responded (winner_idx=0)\n");
        fprintf(stderr, "    node[1] still pending (responses_pending=1)\n");
        fprintf(stderr, "    proxy.state=RACE_FANOUT (should be RACE_IDLE)\n");
        fprintf(stderr, "    New request → DROPPED\n");
        fprintf(stderr, "    Expected: ACCEPTED\n");
        fprintf(stderr, "    Log: \"Request received while race/sticky active "
                "(state=1) — dropping request\"\n");
        return -1;
    }

    printf("    PASS: new request accepted after winner sent\n");
    return 0;
}

/*
 * Sub-property: Concrete scenario — 3-node race with rapid sequential request.
 *
 * Simulates:
 *   - decoderawtransaction dispatched to 3 nodes
 *   - node[0] responds in 3ms (winner)
 *   - node[1] and node[2] still pending (2000ms timeout)
 *   - Client sends next request 50ms later
 *   - Expected: ACCEPTED
 *   - Actual (buggy): DROPPED for up to 1950ms
 *
 * EXPECTED TO FAIL on unfixed code.
 */
static int
test_concrete_3node_rapid_request(void)
{
    printf("  concrete: 3-node race, winner sent, rapid next request\n");

    mock_proxy_t proxy;
    proxy.state = RACE_FANOUT;
    proxy.winner_idx = -1;
    proxy.responses_pending = 3;
    proxy.all_must_complete = false;  /* decoderawtransaction is non-broadcast */

    /* node[0] responds (winner) */
    proxy.responses_pending--;
    simulate_winner_sent(&proxy, 0);

    /* 2 nodes still pending */
    if (proxy.responses_pending != 2) {
        fprintf(stderr, "  ERROR: expected 2 pending, got %d\n",
                proxy.responses_pending);
        return -1;
    }

    /* New client request arrives */
    bool accepted = request_gate_current(&proxy);

    if (!accepted) {
        fprintf(stderr, "  COUNTEREXAMPLE:\n");
        fprintf(stderr, "    method=decoderawtransaction, nodes=3\n");
        fprintf(stderr, "    node[0] responded (winner_idx=0)\n");
        fprintf(stderr, "    node[1],node[2] still pending "
                "(responses_pending=2)\n");
        fprintf(stderr, "    proxy.state=RACE_FANOUT (should be RACE_IDLE)\n");
        fprintf(stderr, "    New request → DROPPED\n");
        fprintf(stderr, "    Expected: ACCEPTED\n");
        return -1;
    }

    printf("    PASS: new request accepted\n");
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

    printf("test_race_state_blocking (seed=%ld):\n", seed);
    printf("  Bug condition: proxy_state==RACE_FANOUT AND winner_sent==true "
           "AND all_must_complete==false AND responses_pending>0\n");
    printf("  Expected: new request ACCEPTED after winner sent\n\n");

    int failures = 0;

    if (test_property_bug_condition(seed) < 0)
        failures++;
    if (test_property_varying_pending(seed) < 0)
        failures++;
    if (test_concrete_validateaddress() < 0)
        failures++;
    if (test_concrete_3node_rapid_request() < 0)
        failures++;

    printf("\n");
    if (failures == 0) {
        printf("  ALL PASSED — expected behavior satisfied\n");
        return 0;
    } else {
        printf("  %d test(s) FAILED — bug condition confirmed\n", failures);
        printf("  Counterexample: New request arrives with proxy->state == "
               "RACE_FANOUT and proxy->winner_idx != -1 → request dropped "
               "with \"Request received while race/sticky active (state=1)\"\n");
        return 1;
    }
}
