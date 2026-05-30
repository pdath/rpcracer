/* test_http_notify_path.c — Property test for HTTP notify path parsing (Property 8)
 *
 * Property 8: HTTP Notify Path Parsing
 * For any valid 32-byte block hash encoded as a 64-character hexadecimal string,
 * an HTTP GET request to /NOTIFY/<hex_hash> shall correctly extract and process
 * the block hash bytes.
 *
 * Validates: Requirements 2.1
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand48/lrand48),
 * 1000 trials, seed printed for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* Include the system-under-test functions */
#include "util.h"

#define HASH_SIZE 32
#define HEX_HASH_LEN 64
#define NOTIFY_PREFIX "/NOTIFY/"
#define NOTIFY_PREFIX_LEN 8

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

/* ---- Random generation helpers ---- */

/* Generate a random 32-byte hash */
static void
gen_random_hash(uint8_t *hash)
{
    for (int i = 0; i < HASH_SIZE; i++) {
        hash[i] = (uint8_t)(lrand48() & 0xFF);
    }
}

/* ---- Path parsing logic (mirrors http_server.c) ----
 * Given a path string, check if it starts with /NOTIFY/,
 * extract the hex portion, validate it's 64 hex chars,
 * and decode to 32 bytes.
 * Returns 0 on success, -1 on failure.
 */
static int
parse_notify_path(const char *path, uint8_t *hash_out)
{
    /* Check prefix */
    if (strncmp(path, NOTIFY_PREFIX, NOTIFY_PREFIX_LEN) != 0)
        return -1;

    const char *hex_str = path + NOTIFY_PREFIX_LEN;
    size_t hex_len = strlen(hex_str);

    /* Must be exactly 64 hex characters */
    if (hex_len != HEX_HASH_LEN)
        return -1;

    /* Validate all characters are hex */
    for (size_t i = 0; i < HEX_HASH_LEN; i++) {
        char c = hex_str[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return -1;
    }

    /* Decode hex to bytes */
    if (hex_decode(hex_str, HEX_HASH_LEN, hash_out) != 0)
        return -1;

    return 0;
}

/* ---- Test strategies ---- */

/*
 * Strategy 1: Hex encode/decode round-trip
 * Generate random 32-byte values, hex-encode to 64-char strings,
 * then hex-decode back and verify the original bytes are recovered.
 */
static void
test_hex_roundtrip(long seed)
{
    printf("  strategy 1: hex encode/decode round-trip (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t original[HASH_SIZE];
        gen_random_hash(original);

        /* Encode to hex */
        char hex[HEX_HASH_LEN + 1];
        int rc = hex_encode(original, HASH_SIZE, hex);
        if (rc != 0) {
            fprintf(stderr, "  FAIL trial %d: hex_encode returned %d (seed=%ld)\n",
                    t, rc, seed);
            tests_run++;
            return;
        }

        /* Verify hex string is exactly 64 chars */
        if (strlen(hex) != HEX_HASH_LEN) {
            fprintf(stderr, "  FAIL trial %d: hex length=%zu, expected %d (seed=%ld)\n",
                    t, strlen(hex), HEX_HASH_LEN, seed);
            tests_run++;
            return;
        }

        /* Verify all chars are valid lowercase hex */
        for (size_t i = 0; i < HEX_HASH_LEN; i++) {
            char c = hex[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                fprintf(stderr, "  FAIL trial %d: non-hex char '%c' at pos %zu "
                        "(seed=%ld)\n", t, c, i, seed);
                tests_run++;
                return;
            }
        }

        /* Decode back */
        uint8_t decoded[HASH_SIZE];
        rc = hex_decode(hex, HEX_HASH_LEN, decoded);
        if (rc != 0) {
            fprintf(stderr, "  FAIL trial %d: hex_decode returned %d (seed=%ld)\n",
                    t, rc, seed);
            tests_run++;
            return;
        }

        /* Verify round-trip: decoded must equal original */
        if (memcmp(original, decoded, HASH_SIZE) != 0) {
            fprintf(stderr, "  FAIL trial %d: round-trip mismatch (seed=%ld)\n"
                    "    original: ", t, seed);
            for (int i = 0; i < HASH_SIZE; i++)
                fprintf(stderr, "%02x", original[i]);
            fprintf(stderr, "\n    decoded:  ");
            for (int i = 0; i < HASH_SIZE; i++)
                fprintf(stderr, "%02x", decoded[i]);
            fprintf(stderr, "\n");
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
 * Strategy 2: /NOTIFY/<hex> path extraction
 * Generate random 32-byte values, construct /NOTIFY/<hex> paths,
 * parse the path to extract the hex portion, decode, and verify
 * the original bytes are recovered.
 */
static void
test_notify_path_extraction(long seed)
{
    printf("  strategy 2: /NOTIFY/<hex> path extraction (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t original[HASH_SIZE];
        gen_random_hash(original);

        /* Encode to hex */
        char hex[HEX_HASH_LEN + 1];
        hex_encode(original, HASH_SIZE, hex);

        /* Construct the /NOTIFY/<hex> path */
        char path[128];
        snprintf(path, sizeof(path), "/NOTIFY/%s", hex);

        /* Parse the path and extract hash */
        uint8_t extracted[HASH_SIZE];
        int rc = parse_notify_path(path, extracted);
        if (rc != 0) {
            fprintf(stderr, "  FAIL trial %d: parse_notify_path returned %d\n"
                    "    path='%s' (seed=%ld)\n", t, rc, path, seed);
            tests_run++;
            return;
        }

        /* Verify extracted bytes match original */
        if (memcmp(original, extracted, HASH_SIZE) != 0) {
            fprintf(stderr, "  FAIL trial %d: extracted hash mismatch (seed=%ld)\n"
                    "    path='%s'\n    original: ", t, seed, path);
            for (int i = 0; i < HASH_SIZE; i++)
                fprintf(stderr, "%02x", original[i]);
            fprintf(stderr, "\n    extracted: ");
            for (int i = 0; i < HASH_SIZE; i++)
                fprintf(stderr, "%02x", extracted[i]);
            fprintf(stderr, "\n");
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
 * Strategy 3: Case-insensitive hex decoding
 * Generate random 32-byte values, hex-encode, then randomly uppercase
 * some hex characters. Verify hex_decode still recovers the original bytes
 * (since the HTTP path may contain uppercase hex from Bitcoin Core).
 */
static void
test_mixed_case_hex(long seed)
{
    printf("  strategy 3: mixed-case hex decoding (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t original[HASH_SIZE];
        gen_random_hash(original);

        /* Encode to lowercase hex */
        char hex[HEX_HASH_LEN + 1];
        hex_encode(original, HASH_SIZE, hex);

        /* Randomly uppercase some characters */
        for (size_t i = 0; i < HEX_HASH_LEN; i++) {
            if (hex[i] >= 'a' && hex[i] <= 'f' && (lrand48() % 2)) {
                hex[i] = hex[i] - 'a' + 'A';
            }
        }

        /* Construct /NOTIFY/<mixed_case_hex> path */
        char path[128];
        snprintf(path, sizeof(path), "/NOTIFY/%s", hex);

        /* Parse and extract */
        uint8_t extracted[HASH_SIZE];
        int rc = parse_notify_path(path, extracted);
        if (rc != 0) {
            fprintf(stderr, "  FAIL trial %d: parse_notify_path failed on "
                    "mixed-case hex\n    path='%s' (seed=%ld)\n", t, path, seed);
            tests_run++;
            return;
        }

        /* Verify extracted bytes match original */
        if (memcmp(original, extracted, HASH_SIZE) != 0) {
            fprintf(stderr, "  FAIL trial %d: mixed-case decode mismatch "
                    "(seed=%ld)\n    hex='%s'\n", t, seed, hex);
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
 * Strategy 4: Byte-level correctness
 * For each trial, generate a random 32-byte value, encode to hex,
 * then manually verify each byte pair in the hex string corresponds
 * to the correct byte value.
 */
static void
test_byte_level_correctness(long seed)
{
    printf("  strategy 4: byte-level hex correctness (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t original[HASH_SIZE];
        gen_random_hash(original);

        /* Encode to hex */
        char hex[HEX_HASH_LEN + 1];
        hex_encode(original, HASH_SIZE, hex);

        /* Verify each byte pair manually */
        int byte_ok = 1;
        for (int i = 0; i < HASH_SIZE; i++) {
            /* Parse the two hex chars at position i*2 and i*2+1 */
            char hi_c = hex[i * 2];
            char lo_c = hex[i * 2 + 1];

            int hi_val = -1, lo_val = -1;
            if (hi_c >= '0' && hi_c <= '9') hi_val = hi_c - '0';
            else if (hi_c >= 'a' && hi_c <= 'f') hi_val = hi_c - 'a' + 10;

            if (lo_c >= '0' && lo_c <= '9') lo_val = lo_c - '0';
            else if (lo_c >= 'a' && lo_c <= 'f') lo_val = lo_c - 'a' + 10;

            if (hi_val < 0 || lo_val < 0) {
                fprintf(stderr, "  FAIL trial %d: invalid hex chars at byte %d "
                        "('%c%c') (seed=%ld)\n", t, i, hi_c, lo_c, seed);
                byte_ok = 0;
                break;
            }

            uint8_t expected = (uint8_t)((hi_val << 4) | lo_val);
            if (expected != original[i]) {
                fprintf(stderr, "  FAIL trial %d: byte %d mismatch: "
                        "hex='%c%c'=0x%02x, original=0x%02x (seed=%ld)\n",
                        t, i, hi_c, lo_c, expected, original[i], seed);
                byte_ok = 0;
                break;
            }
        }

        if (!byte_ok) {
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
 * Strategy 5: Path prefix isolation
 * Verify that the hex portion is correctly isolated from the /NOTIFY/ prefix.
 * Generate random hashes, construct paths, and verify the extracted hex
 * substring starts at exactly offset 8 and is exactly 64 chars.
 */
static void
test_path_prefix_isolation(long seed)
{
    printf("  strategy 5: path prefix isolation (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t original[HASH_SIZE];
        gen_random_hash(original);

        /* Encode to hex */
        char hex[HEX_HASH_LEN + 1];
        hex_encode(original, HASH_SIZE, hex);

        /* Construct path */
        char path[128];
        snprintf(path, sizeof(path), "/NOTIFY/%s", hex);

        /* Verify path structure */
        size_t path_len = strlen(path);
        size_t expected_len = NOTIFY_PREFIX_LEN + HEX_HASH_LEN;

        if (path_len != expected_len) {
            fprintf(stderr, "  FAIL trial %d: path length=%zu, expected=%zu "
                    "(seed=%ld)\n", t, path_len, expected_len, seed);
            tests_run++;
            return;
        }

        /* Verify prefix is exactly "/NOTIFY/" */
        if (strncmp(path, "/NOTIFY/", 8) != 0) {
            fprintf(stderr, "  FAIL trial %d: prefix mismatch (seed=%ld)\n",
                    t, seed);
            tests_run++;
            return;
        }

        /* Verify the hex portion at offset 8 matches what we encoded */
        const char *extracted_hex = path + 8;
        if (strncmp(extracted_hex, hex, HEX_HASH_LEN) != 0) {
            fprintf(stderr, "  FAIL trial %d: hex portion mismatch\n"
                    "    expected='%s'\n    got='%.64s'\n    (seed=%ld)\n",
                    t, hex, extracted_hex, seed);
            tests_run++;
            return;
        }

        /* Decode the extracted hex and verify bytes */
        uint8_t decoded[HASH_SIZE];
        if (hex_decode(extracted_hex, HEX_HASH_LEN, decoded) != 0) {
            fprintf(stderr, "  FAIL trial %d: hex_decode of extracted portion "
                    "failed (seed=%ld)\n", t, seed);
            tests_run++;
            return;
        }

        if (memcmp(original, decoded, HASH_SIZE) != 0) {
            fprintf(stderr, "  FAIL trial %d: decoded bytes mismatch "
                    "(seed=%ld)\n", t, seed);
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

    printf("test_http_notify_path — Property 8: HTTP Notify Path Parsing "
           "(seed=%ld):\n", seed);

    /* Run all strategies */
    test_hex_roundtrip(seed);
    test_notify_path_extraction(seed);
    test_mixed_case_hex(seed);
    test_byte_level_correctness(seed);
    test_path_prefix_isolation(seed);

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
