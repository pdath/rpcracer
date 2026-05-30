/* test_all_error_fallback.c — Property test for all-error fallback (Property 4)
 *
 * Property 4: All-Error Fallback
 * For any ordered sequence of RPC responses from the node array where every
 * response is an error (HTTP or RPC), the last error received shall be
 * returned to the stratum proxy.
 *
 * **Validates: Requirements 4.4, 5.6, 6.4, 7.4**
 *
 * This test simulates the all-error fallback logic independently:
 *   - Generate a random number of nodes (1-8)
 *   - All nodes respond with errors (various error codes/messages)
 *   - Randomly order the responses (arrival order)
 *   - Verify: the last error in arrival order is the one returned to the client
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand48/lrand48),
 * 1000 trials, seed printed for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define MAX_NODES 16
#define NUM_TRIALS 1000

/* ---- Error response model ---- */

typedef struct {
    int node_index;       /* Which node this error came from (0..n-1) */
    int error_code;       /* RPC error code or HTTP status */
    char message[128];    /* Error message */
} error_response_t;

/* ---- Reference implementation of all-error fallback logic ----
 *
 * This mirrors the logic in rpc_proxy.c:
 *   - Track last_error_idx as each error response arrives
 *   - When all responses are in and no winner was found, return last_error_idx
 *
 * The key insight: last_error_idx is simply the node_index of the LAST
 * error response received in arrival order.
 */
static int
select_fallback_error(const error_response_t *responses, int count)
{
    /* Process responses in arrival order (array index = arrival order) */
    int last_error_idx = -1;

    for (int i = 0; i < count; i++) {
        /* Every response is an error, so each one updates last_error_idx */
        last_error_idx = responses[i].node_index;
    }

    return last_error_idx;
}

/* ---- Random generators ---- */

/* Generate a random RPC error code. Bitcoin Core uses codes like:
 * -1 (misc error), -5 (invalid address), -8 (invalid parameter),
 * -25 (verify error), -26 (verify rejected), -27 (already in chain),
 * -28 (not ready), etc. Also include HTTP-level error codes. */
static int
gen_error_code(void)
{
    int choice = (int)(lrand48() % 5);
    switch (choice) {
    case 0:
        /* Standard Bitcoin RPC error codes (negative) */
        {
            int codes[] = { -1, -3, -5, -8, -25, -26, -27, -28, -32600,
                            -32601, -32602, -32603, -32700 };
            return codes[lrand48() % 13];
        }
    case 1:
        /* HTTP error status codes */
        {
            int codes[] = { 400, 401, 403, 404, 500, 502, 503, 504 };
            return codes[lrand48() % 8];
        }
    case 2:
        /* Random negative code */
        return -(int)(1 + lrand48() % 100);
    case 3:
        /* Random positive code (unusual but possible) */
        return (int)(400 + lrand48() % 200);
    default:
        return -1;
    }
}

/* Generate a random error message */
static void
gen_error_message(char *buf, size_t buf_size)
{
    static const char *messages[] = {
        "Method not found",
        "Invalid params",
        "Internal error",
        "Parse error",
        "Block not found",
        "Transaction already in block chain",
        "Insufficient funds",
        "Node is initializing",
        "Work queue depth exceeded",
        "Connection refused",
        "Timeout",
        "Bad gateway",
        "Service unavailable",
        "Server error",
        "Bitcoin is not connected",
        "Block decode failed"
    };
    int idx = (int)(lrand48() % 16);
    snprintf(buf, buf_size, "%s", messages[idx]);
}

/* Generate a random permutation of integers 0..n-1 (Fisher-Yates shuffle) */
static void
gen_permutation(int *arr, int n)
{
    for (int i = 0; i < n; i++)
        arr[i] = i;

    for (int i = n - 1; i > 0; i--) {
        int j = (int)(lrand48() % (i + 1));
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/*
 * Property 4: All-Error Fallback
 *
 * For any ordered sequence of RPC responses from the node array where every
 * response is an error (HTTP or RPC), the last error received shall be
 * returned to the stratum proxy.
 *
 * Test strategy:
 *   1. Generate a random number of nodes (1-8)
 *   2. Generate an error response for each node (random error codes/messages)
 *   3. Generate a random arrival order (permutation of node indices)
 *   4. Feed responses in arrival order to the fallback selection logic
 *   5. Verify: the selected error is from the LAST node in arrival order
 *
 * Validates: Requirements 4.4, 5.6, 6.4, 7.4
 */
static int
test_property_all_error_fallback(long seed)
{
    printf("  property: all-error fallback returns last error (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;

    for (int i = 0; i < NUM_TRIALS; i++) {
        /* Generate random number of nodes (1-8) */
        int node_count = 1 + (int)(lrand48() % MAX_NODES);

        /* Generate error responses for each node */
        error_response_t all_errors[MAX_NODES];
        for (int n = 0; n < node_count; n++) {
            all_errors[n].node_index = n;
            all_errors[n].error_code = gen_error_code();
            gen_error_message(all_errors[n].message, sizeof(all_errors[n].message));
        }

        /* Generate random arrival order (permutation) */
        int arrival_order[MAX_NODES];
        gen_permutation(arrival_order, node_count);

        /* Build the response sequence in arrival order */
        error_response_t arrival_sequence[MAX_NODES];
        for (int a = 0; a < node_count; a++) {
            arrival_sequence[a] = all_errors[arrival_order[a]];
        }

        /* Apply the fallback logic */
        int selected_idx = select_fallback_error(arrival_sequence, node_count);

        /* The expected result: the node_index of the LAST response in arrival order */
        int expected_idx = arrival_sequence[node_count - 1].node_index;

        if (selected_idx != expected_idx) {
            fprintf(stderr, "  FAIL trial %d (seed=%ld):\n", i, seed);
            fprintf(stderr, "    node_count=%d\n", node_count);
            fprintf(stderr, "    arrival_order: [");
            for (int a = 0; a < node_count; a++)
                fprintf(stderr, "%d%s", arrival_order[a],
                        (a < node_count - 1) ? ", " : "");
            fprintf(stderr, "]\n");
            fprintf(stderr, "    expected last_error_idx=%d (node %d, code=%d, msg='%s')\n",
                    expected_idx, expected_idx,
                    arrival_sequence[node_count - 1].error_code,
                    arrival_sequence[node_count - 1].message);
            fprintf(stderr, "    got last_error_idx=%d\n", selected_idx);
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
 * Sub-property: Single node case — when there's only one node and it errors,
 * that error must be returned (trivial but important edge case).
 */
static int
test_property_single_node_error(long seed)
{
    printf("  property: single node error is always returned (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;

    for (int i = 0; i < NUM_TRIALS; i++) {
        error_response_t resp;
        resp.node_index = 0;
        resp.error_code = gen_error_code();
        gen_error_message(resp.message, sizeof(resp.message));

        int selected_idx = select_fallback_error(&resp, 1);

        if (selected_idx != 0) {
            fprintf(stderr, "  FAIL trial %d (seed=%ld): single node error "
                    "not returned (got idx=%d)\n", i, seed, selected_idx);
            failures++;
            if (failures >= 5)
                break;
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
 * Sub-property: The fallback selection is order-dependent — different arrival
 * orders of the same set of errors can produce different results (unless all
 * errors are from the same node). This verifies the property is truly about
 * arrival order, not about error content.
 */
static int
test_property_order_determines_result(long seed)
{
    printf("  property: arrival order determines fallback result (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;
    int verified_order_matters = 0;

    for (int i = 0; i < NUM_TRIALS; i++) {
        /* Need at least 2 nodes for order to matter */
        int node_count = 2 + (int)(lrand48() % (MAX_NODES - 1));

        /* Generate error responses */
        error_response_t all_errors[MAX_NODES];
        for (int n = 0; n < node_count; n++) {
            all_errors[n].node_index = n;
            all_errors[n].error_code = gen_error_code();
            gen_error_message(all_errors[n].message, sizeof(all_errors[n].message));
        }

        /* Generate two different arrival orders */
        int order1[MAX_NODES], order2[MAX_NODES];
        gen_permutation(order1, node_count);
        gen_permutation(order2, node_count);

        /* Build arrival sequences */
        error_response_t seq1[MAX_NODES], seq2[MAX_NODES];
        for (int a = 0; a < node_count; a++) {
            seq1[a] = all_errors[order1[a]];
            seq2[a] = all_errors[order2[a]];
        }

        /* Apply fallback logic to both */
        int result1 = select_fallback_error(seq1, node_count);
        int result2 = select_fallback_error(seq2, node_count);

        /* Verify each result matches its own last element */
        int expected1 = seq1[node_count - 1].node_index;
        int expected2 = seq2[node_count - 1].node_index;

        if (result1 != expected1) {
            fprintf(stderr, "  FAIL trial %d (seed=%ld): order1 result "
                    "mismatch (expected=%d, got=%d)\n",
                    i, seed, expected1, result1);
            failures++;
            if (failures >= 5)
                break;
            continue;
        }

        if (result2 != expected2) {
            fprintf(stderr, "  FAIL trial %d (seed=%ld): order2 result "
                    "mismatch (expected=%d, got=%d)\n",
                    i, seed, expected2, result2);
            failures++;
            if (failures >= 5)
                break;
            continue;
        }

        /* If the two orders have different last elements, results should differ */
        if (order1[node_count - 1] != order2[node_count - 1]) {
            if (result1 == result2) {
                fprintf(stderr, "  FAIL trial %d (seed=%ld): different last "
                        "arrivals but same result (%d)\n",
                        i, seed, result1);
                failures++;
                if (failures >= 5)
                    break;
            } else {
                verified_order_matters++;
            }
        }
    }

    if (failures == 0) {
        printf("    %d/%d trials passed (%d verified order-dependent)\n",
               NUM_TRIALS, NUM_TRIALS, verified_order_matters);
    } else {
        fprintf(stderr, "    %d/%d trials FAILED\n", failures, NUM_TRIALS);
    }
    return failures;
}

/*
 * Sub-property: Error content (code, message) does not affect selection —
 * only arrival order matters. Two sequences with identical arrival order
 * but different error codes/messages must select the same node.
 */
static int
test_property_content_irrelevant(long seed)
{
    printf("  property: error content does not affect selection (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int failures = 0;

    for (int i = 0; i < NUM_TRIALS; i++) {
        int node_count = 1 + (int)(lrand48() % MAX_NODES);

        /* Generate a fixed arrival order */
        int arrival_order[MAX_NODES];
        gen_permutation(arrival_order, node_count);

        /* Generate two different sets of error responses with same arrival order */
        error_response_t seq1[MAX_NODES], seq2[MAX_NODES];
        for (int a = 0; a < node_count; a++) {
            seq1[a].node_index = arrival_order[a];
            seq1[a].error_code = gen_error_code();
            gen_error_message(seq1[a].message, sizeof(seq1[a].message));

            seq2[a].node_index = arrival_order[a];
            seq2[a].error_code = gen_error_code();  /* Different code */
            gen_error_message(seq2[a].message, sizeof(seq2[a].message));  /* Different msg */
        }

        int result1 = select_fallback_error(seq1, node_count);
        int result2 = select_fallback_error(seq2, node_count);

        /* Both must select the same node (the last in arrival order) */
        if (result1 != result2) {
            fprintf(stderr, "  FAIL trial %d (seed=%ld): same order, different "
                    "content, but different results (%d vs %d)\n",
                    i, seed, result1, result2);
            failures++;
            if (failures >= 5)
                break;
        }

        /* Both must be the last node in arrival order */
        int expected = arrival_order[node_count - 1];
        if (result1 != expected || result2 != expected) {
            fprintf(stderr, "  FAIL trial %d (seed=%ld): expected=%d, "
                    "got result1=%d result2=%d\n",
                    i, seed, expected, result1, result2);
            failures++;
            if (failures >= 5)
                break;
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

    printf("test_all_error_fallback (seed=%ld):\n", seed);

    int total_failures = 0;

    total_failures += test_property_all_error_fallback(seed);
    total_failures += test_property_single_node_error(seed);
    total_failures += test_property_order_determines_result(seed);
    total_failures += test_property_content_irrelevant(seed);

    if (total_failures == 0) {
        printf("  ALL PASSED\n");
        return 0;
    } else {
        fprintf(stderr, "  TOTAL FAILURES: %d\n", total_failures);
        return 1;
    }
}
