/* test_gbt_height_match.c — Property test for GBT height match selection (Property 5)
 *
 * Property 5: GBT Height Match Selection
 * For any set of valid getblocktemplate() responses received during a race
 * after a block notification, if at least one response has a height equal to
 * `last_block_height + 1`, the first such response in arrival order shall win
 * and its node shall become the sticky node.
 *
 * **Validates: Requirements 5.2**
 *
 * This test simulates the GBT height match logic independently — it does NOT
 * call into rpc_proxy.c. It generates random response sets with various
 * heights and verifies the selection property holds.
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

/* ---- Simulated GBT response ---- */
typedef struct {
    int node_index;       /* which node this response came from */
    int64_t height;       /* block height in the GBT response */
} gbt_response_t;

/* ---- Reference implementation of GBT height match selection ----
 *
 * This mirrors the logic in rpc_proxy.c on_upstream_response() for GBT races:
 *   - Given last_block_height, expected height is last_block_height + 1
 *   - Process responses in arrival order
 *   - First response with height == expected wins immediately
 *   - Its node becomes the sticky node
 *
 * Returns the index into the responses array of the winner, or -1 if no
 * response matches the expected height.
 */
static int
select_gbt_height_match(const gbt_response_t *responses, int count,
                        int64_t last_block_height, int *sticky_node_out)
{
    int64_t expected_height = last_block_height + 1;

    for (int i = 0; i < count; i++) {
        if (responses[i].height == expected_height) {
            /* First match wins — node becomes sticky */
            if (sticky_node_out)
                *sticky_node_out = responses[i].node_index;
            return i;
        }
    }

    return -1;  /* No match found (handled by Property 6 fallback) */
}

/* ---- Fisher-Yates shuffle for arrival order ---- */
static void
shuffle_responses(gbt_response_t *arr, int count)
{
    for (int i = count - 1; i > 0; i--) {
        int j = (int)(lrand48() % (i + 1));
        gbt_response_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/* ---- Generate a random block height in realistic range ---- */
static int64_t
gen_block_height(void)
{
    /* Range: 800000 - 900000 (realistic Bitcoin block heights) */
    return 800000 + (int64_t)(lrand48() % 100000);
}

/* ---- Generate a random height that does NOT match expected ---- */
static int64_t
gen_non_matching_height(int64_t expected)
{
    int64_t h;
    int choice = (int)(lrand48() % 4);
    switch (choice) {
    case 0:
        /* One below expected (same as last_block_height) */
        h = expected - 1;
        break;
    case 1:
        /* Two below expected */
        h = expected - 2;
        break;
    case 2:
        /* One above expected */
        h = expected + 1;
        break;
    default:
        /* Random height in range, ensure it's not expected */
        h = 800000 + (int64_t)(lrand48() % 100000);
        if (h == expected)
            h = expected + 2;
        break;
    }
    return h;
}

/*
 * Property 5: GBT Height Match Selection
 *
 * For any set of valid GBT responses where at least one has height ==
 * last_block_height + 1, the first such response in arrival order wins
 * and its node becomes sticky.
 *
 * Test strategy:
 * - Generate random last_block_height (800000-900000)
 * - Generate 1-8 node responses with random heights
 * - Ensure at least one matches last_block_height + 1
 * - Randomly order responses (arrival order)
 * - Verify: first response with height == expected wins
 * - Verify: winner's node becomes sticky
 *
 * Validates: Requirements 5.2
 */
static int
test_property_gbt_height_match(long seed)
{
    printf("  property: GBT height match selection (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Generate random last_block_height */
        int64_t last_block_height = gen_block_height();
        int64_t expected_height = last_block_height + 1;

        /* Generate random number of nodes (1-8) */
        int num_nodes = 1 + (int)(lrand48() % MAX_NODES);

        /* Generate responses with various heights */
        gbt_response_t responses[MAX_NODES];
        bool has_match = false;

        for (int i = 0; i < num_nodes; i++) {
            responses[i].node_index = i;

            /* 40% chance of matching expected height */
            if (lrand48() % 100 < 40) {
                responses[i].height = expected_height;
                has_match = true;
            } else {
                responses[i].height = gen_non_matching_height(expected_height);
            }
        }

        /* Ensure at least one matches (Property 5 precondition) */
        if (!has_match) {
            int force_idx = (int)(lrand48() % num_nodes);
            responses[force_idx].height = expected_height;
        }

        /* Shuffle to simulate random arrival order */
        shuffle_responses(responses, num_nodes);

        /* Select winner using reference implementation */
        int sticky_node = -1;
        int winner_idx = select_gbt_height_match(responses, num_nodes,
                                                  last_block_height,
                                                  &sticky_node);

        /* Verify: winner must exist (we guaranteed at least one match) */
        if (winner_idx == -1) {
            fprintf(stderr, "  FAIL trial %d: no winner found despite "
                    "guaranteed height match\n", trial);
            fprintf(stderr, "  (seed=%ld, trial=%d, last_block_height=%lld, "
                    "expected=%lld, num_nodes=%d)\n",
                    seed, trial, (long long)last_block_height,
                    (long long)expected_height, num_nodes);
            for (int i = 0; i < num_nodes; i++) {
                fprintf(stderr, "    response[%d]: node=%d height=%lld\n",
                        i, responses[i].node_index,
                        (long long)responses[i].height);
            }
            return -1;
        }

        /* Verify: winner's height must equal expected */
        if (responses[winner_idx].height != expected_height) {
            fprintf(stderr, "  FAIL trial %d: winner height %lld != "
                    "expected %lld\n", trial,
                    (long long)responses[winner_idx].height,
                    (long long)expected_height);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        /* Verify: no earlier response in arrival order has matching height
         * (i.e., the winner is truly the FIRST match) */
        for (int i = 0; i < winner_idx; i++) {
            if (responses[i].height == expected_height) {
                fprintf(stderr, "  FAIL trial %d: earlier matching response "
                        "exists at index %d (winner at %d)\n",
                        trial, i, winner_idx);
                fprintf(stderr, "  (seed=%ld, trial=%d, expected=%lld)\n",
                        seed, trial, (long long)expected_height);
                fprintf(stderr, "  earlier: node=%d height=%lld\n",
                        responses[i].node_index,
                        (long long)responses[i].height);
                fprintf(stderr, "  winner:  node=%d height=%lld\n",
                        responses[winner_idx].node_index,
                        (long long)responses[winner_idx].height);
                return -1;
            }
        }

        /* Verify: sticky node is set to the winner's node_index */
        if (sticky_node != responses[winner_idx].node_index) {
            fprintf(stderr, "  FAIL trial %d: sticky_node=%d but winner "
                    "node_index=%d\n", trial, sticky_node,
                    responses[winner_idx].node_index);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Height match wins immediately regardless of later responses.
 *
 * Once a height match is found, subsequent responses (even with matching
 * height) are discarded. The first match in arrival order is final.
 */
static int
test_property_first_match_wins_over_later(long seed)
{
    printf("  property: first height match wins over later matches "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = gen_block_height();
        int64_t expected_height = last_block_height + 1;

        /* Generate 2-8 nodes, ALL with matching height */
        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));

        gbt_response_t responses[MAX_NODES];
        for (int i = 0; i < num_nodes; i++) {
            responses[i].node_index = i;
            responses[i].height = expected_height;
        }

        /* Shuffle to randomize arrival order */
        shuffle_responses(responses, num_nodes);

        /* Select winner */
        int sticky_node = -1;
        int winner_idx = select_gbt_height_match(responses, num_nodes,
                                                  last_block_height,
                                                  &sticky_node);

        /* Winner must be index 0 (first in arrival order) since all match */
        if (winner_idx != 0) {
            fprintf(stderr, "  FAIL trial %d: winner at index %d, expected 0 "
                    "(all responses match expected height)\n",
                    trial, winner_idx);
            fprintf(stderr, "  (seed=%ld, trial=%d, expected=%lld)\n",
                    seed, trial, (long long)expected_height);
            for (int i = 0; i < num_nodes; i++) {
                fprintf(stderr, "    response[%d]: node=%d height=%lld\n",
                        i, responses[i].node_index,
                        (long long)responses[i].height);
            }
            return -1;
        }

        /* Sticky must be the first response's node */
        if (sticky_node != responses[0].node_index) {
            fprintf(stderr, "  FAIL trial %d: sticky=%d but first response "
                    "node=%d\n", trial, sticky_node,
                    responses[0].node_index);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Non-matching responses before the match don't affect selection.
 *
 * Generate sequences with N non-matching responses followed by a match.
 * The match must always be selected regardless of how many non-matches precede it.
 */
static int
test_property_non_matches_before_match(long seed)
{
    printf("  property: non-matching responses before match don't affect "
           "selection (seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = gen_block_height();
        int64_t expected_height = last_block_height + 1;

        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));

        gbt_response_t responses[MAX_NODES];

        /* Place the matching response at a random position */
        int match_pos = (int)(lrand48() % num_nodes);

        for (int i = 0; i < num_nodes; i++) {
            responses[i].node_index = i;
            if (i == match_pos) {
                responses[i].height = expected_height;
            } else if (i < match_pos) {
                /* All before match_pos are non-matching */
                responses[i].height = gen_non_matching_height(expected_height);
            } else {
                /* After match_pos: random (may or may not match) */
                if (lrand48() % 2 == 0)
                    responses[i].height = expected_height;
                else
                    responses[i].height = gen_non_matching_height(expected_height);
            }
        }

        /* Select winner */
        int sticky_node = -1;
        int winner_idx = select_gbt_height_match(responses, num_nodes,
                                                  last_block_height,
                                                  &sticky_node);

        /* Winner must be at match_pos (first match in arrival order) */
        if (winner_idx != match_pos) {
            fprintf(stderr, "  FAIL trial %d: winner at %d, expected %d\n",
                    trial, winner_idx, match_pos);
            fprintf(stderr, "  (seed=%ld, trial=%d, expected=%lld)\n",
                    seed, trial, (long long)expected_height);
            for (int i = 0; i < num_nodes; i++) {
                fprintf(stderr, "    response[%d]: node=%d height=%lld %s\n",
                        i, responses[i].node_index,
                        (long long)responses[i].height,
                        (responses[i].height == expected_height) ? "(MATCH)" : "");
            }
            return -1;
        }

        /* Sticky must be the winner's node */
        if (sticky_node != responses[match_pos].node_index) {
            fprintf(stderr, "  FAIL trial %d: sticky=%d but winner node=%d\n",
                    trial, sticky_node, responses[match_pos].node_index);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Sticky node assignment is correct.
 *
 * The sticky node must always be the node_index of the winning response,
 * not the array index of the response in the arrival order.
 */
static int
test_property_sticky_node_assignment(long seed)
{
    printf("  property: sticky node is winner's node_index "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = gen_block_height();
        int64_t expected_height = last_block_height + 1;

        int num_nodes = 2 + (int)(lrand48() % (MAX_NODES - 1));

        gbt_response_t responses[MAX_NODES];
        for (int i = 0; i < num_nodes; i++) {
            responses[i].node_index = i;
            /* Mix of matching and non-matching */
            if (lrand48() % 3 == 0)
                responses[i].height = expected_height;
            else
                responses[i].height = gen_non_matching_height(expected_height);
        }

        /* Ensure at least one match */
        bool has_match = false;
        for (int i = 0; i < num_nodes; i++) {
            if (responses[i].height == expected_height) {
                has_match = true;
                break;
            }
        }
        if (!has_match) {
            int force_idx = (int)(lrand48() % num_nodes);
            responses[force_idx].height = expected_height;
        }

        /* Shuffle to randomize arrival order (node_index != array position) */
        shuffle_responses(responses, num_nodes);

        /* Select winner */
        int sticky_node = -1;
        int winner_idx = select_gbt_height_match(responses, num_nodes,
                                                  last_block_height,
                                                  &sticky_node);

        if (winner_idx == -1) {
            fprintf(stderr, "  FAIL trial %d: no winner found\n", trial);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        /* The sticky node must be the node_index of the winning response,
         * NOT the position in the array */
        int expected_sticky = responses[winner_idx].node_index;
        if (sticky_node != expected_sticky) {
            fprintf(stderr, "  FAIL trial %d: sticky=%d but winner's "
                    "node_index=%d (winner at array pos %d)\n",
                    trial, sticky_node, expected_sticky, winner_idx);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            for (int i = 0; i < num_nodes; i++) {
                fprintf(stderr, "    response[%d]: node=%d height=%lld %s\n",
                        i, responses[i].node_index,
                        (long long)responses[i].height,
                        (i == winner_idx) ? "(WINNER)" : "");
            }
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

    printf("test_gbt_height_match (seed=%ld):\n", seed);

    int failures = 0;

    if (test_property_gbt_height_match(seed) < 0)
        failures++;
    if (test_property_first_match_wins_over_later(seed) < 0)
        failures++;
    if (test_property_non_matches_before_match(seed) < 0)
        failures++;
    if (test_property_sticky_node_assignment(seed) < 0)
        failures++;

    if (failures == 0) {
        printf("  All property tests passed\n");
        return 0;
    } else {
        printf("  %d property test(s) FAILED\n", failures);
        return 1;
    }
}
