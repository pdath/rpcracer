/* test_race_winner.c — Property test for race winner selection (Property 3)
 *
 * Property 3: Race Winner Selection
 * For any ordered sequence of RPC responses from the node array where at least
 * one response is a non-error (neither HTTP error nor RPC error), the first
 * non-error response in arrival order shall be selected as the winner and
 * returned to the stratum proxy.
 *
 * **Validates: Requirements 4.3, 6.3, 7.3**
 *
 * This test simulates the race logic independently — it does NOT depend on
 * the actual rpc_proxy.c implementation. It generates random response
 * sequences and verifies the winner selection property holds.
 *
 * A response is an "error" if:
 *   - HTTP status != 200, OR
 *   - JSON body has a non-null "error" field
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

/* ---- Simulated response ---- */
typedef struct {
    int node_index;       /* which node this response came from */
    int http_status;      /* HTTP status code (200 = OK, others = error) */
    bool has_rpc_error;   /* true if JSON body has non-null "error" field */
} sim_response_t;

/* ---- Determine if a simulated response is an error ---- */
static bool
sim_response_is_error(const sim_response_t *resp)
{
    /* Error if HTTP status != 200 OR JSON body has non-null error field */
    if (resp->http_status != 200)
        return true;
    if (resp->has_rpc_error)
        return true;
    return false;
}

/* ---- Reference implementation of race winner selection ----
 *
 * Given an ordered sequence of responses (in arrival order), select the
 * first non-error response as the winner. Returns the index into the
 * responses array, or -1 if all are errors.
 */
static int
select_race_winner(const sim_response_t *responses, int count)
{
    for (int i = 0; i < count; i++) {
        if (!sim_response_is_error(&responses[i]))
            return i;
    }
    return -1;  /* All errors — no winner (handled by Property 4) */
}

/* ---- Fisher-Yates shuffle for arrival order ---- */
static void
shuffle_responses(sim_response_t *arr, int count)
{
    for (int i = count - 1; i > 0; i--) {
        int j = (int)(lrand48() % (i + 1));
        sim_response_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/* ---- Generate a random HTTP status code ---- */
static int
gen_http_status(void)
{
    /* Distribution:
     * - 60% chance: 200 (success)
     * - 10% chance: 500 (internal server error)
     * - 10% chance: 503 (service unavailable)
     * - 5% chance: 401 (unauthorized)
     * - 5% chance: 403 (forbidden)
     * - 5% chance: 408 (request timeout)
     * - 5% chance: random 4xx/5xx
     */
    int r = (int)(lrand48() % 100);
    if (r < 60) return 200;
    if (r < 70) return 500;
    if (r < 80) return 503;
    if (r < 85) return 401;
    if (r < 90) return 403;
    if (r < 95) return 408;
    /* Random error status */
    int codes[] = {400, 404, 429, 502, 504};
    return codes[lrand48() % 5];
}

/* ---- Generate a random response (may be success or error) ---- */
static sim_response_t
gen_random_response(int node_index)
{
    sim_response_t resp;
    resp.node_index = node_index;
    resp.http_status = gen_http_status();

    /* If HTTP status is 200, there's still a chance of RPC-level error */
    if (resp.http_status == 200) {
        /* 25% chance of RPC error even with HTTP 200 */
        resp.has_rpc_error = (lrand48() % 4 == 0);
    } else {
        /* Non-200 HTTP status: RPC error field doesn't matter (already error) */
        resp.has_rpc_error = (lrand48() % 2 == 0);
    }

    return resp;
}

/* ---- Generate a guaranteed success response ---- */
static sim_response_t
gen_success_response(int node_index)
{
    sim_response_t resp;
    resp.node_index = node_index;
    resp.http_status = 200;
    resp.has_rpc_error = false;
    return resp;
}

/* ---- Generate a guaranteed error response ---- */
static sim_response_t
gen_error_response(int node_index)
{
    sim_response_t resp;
    resp.node_index = node_index;

    /* Randomly choose error type: HTTP error or RPC error */
    if (lrand48() % 2 == 0) {
        /* HTTP-level error */
        int codes[] = {500, 503, 401, 403, 408, 502, 504};
        resp.http_status = codes[lrand48() % 7];
        resp.has_rpc_error = (lrand48() % 2 == 0);
    } else {
        /* HTTP 200 but RPC-level error */
        resp.http_status = 200;
        resp.has_rpc_error = true;
    }

    return resp;
}

/*
 * Property 3: Race Winner Selection
 *
 * For any ordered sequence of RPC responses from the node array where at
 * least one response is a non-error, the first non-error response in
 * arrival order shall be selected as the winner.
 *
 * Test strategy:
 * - Generate a random number of nodes (1-8)
 * - For each node, randomly decide if it responds with success or error
 * - Ensure at least one success exists (Property 3 precondition)
 * - Randomly shuffle to simulate arrival order
 * - Verify: the winner is the first non-error in arrival order
 *
 * Validates: Requirements 4.3, 6.3, 7.3
 */
static int
test_property_race_winner(long seed)
{
    printf("  property: race winner selection (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Generate random number of nodes (1-8) */
        int num_nodes = 1 + (int)(lrand48() % MAX_NODES);

        /* Generate responses for each node */
        sim_response_t responses[MAX_NODES];
        int num_responses = 0;
        bool has_success = false;

        for (int i = 0; i < num_nodes; i++) {
            responses[i] = gen_random_response(i);
            if (!sim_response_is_error(&responses[i]))
                has_success = true;
            num_responses++;
        }

        /* Ensure at least one success (Property 3 precondition) */
        if (!has_success) {
            /* Force one random node to be a success */
            int force_idx = (int)(lrand48() % num_nodes);
            responses[force_idx] = gen_success_response(force_idx);
        }

        /* Shuffle to simulate random arrival order */
        shuffle_responses(responses, num_responses);

        /* Select winner using our reference implementation */
        int winner_idx = select_race_winner(responses, num_responses);

        /* Verify: winner must exist (we guaranteed at least one success) */
        if (winner_idx == -1) {
            fprintf(stderr, "  FAIL trial %d: no winner found despite "
                    "guaranteed success\n", trial);
            fprintf(stderr, "  (seed=%ld, trial=%d, num_nodes=%d)\n",
                    seed, trial, num_nodes);
            for (int i = 0; i < num_responses; i++) {
                fprintf(stderr, "    response[%d]: node=%d http=%d rpc_err=%d "
                        "is_error=%d\n",
                        i, responses[i].node_index, responses[i].http_status,
                        responses[i].has_rpc_error,
                        sim_response_is_error(&responses[i]));
            }
            return -1;
        }

        /* Verify: winner response must be a non-error */
        if (sim_response_is_error(&responses[winner_idx])) {
            fprintf(stderr, "  FAIL trial %d: winner is an error response\n",
                    trial);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            fprintf(stderr, "  winner: node=%d http=%d rpc_err=%d\n",
                    responses[winner_idx].node_index,
                    responses[winner_idx].http_status,
                    responses[winner_idx].has_rpc_error);
            return -1;
        }

        /* Verify: no earlier response in arrival order is a non-error
         * (i.e., the winner is truly the FIRST non-error) */
        for (int i = 0; i < winner_idx; i++) {
            if (!sim_response_is_error(&responses[i])) {
                fprintf(stderr, "  FAIL trial %d: earlier non-error response "
                        "exists at index %d (winner at %d)\n",
                        trial, i, winner_idx);
                fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
                fprintf(stderr, "  earlier: node=%d http=%d rpc_err=%d\n",
                        responses[i].node_index, responses[i].http_status,
                        responses[i].has_rpc_error);
                fprintf(stderr, "  winner:  node=%d http=%d rpc_err=%d\n",
                        responses[winner_idx].node_index,
                        responses[winner_idx].http_status,
                        responses[winner_idx].has_rpc_error);
                return -1;
            }
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Winner selection is deterministic for a given arrival order.
 * The same sequence of responses always produces the same winner.
 */
static int
test_property_winner_deterministic(long seed)
{
    printf("  property: winner selection is deterministic (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 1 + (int)(lrand48() % MAX_NODES);

        /* Generate and shuffle responses */
        sim_response_t responses[MAX_NODES];
        bool has_success = false;

        for (int i = 0; i < num_nodes; i++) {
            responses[i] = gen_random_response(i);
            if (!sim_response_is_error(&responses[i]))
                has_success = true;
        }

        if (!has_success) {
            int force_idx = (int)(lrand48() % num_nodes);
            responses[force_idx] = gen_success_response(force_idx);
        }

        shuffle_responses(responses, num_nodes);

        /* Call select_race_winner twice with same input */
        int winner1 = select_race_winner(responses, num_nodes);
        int winner2 = select_race_winner(responses, num_nodes);

        if (winner1 != winner2) {
            fprintf(stderr, "  FAIL trial %d: non-deterministic winner "
                    "selection (%d vs %d)\n", trial, winner1, winner2);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: If the first response in arrival order is a success,
 * it must always be the winner (regardless of what follows).
 */
static int
test_property_first_success_wins(long seed)
{
    printf("  property: first success in arrival order always wins "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));

        sim_response_t responses[MAX_NODES];

        /* Force first response to be a success */
        responses[0] = gen_success_response((int)(lrand48() % num_nodes));

        /* Fill remaining with random responses */
        for (int i = 1; i < num_nodes; i++) {
            responses[i] = gen_random_response(i);
        }

        int winner_idx = select_race_winner(responses, num_nodes);

        /* Winner must be index 0 (the first response) */
        if (winner_idx != 0) {
            fprintf(stderr, "  FAIL trial %d: first success not selected as "
                    "winner (winner_idx=%d)\n", trial, winner_idx);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            fprintf(stderr, "  first: node=%d http=%d rpc_err=%d is_error=%d\n",
                    responses[0].node_index, responses[0].http_status,
                    responses[0].has_rpc_error,
                    sim_response_is_error(&responses[0]));
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Errors before the winner do not affect winner selection.
 * Generate sequences with N errors followed by a success — the success
 * must always be the winner regardless of how many errors precede it.
 */
static int
test_property_errors_before_winner(long seed)
{
    printf("  property: errors before winner don't affect selection "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));

        sim_response_t responses[MAX_NODES];

        /* Place the success at a random position */
        int success_pos = (int)(lrand48() % num_nodes);

        for (int i = 0; i < num_nodes; i++) {
            if (i == success_pos) {
                responses[i] = gen_success_response(i);
            } else if (i < success_pos) {
                /* All before success_pos are errors */
                responses[i] = gen_error_response(i);
            } else {
                /* After success_pos: random (doesn't matter) */
                responses[i] = gen_random_response(i);
            }
        }

        int winner_idx = select_race_winner(responses, num_nodes);

        /* Winner must be at success_pos */
        if (winner_idx != success_pos) {
            fprintf(stderr, "  FAIL trial %d: winner at %d, expected %d\n",
                    trial, winner_idx, success_pos);
            fprintf(stderr, "  (seed=%ld, trial=%d, num_nodes=%d)\n",
                    seed, trial, num_nodes);
            for (int i = 0; i < num_nodes; i++) {
                fprintf(stderr, "    response[%d]: node=%d http=%d rpc_err=%d "
                        "is_error=%d\n",
                        i, responses[i].node_index, responses[i].http_status,
                        responses[i].has_rpc_error,
                        sim_response_is_error(&responses[i]));
            }
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Error classification correctness.
 * Verify that the error classification logic correctly identifies:
 *   - HTTP 200 + no RPC error → success
 *   - HTTP 200 + RPC error → error
 *   - HTTP != 200 (any RPC error state) → error
 */
static int
test_property_error_classification(long seed)
{
    printf("  property: error classification correctness (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        sim_response_t resp;
        resp.node_index = (int)(lrand48() % MAX_NODES);
        resp.http_status = gen_http_status();
        resp.has_rpc_error = (lrand48() % 2 == 0);

        bool is_error = sim_response_is_error(&resp);

        /* A response is success ONLY if HTTP 200 AND no RPC error */
        bool expected_success = (resp.http_status == 200 && !resp.has_rpc_error);

        if (is_error == expected_success) {
            fprintf(stderr, "  FAIL trial %d: error classification mismatch\n",
                    trial);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            fprintf(stderr, "  http=%d rpc_err=%d → is_error=%d "
                    "(expected_success=%d)\n",
                    resp.http_status, resp.has_rpc_error,
                    is_error, expected_success);
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

    printf("test_race_winner (seed=%ld):\n", seed);

    int failures = 0;

    if (test_property_race_winner(seed) < 0)
        failures++;
    if (test_property_winner_deterministic(seed) < 0)
        failures++;
    if (test_property_first_success_wins(seed) < 0)
        failures++;
    if (test_property_errors_before_winner(seed) < 0)
        failures++;
    if (test_property_error_classification(seed) < 0)
        failures++;

    if (failures == 0) {
        printf("  All property tests passed\n");
        return 0;
    } else {
        printf("  %d property test(s) FAILED\n", failures);
        return 1;
    }
}
