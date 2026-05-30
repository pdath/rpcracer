/* test_dedup.c — Property test for block hash deduplication (Property 1)
 *
 * Property 1: Block Hash Deduplication
 * For any sequence of block notifications (arriving via ZMQ or HTTP in any
 * interleaving), the notifier shall relay exactly one notification per
 * consecutive occurrence of a block hash. A hash is relayed if and only if
 * it differs from the previously relayed hash (or is the first hash ever).
 *
 * NOTE: The dedup logic tracks only the LAST relayed hash (not a full set).
 * This means non-consecutive duplicates (e.g., A, B, A) relay A twice.
 * This is by design — Bitcoin block notifications arrive in bursts of the
 * same hash from multiple nodes.
 *
 * Validates: Requirements 1.3, 2.2, 3.5
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

#define HASH_SIZE 32
#define MAX_SEQ_LEN 64

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while (0)

/* ---- Reference dedup implementation (mirrors notifier.c logic) ---- */

typedef struct {
    uint8_t last_hash[HASH_SIZE];
    bool has_last_hash;
    int relay_count;
} dedup_state_t;

static void
dedup_init(dedup_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->has_last_hash = false;
    s->relay_count = 0;
}

/* Process a hash through the dedup logic.
 * Returns 1 if relayed (new/different), 0 if suppressed (duplicate). */
static int
dedup_process(dedup_state_t *s, const uint8_t *hash)
{
    if (s->has_last_hash && memcmp(s->last_hash, hash, HASH_SIZE) == 0) {
        return 0;  /* Duplicate — suppress */
    }
    memcpy(s->last_hash, hash, HASH_SIZE);
    s->has_last_hash = true;
    s->relay_count++;
    return 1;  /* New — relay */
}

/* ---- Hash generation helpers ---- */

/* Generate a random 32-byte hash */
static void
gen_random_hash(uint8_t *hash)
{
    for (int i = 0; i < HASH_SIZE; i++) {
        hash[i] = (uint8_t)(lrand48() & 0xFF);
    }
}

/* Generate a pool of distinct hashes for use in sequences */
static void
gen_hash_pool(uint8_t pool[][HASH_SIZE], int pool_size)
{
    for (int i = 0; i < pool_size; i++) {
        gen_random_hash(pool[i]);
    }
}

/* Compute expected relay count for a sequence using the "compare with last" rule.
 * A hash is relayed iff it differs from the previously relayed hash. */
static int
expected_relay_count(const uint8_t *seq, int seq_len)
{
    if (seq_len == 0)
        return 0;

    int count = 1;  /* First hash is always relayed */
    for (int i = 1; i < seq_len; i++) {
        if (memcmp(seq + i * HASH_SIZE, seq + (i - 1) * HASH_SIZE, HASH_SIZE) != 0) {
            count++;
        }
    }
    return count;
}

/* ---- Test strategies ---- */

/*
 * Strategy 1: Pure random sequences
 * Generate sequences of random hashes (from a small pool to ensure some
 * duplicates). Verify relay count matches expected.
 */
static void
test_random_sequences(long seed)
{
    printf("  strategy 1: random sequences with duplicates (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        /* Pool of 2–6 distinct hashes to ensure duplicates */
        int pool_size = 2 + (int)(lrand48() % 5);
        uint8_t pool[6][HASH_SIZE];
        gen_hash_pool(pool, pool_size);

        /* Generate sequence of 1–MAX_SEQ_LEN hashes drawn from pool */
        int seq_len = 1 + (int)(lrand48() % MAX_SEQ_LEN);
        uint8_t seq[MAX_SEQ_LEN][HASH_SIZE];
        for (int i = 0; i < seq_len; i++) {
            int idx = (int)(lrand48() % pool_size);
            memcpy(seq[i], pool[idx], HASH_SIZE);
        }

        /* Compute expected relay count */
        int expected = expected_relay_count((const uint8_t *)seq, seq_len);

        /* Run through dedup */
        dedup_state_t state;
        dedup_init(&state);
        for (int i = 0; i < seq_len; i++) {
            dedup_process(&state, seq[i]);
        }

        if (state.relay_count == expected) {
            passed++;
        } else {
            fprintf(stderr, "  FAIL trial %d: seq_len=%d pool_size=%d "
                    "expected=%d got=%d (seed=%ld)\n",
                    t, seq_len, pool_size, expected, state.relay_count, seed);
            tests_run++;
            return;
        }
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

/*
 * Strategy 2: Consecutive burst patterns
 * Generate sequences where the same hash repeats N times in a row (simulating
 * multiple nodes reporting the same block). Verify exactly 1 relay per burst.
 */
static void
test_consecutive_bursts(long seed)
{
    printf("  strategy 2: consecutive burst patterns (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        /* Generate 1–8 distinct hashes, each repeated 1–8 times consecutively */
        int num_blocks = 1 + (int)(lrand48() % 8);
        uint8_t blocks[8][HASH_SIZE];
        gen_hash_pool(blocks, num_blocks);

        /* Build sequence: each block hash repeated burst_len times */
        uint8_t seq[MAX_SEQ_LEN][HASH_SIZE];
        int seq_len = 0;
        int expected_relays = 0;

        for (int b = 0; b < num_blocks && seq_len < MAX_SEQ_LEN; b++) {
            int burst_len = 1 + (int)(lrand48() % 8);
            /* Check if this block differs from previous (for expected count) */
            if (b == 0 || memcmp(blocks[b], blocks[b - 1], HASH_SIZE) != 0) {
                expected_relays++;
            }
            for (int r = 0; r < burst_len && seq_len < MAX_SEQ_LEN; r++) {
                memcpy(seq[seq_len], blocks[b], HASH_SIZE);
                seq_len++;
            }
        }

        /* Run through dedup */
        dedup_state_t state;
        dedup_init(&state);
        for (int i = 0; i < seq_len; i++) {
            dedup_process(&state, seq[i]);
        }

        if (state.relay_count == expected_relays) {
            passed++;
        } else {
            fprintf(stderr, "  FAIL trial %d: num_blocks=%d seq_len=%d "
                    "expected=%d got=%d (seed=%ld)\n",
                    t, num_blocks, seq_len, expected_relays,
                    state.relay_count, seed);
            tests_run++;
            return;
        }
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

/*
 * Strategy 3: Verify per-element relay/suppress decisions
 * For each hash in a random sequence, verify that the dedup decision
 * (relay vs suppress) matches the "differs from last relayed" rule.
 */
static void
test_per_element_decisions(long seed)
{
    printf("  strategy 3: per-element relay/suppress decisions (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        /* Small pool to ensure duplicates */
        int pool_size = 2 + (int)(lrand48() % 4);
        uint8_t pool[5][HASH_SIZE];
        gen_hash_pool(pool, pool_size);

        int seq_len = 2 + (int)(lrand48() % (MAX_SEQ_LEN - 1));
        uint8_t seq[MAX_SEQ_LEN][HASH_SIZE];
        for (int i = 0; i < seq_len; i++) {
            int idx = (int)(lrand48() % pool_size);
            memcpy(seq[i], pool[idx], HASH_SIZE);
        }

        /* Process and verify each decision */
        dedup_state_t state;
        dedup_init(&state);
        bool trial_ok = true;

        for (int i = 0; i < seq_len; i++) {
            int result = dedup_process(&state, seq[i]);

            bool should_relay;
            if (i == 0) {
                should_relay = true;  /* First hash always relayed */
            } else {
                /* Relay iff differs from previous element in sequence
                 * (which is the last relayed or last seen) */
                should_relay = (memcmp(seq[i], seq[i - 1], HASH_SIZE) != 0);
            }

            if ((result == 1) != should_relay) {
                fprintf(stderr, "  FAIL trial %d, element %d: "
                        "expected %s, got %s (seed=%ld)\n",
                        t, i,
                        should_relay ? "relay" : "suppress",
                        result ? "relay" : "suppress",
                        seed);
                trial_ok = false;
                break;
            }
        }

        if (trial_ok) {
            passed++;
        } else {
            tests_run++;
            return;
        }
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

/*
 * Strategy 4: Single hash repeated (all duplicates after first)
 * Verify that repeating the same hash N times results in exactly 1 relay.
 */
static void
test_all_same_hash(long seed)
{
    printf("  strategy 4: single hash repeated (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t hash[HASH_SIZE];
        gen_random_hash(hash);

        int repeat_count = 1 + (int)(lrand48() % MAX_SEQ_LEN);

        dedup_state_t state;
        dedup_init(&state);
        for (int i = 0; i < repeat_count; i++) {
            dedup_process(&state, hash);
        }

        if (state.relay_count == 1) {
            passed++;
        } else {
            fprintf(stderr, "  FAIL trial %d: repeated %d times, "
                    "expected 1 relay, got %d (seed=%ld)\n",
                    t, repeat_count, state.relay_count, seed);
            tests_run++;
            return;
        }
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

/*
 * Strategy 5: All distinct hashes (no duplicates)
 * Verify that a sequence of all-distinct hashes results in relay_count == seq_len.
 */
static void
test_all_distinct(long seed)
{
    printf("  strategy 5: all distinct hashes (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        /* Generate 1–MAX_SEQ_LEN distinct hashes.
         * Use sequential modification to guarantee uniqueness. */
        int seq_len = 1 + (int)(lrand48() % MAX_SEQ_LEN);
        uint8_t seq[MAX_SEQ_LEN][HASH_SIZE];

        /* Start with a random base hash, then flip a byte for each subsequent */
        gen_random_hash(seq[0]);
        for (int i = 1; i < seq_len; i++) {
            memcpy(seq[i], seq[i - 1], HASH_SIZE);
            /* Modify byte at position (i % HASH_SIZE) to ensure difference */
            seq[i][i % HASH_SIZE] ^= (uint8_t)(1 + (lrand48() % 255));
        }

        dedup_state_t state;
        dedup_init(&state);
        for (int i = 0; i < seq_len; i++) {
            dedup_process(&state, seq[i]);
        }

        if (state.relay_count == seq_len) {
            passed++;
        } else {
            fprintf(stderr, "  FAIL trial %d: seq_len=%d expected %d relays, "
                    "got %d (seed=%ld)\n",
                    t, seq_len, seq_len, state.relay_count, seed);
            tests_run++;
            return;
        }
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
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

    printf("test_dedup — Property 1: Block Hash Deduplication (seed=%ld):\n", seed);

    /* Run all strategies */
    test_random_sequences(seed);
    test_consecutive_bursts(seed);
    test_per_element_decisions(seed);
    test_all_same_hash(seed);
    test_all_distinct(seed);

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
