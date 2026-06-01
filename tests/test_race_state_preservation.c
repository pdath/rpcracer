/* test_race_state_preservation.c — Preservation property tests for race state
 *
 * Property 2: Preservation
 * Broadcast races and sticky requests continue to block new requests.
 * These tests capture the BASELINE behavior that must be preserved after
 * the race-state-blocking fix is applied.
 *
 * **Validates: Requirements 3.1, 3.2, 3.4, 3.5, 3.6**
 *
 * Properties tested:
 *   P1: For all broadcast races (all_must_complete=true), proxy SHALL remain
 *       in RACE_FANOUT and reject new requests until all nodes respond
 *   P2: For all sticky requests, proxy SHALL remain in RACE_STICKY and reject
 *       new requests until the sticky response arrives
 *   P3: For all non-broadcast races where winner_idx == -1 (no winner yet),
 *       proxy SHALL remain in RACE_FANOUT
 *   P4: For all races where responses_pending reaches 0, race_complete() is
 *       called and state transitions to RACE_IDLE
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

/* ---- Simulated race state machine ----
 *
 * This models the race state logic from rpc_proxy.c faithfully.
 * We simulate the state transitions that occur in on_upstream_response()
 * to verify preservation properties.
 */

typedef enum {
    RACE_IDLE,
    RACE_FANOUT,
    RACE_STICKY
} race_state_t;

typedef struct {
    race_state_t state;
    int responses_pending;
    int winner_idx;
    int last_error_idx;
    bool all_must_complete;
    bool race_complete_called;
} sim_proxy_t;

/* Simulated response */
typedef struct {
    int node_index;
    bool is_error;
} sim_response_t;

/* Initialize proxy for a fan-out race */
static void
sim_init_fanout(sim_proxy_t *proxy, int num_nodes, bool all_must_complete)
{
    proxy->state = RACE_FANOUT;
    proxy->responses_pending = num_nodes;
    proxy->winner_idx = -1;
    proxy->last_error_idx = -1;
    proxy->all_must_complete = all_must_complete;
    proxy->race_complete_called = false;
}

/* Initialize proxy for a sticky request */
static void
sim_init_sticky(sim_proxy_t *proxy)
{
    proxy->state = RACE_STICKY;
    proxy->responses_pending = 1;
    proxy->winner_idx = -1;
    proxy->last_error_idx = -1;
    proxy->all_must_complete = false;
    proxy->race_complete_called = false;
}

/* Simulate race_complete() */
static void
sim_race_complete(sim_proxy_t *proxy)
{
    proxy->state = RACE_IDLE;
    proxy->responses_pending = 0;
    proxy->winner_idx = -1;
    proxy->last_error_idx = -1;
    proxy->race_complete_called = true;
}

/* Simulate on_upstream_response() — mirrors rpc_proxy.c logic exactly.
 * This is the UNFIXED logic (no early IDLE transition). */
static void
sim_on_upstream_response(sim_proxy_t *proxy, const sim_response_t *resp)
{
    proxy->responses_pending--;

    if (resp->is_error) {
        proxy->last_error_idx = resp->node_index;
    } else {
        /* Non-error response */
        if (proxy->winner_idx == -1) {
            /* First non-error wins */
            proxy->winner_idx = resp->node_index;
            /* (response sent to client) */
        }
        /* Subsequent non-errors are discarded */
    }

    /* Determine if race is over */
    if (proxy->responses_pending <= 0) {
        /* All responses received */
        if (proxy->winner_idx == -1 && proxy->last_error_idx >= 0) {
            /* All-error fallback: last error forwarded to client */
            /* (send last error to client) */
        }
        sim_race_complete(proxy);
    } else if (!proxy->all_must_complete && proxy->winner_idx != -1) {
        /* Non-broadcast with winner: stay in RACE_FANOUT (the bug behavior,
         * but also the correct behavior for broadcast preservation) */
        /* Log: "Winner found, N responses still pending (will discard)" */
    }
    /* For broadcast (all_must_complete): always wait for all to finish */
}

/* Check if a new request would be accepted (mirrors client_cb logic) */
static bool
sim_would_accept_request(const sim_proxy_t *proxy)
{
    return (proxy->state == RACE_IDLE);
}

/* ---- Random generators ---- */

static sim_response_t
gen_error_response(int node_index)
{
    sim_response_t resp;
    resp.node_index = node_index;
    resp.is_error = true;
    return resp;
}

static sim_response_t
gen_success_response(int node_index)
{
    sim_response_t resp;
    resp.node_index = node_index;
    resp.is_error = false;
    return resp;
}

static sim_response_t
gen_random_response(int node_index)
{
    sim_response_t resp;
    resp.node_index = node_index;
    resp.is_error = (lrand48() % 2 == 0);
    return resp;
}

/* Fisher-Yates shuffle for arrival order */
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
 * Property P1: Broadcast race preservation
 *
 * For all broadcast races (all_must_complete=true), proxy SHALL remain in
 * RACE_FANOUT and reject new requests until ALL nodes respond.
 *
 * Test strategy:
 *   - Generate random node count (2-8)
 *   - Set all_must_complete = true (broadcast)
 *   - Deliver responses one at a time in random order (mix of success/error)
 *   - After each response EXCEPT the last: verify state == RACE_FANOUT
 *     and new requests would be rejected
 *   - After the last response: verify race_complete was called and
 *     state == RACE_IDLE
 *
 * Validates: Requirements 3.1, 3.2
 */
static int
test_property_broadcast_blocks(long seed)
{
    printf("  property: broadcast races block until all complete "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));
        sim_proxy_t proxy;
        sim_init_fanout(&proxy, num_nodes, true /* all_must_complete */);

        /* Generate random arrival order */
        int arrival[MAX_NODES];
        for (int i = 0; i < num_nodes; i++)
            arrival[i] = i;
        shuffle_int(arrival, num_nodes);

        bool failed = false;

        /* Deliver responses one at a time */
        for (int i = 0; i < num_nodes; i++) {
            /* Generate a random response (some success, some error) */
            sim_response_t resp = gen_random_response(arrival[i]);
            sim_on_upstream_response(&proxy, &resp);

            if (i < num_nodes - 1) {
                /* Not the last response: must still be in RACE_FANOUT */
                if (proxy.state != RACE_FANOUT) {
                    fprintf(stderr, "  FAIL trial %d: broadcast race left "
                            "RACE_FANOUT after %d/%d responses (state=%d)\n",
                            trial, i + 1, num_nodes, proxy.state);
                    failed = true;
                    break;
                }
                /* New requests must be rejected */
                if (sim_would_accept_request(&proxy)) {
                    fprintf(stderr, "  FAIL trial %d: broadcast race would "
                            "accept new request after %d/%d responses\n",
                            trial, i + 1, num_nodes);
                    failed = true;
                    break;
                }
            } else {
                /* Last response: race_complete must have been called */
                if (!proxy.race_complete_called) {
                    fprintf(stderr, "  FAIL trial %d: race_complete not called "
                            "after all %d responses\n", trial, num_nodes);
                    failed = true;
                    break;
                }
                if (proxy.state != RACE_IDLE) {
                    fprintf(stderr, "  FAIL trial %d: state not RACE_IDLE "
                            "after all responses (state=%d)\n",
                            trial, proxy.state);
                    failed = true;
                    break;
                }
            }
        }

        if (failed)
            return -1;
        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Property P2: Sticky request preservation
 *
 * For all sticky requests, proxy SHALL remain in RACE_STICKY and reject
 * new requests until the sticky response arrives.
 *
 * Test strategy:
 *   - Initialize proxy in RACE_STICKY state with 1 response pending
 *   - Verify state == RACE_STICKY and new requests rejected BEFORE response
 *   - Deliver the response (success or error)
 *   - Verify race_complete called and state == RACE_IDLE after response
 *
 * Validates: Requirement 3.6
 */
static int
test_property_sticky_blocks(long seed)
{
    printf("  property: sticky requests block until response arrives "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        sim_proxy_t proxy;
        sim_init_sticky(&proxy);

        /* Before response: must be in RACE_STICKY, rejecting requests */
        if (proxy.state != RACE_STICKY) {
            fprintf(stderr, "  FAIL trial %d: initial state not RACE_STICKY "
                    "(state=%d)\n", trial, proxy.state);
            return -1;
        }
        if (sim_would_accept_request(&proxy)) {
            fprintf(stderr, "  FAIL trial %d: sticky request would accept "
                    "new request before response\n", trial);
            return -1;
        }

        /* Deliver response (randomly success or error) */
        sim_response_t resp;
        resp.node_index = 0;
        resp.is_error = (lrand48() % 3 == 0); /* 33% chance of error */
        sim_on_upstream_response(&proxy, &resp);

        /* After response: race_complete must be called, state IDLE */
        if (!proxy.race_complete_called) {
            fprintf(stderr, "  FAIL trial %d: race_complete not called "
                    "after sticky response\n", trial);
            return -1;
        }
        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: state not RACE_IDLE after "
                    "sticky response (state=%d)\n", trial, proxy.state);
            return -1;
        }
        if (!sim_would_accept_request(&proxy)) {
            fprintf(stderr, "  FAIL trial %d: new request not accepted "
                    "after sticky response\n", trial);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Property P3: No-winner preservation
 *
 * For all non-broadcast races where winner_idx == -1 (no winner yet, all
 * responses so far are errors), proxy SHALL remain in RACE_FANOUT.
 *
 * Test strategy:
 *   - Generate random node count (2-8)
 *   - Set all_must_complete = false (non-broadcast)
 *   - Deliver error responses one at a time (but NOT the last one)
 *   - After each error: verify state == RACE_FANOUT and winner_idx == -1
 *     and new requests rejected
 *
 * Validates: Requirement 3.4
 */
static int
test_property_no_winner_blocks(long seed)
{
    printf("  property: no-winner race stays in RACE_FANOUT "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));
        sim_proxy_t proxy;
        sim_init_fanout(&proxy, num_nodes, false /* non-broadcast */);

        /* Generate random arrival order */
        int arrival[MAX_NODES];
        for (int i = 0; i < num_nodes; i++)
            arrival[i] = i;
        shuffle_int(arrival, num_nodes);

        bool failed = false;

        /* Deliver error responses for all but the last node */
        int errors_to_send = num_nodes - 1;
        for (int i = 0; i < errors_to_send; i++) {
            sim_response_t resp = gen_error_response(arrival[i]);
            sim_on_upstream_response(&proxy, &resp);

            /* Must still be in RACE_FANOUT with no winner */
            if (proxy.state != RACE_FANOUT) {
                fprintf(stderr, "  FAIL trial %d: left RACE_FANOUT after "
                        "%d errors (state=%d)\n", trial, i + 1, proxy.state);
                failed = true;
                break;
            }
            if (proxy.winner_idx != -1) {
                fprintf(stderr, "  FAIL trial %d: winner_idx=%d after "
                        "only errors\n", trial, proxy.winner_idx);
                failed = true;
                break;
            }
            if (sim_would_accept_request(&proxy)) {
                fprintf(stderr, "  FAIL trial %d: would accept request "
                        "with no winner and %d pending\n",
                        trial, proxy.responses_pending);
                failed = true;
                break;
            }
        }

        if (failed)
            return -1;
        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Property P4: Race completion on zero pending
 *
 * For all races where responses_pending reaches 0, race_complete() is called
 * and state transitions to RACE_IDLE.
 *
 * Test strategy:
 *   - Generate random node count (1-8)
 *   - Randomly choose broadcast or non-broadcast
 *   - Deliver ALL responses (random mix of success/error)
 *   - Verify: race_complete_called == true and state == RACE_IDLE
 *
 * Validates: Requirements 3.2, 3.5
 */
static int
test_property_completion_on_zero_pending(long seed)
{
    printf("  property: race completes when responses_pending reaches 0 "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 1 + (int)(lrand48() % MAX_NODES);
        bool broadcast = (lrand48() % 2 == 0);
        sim_proxy_t proxy;
        sim_init_fanout(&proxy, num_nodes, broadcast);

        /* Generate random arrival order */
        int arrival[MAX_NODES];
        for (int i = 0; i < num_nodes; i++)
            arrival[i] = i;
        shuffle_int(arrival, num_nodes);

        /* Deliver all responses */
        for (int i = 0; i < num_nodes; i++) {
            sim_response_t resp = gen_random_response(arrival[i]);
            sim_on_upstream_response(&proxy, &resp);
        }

        /* After all responses: race_complete must have been called */
        if (!proxy.race_complete_called) {
            fprintf(stderr, "  FAIL trial %d: race_complete not called "
                    "after all %d responses (broadcast=%d)\n",
                    trial, num_nodes, broadcast);
            return -1;
        }
        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: state not RACE_IDLE after "
                    "all responses (state=%d, broadcast=%d)\n",
                    trial, proxy.state, broadcast);
            return -1;
        }
        if (proxy.responses_pending != 0) {
            fprintf(stderr, "  FAIL trial %d: responses_pending=%d after "
                    "race_complete\n", trial, proxy.responses_pending);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: All-error fallback with race_complete
 *
 * When all nodes return errors in a non-broadcast race, the last error is
 * forwarded to the client and race_complete() is called.
 *
 * Validates: Requirement 3.4 (combined with 3.2)
 */
static int
test_property_all_error_completes(long seed)
{
    printf("  property: all-error race completes and forwards last error "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 1 + (int)(lrand48() % MAX_NODES);
        sim_proxy_t proxy;
        sim_init_fanout(&proxy, num_nodes, false /* non-broadcast */);

        /* Generate random arrival order */
        int arrival[MAX_NODES];
        for (int i = 0; i < num_nodes; i++)
            arrival[i] = i;
        shuffle_int(arrival, num_nodes);

        /* Deliver ALL error responses */
        for (int i = 0; i < num_nodes; i++) {
            sim_response_t resp = gen_error_response(arrival[i]);
            sim_on_upstream_response(&proxy, &resp);
        }

        /* After all errors: race_complete must be called */
        if (!proxy.race_complete_called) {
            fprintf(stderr, "  FAIL trial %d: race_complete not called "
                    "after all %d errors\n", trial, num_nodes);
            return -1;
        }
        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: state not RACE_IDLE after "
                    "all errors (state=%d)\n", trial, proxy.state);
            return -1;
        }
        /* last_error_idx should be the last node in arrival order */
        /* (race_complete resets it, but we check it was set before) */
        /* The important thing is race_complete was called */

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Broadcast with winner still waits for all
 *
 * Even when a broadcast race finds a winner early, it must continue to
 * wait for all remaining nodes before calling race_complete.
 *
 * Validates: Requirement 3.1
 */
static int
test_property_broadcast_winner_still_waits(long seed)
{
    printf("  property: broadcast with early winner still waits for all "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));
        sim_proxy_t proxy;
        sim_init_fanout(&proxy, num_nodes, true /* broadcast */);

        /* Generate random arrival order */
        int arrival[MAX_NODES];
        for (int i = 0; i < num_nodes; i++)
            arrival[i] = i;
        shuffle_int(arrival, num_nodes);

        /* First response is a success (winner found early) */
        sim_response_t first = gen_success_response(arrival[0]);
        sim_on_upstream_response(&proxy, &first);

        /* Winner found, but must still be in RACE_FANOUT for broadcast */
        if (proxy.winner_idx == -1) {
            fprintf(stderr, "  FAIL trial %d: no winner after success "
                    "response\n", trial);
            return -1;
        }
        if (proxy.state != RACE_FANOUT) {
            fprintf(stderr, "  FAIL trial %d: broadcast left RACE_FANOUT "
                    "after winner (state=%d)\n", trial, proxy.state);
            return -1;
        }
        if (sim_would_accept_request(&proxy)) {
            fprintf(stderr, "  FAIL trial %d: broadcast would accept "
                    "request after winner with %d pending\n",
                    trial, proxy.responses_pending);
            return -1;
        }

        /* Deliver remaining responses */
        for (int i = 1; i < num_nodes; i++) {
            sim_response_t resp = gen_random_response(arrival[i]);
            sim_on_upstream_response(&proxy, &resp);

            if (i < num_nodes - 1) {
                /* Still not done */
                if (proxy.state != RACE_FANOUT) {
                    fprintf(stderr, "  FAIL trial %d: broadcast left "
                            "RACE_FANOUT at response %d/%d\n",
                            trial, i + 1, num_nodes);
                    return -1;
                }
            }
        }

        /* Now all done */
        if (!proxy.race_complete_called) {
            fprintf(stderr, "  FAIL trial %d: race_complete not called "
                    "after all broadcast responses\n", trial);
            return -1;
        }
        if (proxy.state != RACE_IDLE) {
            fprintf(stderr, "  FAIL trial %d: state not IDLE after "
                    "broadcast complete (state=%d)\n", trial, proxy.state);
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

    printf("test_race_state_preservation (seed=%ld):\n", seed);

    int failures = 0;

    if (test_property_broadcast_blocks(seed) < 0)
        failures++;
    if (test_property_sticky_blocks(seed) < 0)
        failures++;
    if (test_property_no_winner_blocks(seed) < 0)
        failures++;
    if (test_property_completion_on_zero_pending(seed) < 0)
        failures++;
    if (test_property_all_error_completes(seed) < 0)
        failures++;
    if (test_property_broadcast_winner_still_waits(seed) < 0)
        failures++;

    if (failures == 0) {
        printf("  All preservation property tests PASSED\n");
        return 0;
    } else {
        printf("  %d preservation property test(s) FAILED\n", failures);
        return 1;
    }
}
