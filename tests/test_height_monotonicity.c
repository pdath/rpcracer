/* test_height_monotonicity.c — Property test for block height monotonicity (Property 7)
 *
 * Property 7: Block Height Monotonicity
 * For any sequence of successful getblocktemplate() responses processed over
 * time, the recorded `last_block_height` value shall be monotonically
 * non-decreasing — it is updated only when a response height is strictly
 * greater than the current recorded value.
 *
 * **Validates: Requirements 5.5**
 *
 * This test simulates the height update logic independently — it does NOT
 * call into rpc_proxy.c. It generates random sequences of heights and
 * applies the update rule:
 *   if (new_height > last_block_height) last_block_height = new_height;
 *
 * Verifications:
 *   1. last_block_height is monotonically non-decreasing throughout the sequence
 *   2. Final last_block_height equals the maximum height seen in the sequence
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

#define NUM_TRIALS    1000
#define MIN_SEQ_LEN   10
#define MAX_SEQ_LEN   100

/* ---- Height update rule (mirrors rpc_proxy.c logic) ---- */
static void
height_update(int64_t *last_block_height, int64_t new_height)
{
    if (new_height > *last_block_height)
        *last_block_height = new_height;
}

/* ---- Generate a random height value ----
 * Produces heights that go up, down, stay the same, and jump around.
 * Range: 0 to ~1,000,000 to simulate realistic block heights with variation.
 */
static int64_t
gen_random_height(void)
{
    /* Mix of different height ranges to create interesting sequences:
     * - 40% chance: typical block height range (800000-900000)
     * - 20% chance: small values (0-1000)
     * - 20% chance: medium values (100000-500000)
     * - 10% chance: large values (900000-1000000)
     * - 10% chance: very large values (up to INT32_MAX)
     */
    int r = (int)(lrand48() % 100);
    if (r < 40) {
        return 800000 + (lrand48() % 100001);  /* 800000-900000 */
    } else if (r < 60) {
        return lrand48() % 1001;                /* 0-1000 */
    } else if (r < 80) {
        return 100000 + (lrand48() % 400001);   /* 100000-500000 */
    } else if (r < 90) {
        return 900000 + (lrand48() % 100001);   /* 900000-1000000 */
    } else {
        return lrand48() % 2147483647;          /* 0 to INT32_MAX-1 */
    }
}

/*
 * Property 7: Block Height Monotonicity — Core property
 *
 * Generate random sequences of heights (10-100 per trial), apply the
 * height update rule, and verify:
 *   1. last_block_height never decreases at any point in the sequence
 *   2. Final last_block_height equals the maximum height in the sequence
 *
 * Validates: Requirements 5.5
 */
static int
test_property_height_monotonicity(long seed)
{
    printf("  property: height monotonicity (seed=%ld, %d trials)\n",
           seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Generate random sequence length (10-100) */
        int seq_len = MIN_SEQ_LEN + (int)(lrand48() % (MAX_SEQ_LEN - MIN_SEQ_LEN + 1));

        /* Start with initial last_block_height of 0 (startup state) */
        int64_t last_block_height = 0;
        int64_t prev_block_height = 0;
        int64_t max_height_seen = 0;

        for (int i = 0; i < seq_len; i++) {
            int64_t new_height = gen_random_height();

            /* Track the maximum height in the sequence */
            if (new_height > max_height_seen)
                max_height_seen = new_height;

            /* Apply the update rule */
            height_update(&last_block_height, new_height);

            /* Verify: last_block_height must be >= previous value (monotonically non-decreasing) */
            if (last_block_height < prev_block_height) {
                fprintf(stderr, "  FAIL trial %d step %d: height decreased "
                        "from %ld to %ld (new_height=%ld)\n",
                        trial, i, (long)prev_block_height,
                        (long)last_block_height, (long)new_height);
                fprintf(stderr, "  (seed=%ld, trial=%d, seq_len=%d)\n",
                        seed, trial, seq_len);
                return -1;
            }

            prev_block_height = last_block_height;
        }

        /* Verify: final last_block_height equals the maximum height seen */
        if (last_block_height != max_height_seen) {
            fprintf(stderr, "  FAIL trial %d: final height %ld != max seen %ld\n",
                    trial, (long)last_block_height, (long)max_height_seen);
            fprintf(stderr, "  (seed=%ld, trial=%d, seq_len=%d)\n",
                    seed, trial, seq_len);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Height update is idempotent for same or lower values.
 *
 * Applying the same height or a lower height multiple times should not
 * change last_block_height.
 */
static int
test_property_height_idempotent(long seed)
{
    printf("  property: height update idempotent for same/lower values "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Set an initial height */
        int64_t initial_height = gen_random_height();
        int64_t last_block_height = initial_height;

        /* Apply the same height multiple times */
        int repeat_count = 1 + (int)(lrand48() % 10);
        for (int i = 0; i < repeat_count; i++) {
            height_update(&last_block_height, initial_height);
        }

        if (last_block_height != initial_height) {
            fprintf(stderr, "  FAIL trial %d: same height changed value "
                    "from %ld to %ld\n",
                    trial, (long)initial_height, (long)last_block_height);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        /* Apply lower heights — should not change */
        int lower_count = 1 + (int)(lrand48() % 10);
        for (int i = 0; i < lower_count; i++) {
            int64_t lower = lrand48() % (initial_height > 0 ? initial_height : 1);
            height_update(&last_block_height, lower);
        }

        if (last_block_height != initial_height) {
            fprintf(stderr, "  FAIL trial %d: lower height changed value "
                    "from %ld to %ld\n",
                    trial, (long)initial_height, (long)last_block_height);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
            return -1;
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Strictly increasing heights always update.
 *
 * If we feed a strictly increasing sequence, last_block_height must
 * equal the last (highest) value after each step.
 */
static int
test_property_increasing_always_updates(long seed)
{
    printf("  property: strictly increasing heights always update "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int seq_len = MIN_SEQ_LEN + (int)(lrand48() % (MAX_SEQ_LEN - MIN_SEQ_LEN + 1));

        int64_t last_block_height = 0;
        int64_t current = (int64_t)(lrand48() % 1000);  /* random start */

        for (int i = 0; i < seq_len; i++) {
            /* Generate strictly increasing height */
            current += 1 + (lrand48() % 100);  /* increment by 1-100 */

            height_update(&last_block_height, current);

            /* After update with a strictly greater value, last_block_height must equal it */
            if (last_block_height != current) {
                fprintf(stderr, "  FAIL trial %d step %d: height not updated "
                        "to %ld (got %ld)\n",
                        trial, i, (long)current, (long)last_block_height);
                fprintf(stderr, "  (seed=%ld, trial=%d, seq_len=%d)\n",
                        seed, trial, seq_len);
                return -1;
            }
        }

        passed++;
    }

    printf("    %d/%d trials passed\n", passed, NUM_TRIALS);
    return 0;
}

/*
 * Sub-property: Mixed sequence — height only changes on strictly greater values.
 *
 * For each step in a random sequence, verify:
 *   - If new_height > last_block_height before update: last_block_height == new_height after
 *   - If new_height <= last_block_height before update: last_block_height unchanged
 */
static int
test_property_update_only_on_greater(long seed)
{
    printf("  property: update only on strictly greater values "
           "(seed=%ld, %d trials)\n", seed, NUM_TRIALS);
    srand48(seed);

    int passed = 0;

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        int seq_len = MIN_SEQ_LEN + (int)(lrand48() % (MAX_SEQ_LEN - MIN_SEQ_LEN + 1));

        int64_t last_block_height = 0;

        for (int i = 0; i < seq_len; i++) {
            int64_t new_height = gen_random_height();
            int64_t before = last_block_height;

            height_update(&last_block_height, new_height);

            if (new_height > before) {
                /* Should have updated */
                if (last_block_height != new_height) {
                    fprintf(stderr, "  FAIL trial %d step %d: should have "
                            "updated to %ld but got %ld (before=%ld)\n",
                            trial, i, (long)new_height,
                            (long)last_block_height, (long)before);
                    fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
                    return -1;
                }
            } else {
                /* Should NOT have updated */
                if (last_block_height != before) {
                    fprintf(stderr, "  FAIL trial %d step %d: should not have "
                            "updated (new=%ld <= before=%ld) but got %ld\n",
                            trial, i, (long)new_height, (long)before,
                            (long)last_block_height);
                    fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, trial);
                    return -1;
                }
            }
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

    printf("test_height_monotonicity (seed=%ld):\n", seed);

    int failures = 0;

    if (test_property_height_monotonicity(seed) < 0)
        failures++;
    if (test_property_height_idempotent(seed) < 0)
        failures++;
    if (test_property_increasing_always_updates(seed) < 0)
        failures++;
    if (test_property_update_only_on_greater(seed) < 0)
        failures++;

    if (failures == 0) {
        printf("  All property tests passed\n");
        return 0;
    } else {
        printf("  %d property test(s) FAILED\n", failures);
        return 1;
    }
}
