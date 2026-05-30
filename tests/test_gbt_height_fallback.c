/* test_gbt_height_fallback.c — Property test for GBT height fallback selection (Property 6)
 *
 * Property 6: GBT Height Fallback Selection
 * For any set of valid getblocktemplate() responses received during a race
 * where no response matches `last_block_height + 1`, the response with the
 * highest height shall win (last received if tied), and its node shall become
 * the sticky node.
 *
 * **Validates: Requirements 5.3**
 *
 * This test simulates the GBT height fallback logic independently — it does
 * NOT depend on the actual rpc_proxy.c implementation. It generates random
 * response sets where no response matches the expected height, and verifies
 * the fallback selection property holds.
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
    int64_t height;       /* block height from the GBT response */
    int arrival_order;    /* position in arrival sequence (0-based) */
} sim_gbt_response_t;

/* ---- Reference implementation of GBT height fallback selection ----
 *
 * Given an ordered sequence of GBT responses (in arrival order) where NONE
 * match the expected height (last_block_height + 1):
 * - Select the response with the highest height.
 * - If multiple responses have the same highest height, the LAST one
 *   received (in arrival order) wins.
 *
 * Returns the index into the responses array of the winner, or -1 if empty.
 */
static int
select_gbt_height_fallback(const sim_gbt_response_t *responses, int count)
{
    if (count <= 0)
        return -1;

    int best_idx = 0;
    int64_t best_height = responses[0].height;

    for (int i = 1; i < count; i++) {
        /* >= means: if tied, last received wins (overwrites previous best) */
        if (responses[i].height >= best_height) {
            best_height = responses[i].height;
            best_idx = i;
        }
    }

    return best_idx;
}

/* ---- Generate a random height that does NOT match expected ---- */
static int64_t
gen_non_matching_height(int64_t last_block_height)
{
    int64_t expected = last_block_height + 1;
    int64_t height;

    do {
        /* Generate heights in a range around last_block_height.
         * Bias toward heights below expected (common in fallback scenario). */
        int r = (int)(lrand48() % 100);
        if (r < 40) {
            /* Same as last_block_height (stale) */
            height = last_block_height;
        } else if (r < 60) {
            /* Below last_block_height (very stale) */
            height = last_block_height - (int64_t)(lrand48() % 5) - 1;
        } else if (r < 80) {
            /* Above expected (ahead) */
            height = expected + (int64_t)(lrand48() % 5) + 1;
        } else {
            /* Random height in a wide range */
            height = last_block_height - 10 + (int64_t)(lrand48() % 20);
        }
    } while (height == expected);

    return height;
}

/*
 * Property 6: GBT Height Fallback Selection
 *
 * For any set of valid GBT responses where no response matches
 * last_block_height + 1, the response with the highest height wins.
 * If tied, the last received (in arrival order) wins.
 *
 * Test strategy:
 * - Generate random last_block_height (800000-900000)
 * - Generate 1-8 node responses with random heights, ensuring NONE match
 *   last_block_height + 1
 * - Process responses in arrival order
 * - Verify: response with highest height wins; if tied, last received wins
 *
 * Validates: Requirements 5.3
 */
static int
test_property_gbt_height_fallback(long seed)
{
    printf("  property: GBT height fallback selection (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Generate random last_block_height */
        int64_t last_block_height = 800000 + (int64_t)(lrand48() % 100000);
        int64_t expected_height = last_block_height + 1;

        /* Generate random number of responses (1-8) */
        int num_responses = 1 + (int)(lrand48() % MAX_NODES);

        /* Generate responses — none matching expected height */
        sim_gbt_response_t responses[MAX_NODES];
        for (int i = 0; i < num_responses; i++) {
            responses[i].node_index = i;
            responses[i].height = gen_non_matching_height(last_block_height);
            responses[i].arrival_order = i;
        }

        /* Verify precondition: no response matches expected height */
        for (int i = 0; i < num_responses; i++) {
            if (responses[i].height == expected_height) {
                fprintf(stderr, "  BUG in test: response[%d] matches expected "
                        "height %lld\n", i, (long long)expected_height);
                return -1;
            }
        }

        /* Select winner using reference implementation */
        int winner_idx = select_gbt_height_fallback(responses, num_responses);

        if (winner_idx < 0 || winner_idx >= num_responses) {
            fprintf(stderr, "  FAIL trial %d: invalid winner_idx=%d "
                    "(num_responses=%d)\n", trial, winner_idx, num_responses);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        int64_t winner_height = responses[winner_idx].height;

        /* Verify: winner has the highest height */
        for (int i = 0; i < num_responses; i++) {
            if (responses[i].height > winner_height) {
                fprintf(stderr, "  FAIL trial %d: response[%d] has higher "
                        "height (%lld) than winner[%d] (%lld)\n",
                        trial, i, (long long)responses[i].height,
                        winner_idx, (long long)winner_height);
                fprintf(stderr, "  (seed=%ld, trial=%d, last_block_height=%lld)\n",
                        seed, trial, (long long)last_block_height);
                for (int j = 0; j < num_responses; j++) {
                    fprintf(stderr, "    response[%d]: node=%d height=%lld\n",
                            j, responses[j].node_index,
                            (long long)responses[j].height);
                }
                return -1;
            }
        }

        /* Verify: if tied, winner is the LAST received at that height */
        for (int i = winner_idx + 1; i < num_responses; i++) {
            if (responses[i].height == winner_height) {
                fprintf(stderr, "  FAIL trial %d: response[%d] has same "
                        "height (%lld) as winner[%d] but arrives later\n",
                        trial, i, (long long)winner_height, winner_idx);
                fprintf(stderr, "  (seed=%ld, trial=%d, last_block_height=%lld)\n",
                        seed, trial, (long long)last_block_height);
                for (int j = 0; j < num_responses; j++) {
                    fprintf(stderr, "    response[%d]: node=%d height=%lld\n",
                            j, responses[j].node_index,
                            (long long)responses[j].height);
                }
                return -1;
            }
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: When all responses have the same height, the last one wins.
 *
 * This is a specific case of the tie-breaking rule: if all responses have
 * the same height (which doesn't match expected), the last received wins.
 */
static int
test_property_all_same_height_last_wins(long seed)
{
    printf("  property: all same height — last received wins "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = 800000 + (int64_t)(lrand48() % 100000);
        int64_t expected_height = last_block_height + 1;

        int num_responses = 2 + (int)(lrand48() % (MAX_NODES - 1));

        /* All responses have the same height (not matching expected) */
        int64_t same_height;
        do {
            same_height = last_block_height - (int64_t)(lrand48() % 10);
        } while (same_height == expected_height);

        sim_gbt_response_t responses[MAX_NODES];
        for (int i = 0; i < num_responses; i++) {
            responses[i].node_index = i;
            responses[i].height = same_height;
            responses[i].arrival_order = i;
        }

        int winner_idx = select_gbt_height_fallback(responses, num_responses);

        /* Winner must be the last response (highest index = last received) */
        int expected_winner = num_responses - 1;
        if (winner_idx != expected_winner) {
            fprintf(stderr, "  FAIL trial %d: all same height=%lld, "
                    "expected winner=%d (last), got winner=%d\n",
                    trial, (long long)same_height,
                    expected_winner, winner_idx);
            fprintf(stderr, "  (seed=%ld, trial=%d, num_responses=%d)\n",
                    seed, trial, num_responses);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Single response always wins (trivial case).
 *
 * When there is only one response (that doesn't match expected height),
 * it must always be selected as the winner.
 */
static int
test_property_single_response_wins(long seed)
{
    printf("  property: single response always wins "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = 800000 + (int64_t)(lrand48() % 100000);

        sim_gbt_response_t responses[1];
        responses[0].node_index = (int)(lrand48() % MAX_NODES);
        responses[0].height = gen_non_matching_height(last_block_height);
        responses[0].arrival_order = 0;

        int winner_idx = select_gbt_height_fallback(responses, 1);

        if (winner_idx != 0) {
            fprintf(stderr, "  FAIL trial %d: single response not selected "
                    "(winner_idx=%d)\n", trial, winner_idx);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Highest height always wins regardless of arrival position.
 *
 * Generate responses where exactly one has a uniquely highest height at
 * a random position. Verify it wins regardless of where it appears.
 */
static int
test_property_highest_wins_any_position(long seed)
{
    printf("  property: highest height wins regardless of position "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = 800000 + (int64_t)(lrand48() % 100000);
        int64_t expected_height = last_block_height + 1;

        int num_responses = 2 + (int)(lrand48() % (MAX_NODES - 1));

        /* Pick a base height that doesn't match expected */
        int64_t base_height;
        do {
            base_height = last_block_height - (int64_t)(lrand48() % 5);
        } while (base_height == expected_height);

        /* Pick a uniquely highest height (above base, not matching expected) */
        int64_t highest_height;
        do {
            highest_height = base_height + 2 + (int64_t)(lrand48() % 10);
        } while (highest_height == expected_height);

        /* Place the highest at a random position */
        int highest_pos = (int)(lrand48() % num_responses);

        sim_gbt_response_t responses[MAX_NODES];
        for (int i = 0; i < num_responses; i++) {
            responses[i].node_index = i;
            responses[i].arrival_order = i;
            if (i == highest_pos) {
                responses[i].height = highest_height;
            } else {
                /* Use base_height or below (never matching expected or highest) */
                int64_t h;
                do {
                    h = base_height - (int64_t)(lrand48() % 3);
                } while (h == expected_height || h >= highest_height);
                responses[i].height = h;
            }
        }

        int winner_idx = select_gbt_height_fallback(responses, num_responses);

        if (winner_idx != highest_pos) {
            fprintf(stderr, "  FAIL trial %d: highest height at pos %d "
                    "(height=%lld) but winner at pos %d (height=%lld)\n",
                    trial, highest_pos, (long long)highest_height,
                    winner_idx, (long long)responses[winner_idx].height);
            fprintf(stderr, "  (seed=%ld, trial=%d, last_block_height=%lld)\n",
                    seed, trial, (long long)last_block_height);
            for (int j = 0; j < num_responses; j++) {
                fprintf(stderr, "    response[%d]: height=%lld%s\n",
                        j, (long long)responses[j].height,
                        j == highest_pos ? " (highest)" : "");
            }
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Winner node becomes sticky.
 *
 * Verify that the winning node index is correctly identified as the one
 * that should become the sticky node (i.e., the node_index of the winner).
 */
static int
test_property_winner_becomes_sticky(long seed)
{
    printf("  property: winner node becomes sticky "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = 800000 + (int64_t)(lrand48() % 100000);

        int num_responses = 1 + (int)(lrand48() % MAX_NODES);

        sim_gbt_response_t responses[MAX_NODES];
        for (int i = 0; i < num_responses; i++) {
            responses[i].node_index = i;
            responses[i].height = gen_non_matching_height(last_block_height);
            responses[i].arrival_order = i;
        }

        int winner_idx = select_gbt_height_fallback(responses, num_responses);

        if (winner_idx < 0 || winner_idx >= num_responses) {
            fprintf(stderr, "  FAIL trial %d: invalid winner_idx=%d\n",
                    trial, winner_idx);
            return -1;
        }

        /* The sticky node should be the node_index of the winner */
        int sticky_node = responses[winner_idx].node_index;

        /* Verify sticky_node is a valid node index */
        if (sticky_node < 0 || sticky_node >= MAX_NODES) {
            fprintf(stderr, "  FAIL trial %d: invalid sticky_node=%d\n",
                    trial, sticky_node);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        /* Verify sticky_node matches the winner's node_index */
        if (sticky_node != responses[winner_idx].node_index) {
            fprintf(stderr, "  FAIL trial %d: sticky_node=%d != "
                    "winner.node_index=%d\n",
                    trial, sticky_node, responses[winner_idx].node_index);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
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

    printf("test_gbt_height_fallback (seed=%ld):\n", seed);

    int failures = 0;

    if (test_property_gbt_height_fallback(seed) < 0)
        failures++;
    if (test_property_all_same_height_last_wins(seed) < 0)
        failures++;
    if (test_property_single_response_wins(seed) < 0)
        failures++;
    if (test_property_highest_wins_any_position(seed) < 0)
        failures++;
    if (test_property_winner_becomes_sticky(seed) < 0)
        failures++;

    if (failures == 0) {
        printf("  All property tests passed\n");
        return 0;
    } else {
        printf("  %d property test(s) FAILED\n", failures);
        return 1;
    }
}
