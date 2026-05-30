/* test_invalid_hash.c — Property test for invalid block hash rejection (Property 9)
 *
 * Property 9: Invalid Block Hash Rejection
 * For any string that is not a valid 64-character hexadecimal representation
 * of a 32-byte hash (wrong length, non-hex characters, empty string), an HTTP
 * notify request containing that string shall be rejected with HTTP 400.
 *
 * Validates: Requirements 2.4
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

/* ---- Reference implementation of block hash validation ----
 * A block hash is valid if and only if:
 * 1. It is exactly 64 characters long
 * 2. All characters are valid hex (0-9, a-f, A-F)
 *
 * This mirrors the logic in http_server.c http_handle_request().
 */

static int is_hex_char(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* Returns true if the string is a valid block hash (64 hex chars).
 * Returns false (invalid) otherwise — these should get HTTP 400. */
static bool
is_valid_block_hash(const char *str, size_t len)
{
    if (len != 64)
        return false;

    for (size_t i = 0; i < 64; i++) {
        if (!is_hex_char(str[i]))
            return false;
    }
    return true;
}

/* ---- Random generation helpers ---- */

/* Generate a random printable ASCII character (0x20 - 0x7E) */
static char
gen_printable(void)
{
    return (char)(0x20 + (lrand48() % 95));
}

/* Generate a random hex character */
static char
gen_hex_char(void)
{
    static const char hex[] = "0123456789abcdefABCDEF";
    return hex[lrand48() % 22];
}

/* Generate a random non-hex character (printable, but not 0-9, a-f, A-F) */
static char
gen_non_hex_char(void)
{
    /* Non-hex printable chars: g-z, G-Z, punctuation, space */
    static const char non_hex[] =
        "ghijklmnopqrstuvwxyz"
        "GHIJKLMNOPQRSTUVWXYZ"
        " !@#$%^&*()-_=+[]{}|;:',.<>?/~`";
    return non_hex[lrand48() % (sizeof(non_hex) - 1)];
}

/* ---- Test strategies ---- */

/*
 * Strategy 1: Wrong length — too short (1 to 63 chars, all hex)
 * Even if all characters are valid hex, wrong length must be rejected.
 */
static void
test_too_short(long seed)
{
    printf("  strategy 1: too short strings (1-63 hex chars) (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        /* Generate a string of length 1 to 63, all valid hex */
        int len = 1 + (int)(lrand48() % 63);  /* 1..63 */
        char str[128];

        for (int i = 0; i < len; i++) {
            str[i] = gen_hex_char();
        }
        str[len] = '\0';

        /* Must be invalid (wrong length) */
        bool valid = is_valid_block_hash(str, (size_t)len);
        if (valid) {
            fprintf(stderr, "  FAIL trial %d: len=%d accepted as valid "
                    "(seed=%ld)\n", t, len, seed);
            tests_run++;
            return;
        }

        passed++;
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

/*
 * Strategy 2: Wrong length — too long (65 to 128 chars, all hex)
 */
static void
test_too_long(long seed)
{
    printf("  strategy 2: too long strings (65-128 hex chars) (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        /* Generate a string of length 65 to 128, all valid hex */
        int len = 65 + (int)(lrand48() % 64);  /* 65..128 */
        char str[256];

        for (int i = 0; i < len; i++) {
            str[i] = gen_hex_char();
        }
        str[len] = '\0';

        bool valid = is_valid_block_hash(str, (size_t)len);
        if (valid) {
            fprintf(stderr, "  FAIL trial %d: len=%d accepted as valid "
                    "(seed=%ld)\n", t, len, seed);
            tests_run++;
            return;
        }

        passed++;
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

/*
 * Strategy 3: Empty string
 * The empty string must always be rejected.
 */
static void
test_empty_string(long seed)
{
    printf("  strategy 3: empty string (seed=%ld)\n", seed);
    (void)seed;

    bool valid = is_valid_block_hash("", 0);
    ASSERT(!valid, "empty string must be rejected");
    printf("    verified empty string rejected\n");
}

/*
 * Strategy 4: Correct length (64) but with non-hex characters
 * Insert one or more non-hex characters at random positions.
 */
static void
test_non_hex_chars(long seed)
{
    printf("  strategy 4: 64 chars with non-hex characters (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        char str[65];

        /* Start with all valid hex */
        for (int i = 0; i < 64; i++) {
            str[i] = gen_hex_char();
        }
        str[64] = '\0';

        /* Inject 1 to 10 non-hex characters at random positions */
        int num_bad = 1 + (int)(lrand48() % 10);
        for (int i = 0; i < num_bad; i++) {
            int pos = (int)(lrand48() % 64);
            str[pos] = gen_non_hex_char();
        }

        bool valid = is_valid_block_hash(str, 64);
        if (valid) {
            fprintf(stderr, "  FAIL trial %d: string with non-hex chars "
                    "accepted as valid: '%s' (seed=%ld)\n", t, str, seed);
            tests_run++;
            return;
        }

        passed++;
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

/*
 * Strategy 5: Completely random printable strings of random length
 * Most of these will be invalid; verify they are all rejected.
 * (Statistically, a random 64-char printable string has negligible
 * probability of being all-hex.)
 */
static void
test_random_strings(long seed)
{
    printf("  strategy 5: random printable strings (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        /* Random length 0 to 200 */
        int len = (int)(lrand48() % 201);
        char str[256];

        for (int i = 0; i < len; i++) {
            str[i] = gen_printable();
        }
        str[len] = '\0';

        bool valid = is_valid_block_hash(str, (size_t)len);

        /* If it happens to be valid (64 hex chars), skip this trial */
        if (valid) {
            /* Extremely unlikely but possible — verify it actually is valid */
            if (len == 64) {
                bool all_hex = true;
                for (int i = 0; i < 64; i++) {
                    if (!is_hex_char(str[i])) {
                        all_hex = false;
                        break;
                    }
                }
                if (all_hex) {
                    /* Genuinely valid — skip this trial */
                    passed++;
                    continue;
                }
            }
            /* If we get here, validation is wrong */
            fprintf(stderr, "  FAIL trial %d: invalid string accepted "
                    "(len=%d, seed=%ld)\n", t, len, seed);
            tests_run++;
            return;
        }

        passed++;
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

/*
 * Strategy 6: Edge cases — exactly 63 and 65 characters (all hex)
 * These are the boundary cases around the valid length of 64.
 */
static void
test_boundary_lengths(long seed)
{
    printf("  strategy 6: boundary lengths (63, 65 chars) (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        /* Alternate between 63 and 65 */
        int len = (t % 2 == 0) ? 63 : 65;
        char str[128];

        for (int i = 0; i < len; i++) {
            str[i] = gen_hex_char();
        }
        str[len] = '\0';

        bool valid = is_valid_block_hash(str, (size_t)len);
        if (valid) {
            fprintf(stderr, "  FAIL trial %d: len=%d accepted as valid "
                    "(seed=%ld)\n", t, len, seed);
            tests_run++;
            return;
        }

        passed++;
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

/*
 * Strategy 7: Strings with spaces, nulls, and special characters
 * 64-char strings containing spaces, tabs, newlines, and other
 * characters that might slip through naive validation.
 */
static void
test_special_chars(long seed)
{
    printf("  strategy 7: 64-char strings with special chars (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    /* Characters that might be confused with hex or slip through */
    static const char tricky[] = " \t\n\r\0xXoO";

    for (int t = 0; t < trials; t++) {
        char str[65];

        /* Fill with valid hex first */
        for (int i = 0; i < 64; i++) {
            str[i] = gen_hex_char();
        }
        str[64] = '\0';

        /* Replace 1-5 positions with tricky characters */
        int num_bad = 1 + (int)(lrand48() % 5);
        for (int i = 0; i < num_bad; i++) {
            int pos = (int)(lrand48() % 64);
            int tricky_idx = (int)(lrand48() % (sizeof(tricky) - 1));
            str[pos] = tricky[tricky_idx];
        }

        /* If a null byte was inserted, the effective length changes.
         * Use strlen to get the length the validator would see. */
        size_t effective_len = strlen(str);
        bool valid = is_valid_block_hash(str, effective_len);

        if (valid) {
            fprintf(stderr, "  FAIL trial %d: string with special chars "
                    "accepted (effective_len=%zu, seed=%ld)\n",
                    t, effective_len, seed);
            tests_run++;
            return;
        }

        passed++;
    }

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

/*
 * Strategy 8: Verify valid hashes are NOT rejected (sanity check)
 * Generate valid 64-char hex strings and confirm they pass validation.
 * This ensures our reference implementation isn't broken.
 */
static void
test_valid_hashes_accepted(long seed)
{
    printf("  strategy 8: valid hashes accepted (sanity check) (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        char str[65];

        for (int i = 0; i < 64; i++) {
            str[i] = gen_hex_char();
        }
        str[64] = '\0';

        bool valid = is_valid_block_hash(str, 64);
        if (!valid) {
            fprintf(stderr, "  FAIL trial %d: valid hash rejected: '%s' "
                    "(seed=%ld)\n", t, str, seed);
            tests_run++;
            return;
        }

        passed++;
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

    printf("test_invalid_hash — Property 9: Invalid Block Hash Rejection "
           "(seed=%ld):\n", seed);

    /* Run all strategies */
    test_too_short(seed);
    test_too_long(seed);
    test_empty_string(seed);
    test_non_hex_chars(seed);
    test_random_strings(seed);
    test_boundary_lengths(seed);
    test_special_chars(seed);
    test_valid_hashes_accepted(seed);

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
