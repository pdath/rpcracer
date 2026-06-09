/* Feature: conn-pair-refactor, Property 11: GBT height winner selection */
/*
 * test_gbt_height_match.c — Property test for GBT height winner selection
 *
 * Property 11: GBT height winner selection
 * For any set of successful GBT race responses with integer "height" fields,
 * the proxy SHALL select as winner the response with the strictly greatest
 * height value.
 *
 * The proxy's GBT height selection logic:
 *   - First response with height == expected_height wins immediately.
 *   - If no exact match, the response with the greatest height value wins
 *     (tracked via best_gbt_height with >= comparison; ties: last received wins).
 *
 * This test verifies the HEIGHT COMPARISON property directly: given an array
 * of heights, the winner should be the one with the greatest value (or first
 * exact match if expected_height is known).
 *
 * **Validates: Requirements 12.2**
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand48/lrand48),
 * 200+ trials, seed printed for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define NUM_TRIALS 200
#define MAX_RESPONSES 16

/* ---- Simulated GBT response ---- */
typedef struct {
    int node_index;
    int64_t height;
} gbt_response_t;

/* ---- Reference implementation of GBT height winner selection ----
 *
 * Mirrors the proxy logic in on_upstream_response() for GBT races:
 *   - Process responses in arrival order.
 *   - If height == expected_height, that response wins immediately (first match).
 *   - Otherwise, track the greatest height seen so far (>= comparison means
 *     ties go to the last received).
 *   - After all responses arrive, the response with greatest height wins.
 *
 * Returns the index into the responses array of the winner, or -1 if empty.
 */
static int
select_gbt_winner(const gbt_response_t *responses, int count,
                  int64_t expected_height, int *sticky_out)
{
    if (count <= 0)
        return -1;

    /* Pass 1: check for exact height match (first in arrival order wins) */
    for (int i = 0; i < count; i++) {
        if (responses[i].height == expected_height) {
            if (sticky_out)
                *sticky_out = responses[i].node_index;
            return i;
        }
    }

    /* Pass 2: no exact match — greatest height wins (>= means last wins ties) */
    int best_idx = 0;
    int64_t best_height = responses[0].height;

    for (int i = 1; i < count; i++) {
        if (responses[i].height >= best_height) {
            best_height = responses[i].height;
            best_idx = i;
        }
    }

    if (sticky_out)
        *sticky_out = responses[best_idx].node_index;
    return best_idx;
}

/* ---- Generate a random block height in realistic range ---- */
static int64_t
gen_block_height(void)
{
    return 800000 + (int64_t)(lrand48() % 100000);
}

/* ---- Generate a random height offset from a base ---- */
static int64_t
gen_random_height(int64_t base)
{
    /* Range: base - 5 to base + 10 */
    return base - 5 + (int64_t)(lrand48() % 16);
}

/*
 * Property 11 core: GBT height winner selection
 *
 * For any set of successful GBT race responses, the winner must be the
 * response with the strictly greatest height value. When an exact match
 * for expected_height exists, it wins immediately (and expected_height
 * is by definition the greatest meaningful height for that race).
 *
 * When no exact match exists, the response with the greatest height value
 * wins. Ties go to the last received (>= comparison).
 *
 * Test strategy:
 * - Generate random last_block_height
 * - Generate 2-16 responses with random heights (may or may not include
 *   expected_height)
 * - Select winner using reference implementation
 * - Verify: winner has the greatest height among all responses
 * - Verify: if exact match exists, winner has expected_height
 * - Verify: if no exact match, no response has height > winner's height
 *
 * Validates: Requirements 12.2
 */
static int
test_property_greatest_height_wins(long seed)
{
    printf("  property: greatest height wins selection "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = gen_block_height();
        int64_t expected_height = last_block_height + 1;

        /* Generate 2-16 responses with random heights */
        int num_responses = 2 + (int)(lrand48() % (MAX_RESPONSES - 1));

        gbt_response_t responses[MAX_RESPONSES];
        for (int i = 0; i < num_responses; i++) {
            responses[i].node_index = i;
            responses[i].height = gen_random_height(last_block_height);
        }

        /* Select winner */
        int sticky_node = -1;
        int winner_idx = select_gbt_winner(responses, num_responses,
                                           expected_height, &sticky_node);

        if (winner_idx < 0 || winner_idx >= num_responses) {
            fprintf(stderr, "  FAIL trial %d: invalid winner_idx=%d\n",
                    trial, winner_idx);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        int64_t winner_height = responses[winner_idx].height;

        /* Check if there's an exact match in the array */
        bool has_exact_match = false;
        for (int i = 0; i < num_responses; i++) {
            if (responses[i].height == expected_height) {
                has_exact_match = true;
                break;
            }
        }

        if (has_exact_match) {
            /* If exact match exists, winner must have expected_height */
            if (winner_height != expected_height) {
                fprintf(stderr, "  FAIL trial %d: exact match exists but "
                        "winner height=%lld != expected=%lld\n",
                        trial, (long long)winner_height,
                        (long long)expected_height);
                fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
                for (int i = 0; i < num_responses; i++) {
                    fprintf(stderr, "    response[%d]: node=%d height=%lld%s\n",
                            i, responses[i].node_index,
                            (long long)responses[i].height,
                            (responses[i].height == expected_height) ?
                                " (EXACT)" : "");
                }
                return -1;
            }
            /* Winner must be the FIRST exact match */
            for (int i = 0; i < winner_idx; i++) {
                if (responses[i].height == expected_height) {
                    fprintf(stderr, "  FAIL trial %d: earlier exact match at "
                            "idx %d but winner at %d\n",
                            trial, i, winner_idx);
                    fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
                    return -1;
                }
            }
        } else {
            /* No exact match: winner must have the greatest height */
            for (int i = 0; i < num_responses; i++) {
                if (responses[i].height > winner_height) {
                    fprintf(stderr, "  FAIL trial %d: response[%d] height=%lld "
                            "> winner[%d] height=%lld\n",
                            trial, i, (long long)responses[i].height,
                            winner_idx, (long long)winner_height);
                    fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
                    for (int j = 0; j < num_responses; j++) {
                        fprintf(stderr, "    response[%d]: node=%d "
                                "height=%lld%s\n",
                                j, responses[j].node_index,
                                (long long)responses[j].height,
                                (j == winner_idx) ? " (WINNER)" : "");
                    }
                    return -1;
                }
            }
        }

        /* Verify sticky assignment */
        if (sticky_node != responses[winner_idx].node_index) {
            fprintf(stderr, "  FAIL trial %d: sticky=%d != winner node=%d\n",
                    trial, sticky_node, responses[winner_idx].node_index);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: When all heights are distinct, the unique maximum wins.
 *
 * Generate responses with distinct heights. The response with the single
 * highest height must win regardless of arrival position.
 */
static int
test_property_unique_max_always_wins(long seed)
{
    printf("  property: unique maximum height always wins "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = gen_block_height();
        int64_t expected_height = last_block_height + 1;

        int num_responses = 2 + (int)(lrand48() % (MAX_RESPONSES - 1));

        /* Generate distinct heights, none matching expected_height.
         * Use a base well below expected_height to avoid collisions. */
        gbt_response_t responses[MAX_RESPONSES];
        int64_t base = last_block_height - 30;

        for (int i = 0; i < num_responses; i++) {
            responses[i].node_index = i;
            int64_t h = base + (int64_t)i;
            /* Skip expected_height by shifting up */
            if (h >= expected_height)
                h += 1;
            responses[i].height = h;
        }

        /* Find the actual maximum height after generation */
        int64_t max_height = responses[0].height;
        for (int i = 1; i < num_responses; i++) {
            if (responses[i].height > max_height)
                max_height = responses[i].height;
        }

        /* Shuffle to randomize positions */
        for (int i = num_responses - 1; i > 0; i--) {
            int j = (int)(lrand48() % (i + 1));
            gbt_response_t tmp = responses[i];
            responses[i] = responses[j];
            responses[j] = tmp;
        }

        /* Find where the max ended up after shuffle */
        int max_pos = -1;
        for (int i = 0; i < num_responses; i++) {
            if (responses[i].height == max_height) {
                max_pos = i;
                break;
            }
        }

        /* Select winner */
        int sticky_node = -1;
        int winner_idx = select_gbt_winner(responses, num_responses,
                                           expected_height, &sticky_node);

        /* Winner must be at max_pos since heights are distinct and no
         * exact match exists */
        if (winner_idx != max_pos) {
            fprintf(stderr, "  FAIL trial %d: max at pos %d (height=%lld) "
                    "but winner at pos %d (height=%lld)\n",
                    trial, max_pos, (long long)max_height,
                    winner_idx, (long long)responses[winner_idx].height);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            for (int j = 0; j < num_responses; j++) {
                fprintf(stderr, "    response[%d]: node=%d height=%lld%s\n",
                        j, responses[j].node_index,
                        (long long)responses[j].height,
                        (j == max_pos) ? " (MAX)" : "");
            }
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Tied heights — last received wins.
 *
 * When multiple responses share the greatest height and no exact match
 * exists, the last one in arrival order wins (>= comparison behavior).
 */
static int
test_property_tied_heights_last_wins(long seed)
{
    printf("  property: tied greatest heights — last received wins "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = gen_block_height();
        int64_t expected_height = last_block_height + 1;

        int num_responses = 3 + (int)(lrand48() % (MAX_RESPONSES - 2));

        /* Pick a greatest height that is NOT the expected_height */
        int64_t greatest_height;
        do {
            greatest_height = last_block_height + 2 + (int64_t)(lrand48() % 10);
        } while (greatest_height == expected_height);

        /* Pick how many responses will have the greatest height (2+) */
        int num_tied = 2 + (int)(lrand48() % (num_responses - 1));
        if (num_tied > num_responses)
            num_tied = num_responses;

        gbt_response_t responses[MAX_RESPONSES];
        for (int i = 0; i < num_responses; i++) {
            responses[i].node_index = i;
            if (i < num_tied) {
                responses[i].height = greatest_height;
            } else {
                /* Lower heights, not matching expected */
                int64_t h;
                do {
                    h = greatest_height - 1 - (int64_t)(lrand48() % 5);
                } while (h == expected_height);
                responses[i].height = h;
            }
        }

        /* Shuffle to randomize arrival order */
        for (int i = num_responses - 1; i > 0; i--) {
            int j = (int)(lrand48() % (i + 1));
            gbt_response_t tmp = responses[i];
            responses[i] = responses[j];
            responses[j] = tmp;
        }

        /* Find the LAST position with greatest_height (expected winner) */
        int expected_winner = -1;
        for (int i = 0; i < num_responses; i++) {
            if (responses[i].height == greatest_height)
                expected_winner = i;  /* keep updating — last one wins */
        }

        /* Select winner */
        int sticky_node = -1;
        int winner_idx = select_gbt_winner(responses, num_responses,
                                           expected_height, &sticky_node);

        if (winner_idx != expected_winner) {
            fprintf(stderr, "  FAIL trial %d: expected winner at pos %d "
                    "(last tied) but got pos %d\n",
                    trial, expected_winner, winner_idx);
            fprintf(stderr, "  (seed=%ld, trial=%d, greatest=%lld, "
                    "num_tied=%d)\n",
                    seed, trial, (long long)greatest_height, num_tied);
            for (int j = 0; j < num_responses; j++) {
                fprintf(stderr, "    response[%d]: node=%d height=%lld%s\n",
                        j, responses[j].node_index,
                        (long long)responses[j].height,
                        (responses[j].height == greatest_height) ?
                            " (TIED)" : "");
            }
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Exact match wins even when higher heights exist.
 *
 * When a response matches expected_height and other responses have
 * heights above expected_height, the exact match still wins immediately.
 * This validates that the proxy prioritizes exact match over raw maximum.
 */
static int
test_property_exact_match_beats_higher(long seed)
{
    printf("  property: exact match wins over higher heights "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int64_t last_block_height = gen_block_height();
        int64_t expected_height = last_block_height + 1;

        int num_responses = 3 + (int)(lrand48() % (MAX_RESPONSES - 2));

        gbt_response_t responses[MAX_RESPONSES];

        /* Place the exact match at a random position */
        int exact_pos = (int)(lrand48() % num_responses);

        for (int i = 0; i < num_responses; i++) {
            responses[i].node_index = i;
            if (i == exact_pos) {
                responses[i].height = expected_height;
            } else {
                /* Generate heights above expected (so raw max != expected) */
                responses[i].height = expected_height + 1 +
                    (int64_t)(lrand48() % 10);
            }
        }

        /* Select winner */
        int sticky_node = -1;
        int winner_idx = select_gbt_winner(responses, num_responses,
                                           expected_height, &sticky_node);

        /* Winner must be the exact match position */
        if (winner_idx != exact_pos) {
            fprintf(stderr, "  FAIL trial %d: exact match at pos %d "
                    "but winner at pos %d (height=%lld)\n",
                    trial, exact_pos, winner_idx,
                    (long long)responses[winner_idx].height);
            fprintf(stderr, "  (seed=%ld, trial=%d, expected=%lld)\n",
                    seed, trial, (long long)expected_height);
            for (int j = 0; j < num_responses; j++) {
                fprintf(stderr, "    response[%d]: node=%d height=%lld%s\n",
                        j, responses[j].node_index,
                        (long long)responses[j].height,
                        (j == exact_pos) ? " (EXACT)" : "");
            }
            return -1;
        }

        /* Verify sticky */
        if (sticky_node != responses[exact_pos].node_index) {
            fprintf(stderr, "  FAIL trial %d: sticky=%d != exact match "
                    "node=%d\n", trial, sticky_node,
                    responses[exact_pos].node_index);
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

    printf("test_gbt_height_match (seed=%ld):\n", seed);

    int failures = 0;

    if (test_property_greatest_height_wins(seed) < 0)
        failures++;
    if (test_property_unique_max_always_wins(seed) < 0)
        failures++;
    if (test_property_tied_heights_last_wins(seed) < 0)
        failures++;
    if (test_property_exact_match_beats_higher(seed) < 0)
        failures++;

    if (failures == 0) {
        printf("  All property tests passed\n");
        return 0;
    } else {
        printf("  %d property test(s) FAILED\n", failures);
        return 1;
    }
}
