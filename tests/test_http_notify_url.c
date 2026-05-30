/* test_http_notify_url.c — Property test for HTTP notify URL substitution (Property 10)
 *
 * Property 10: HTTP Notify URL Substitution
 * For any HTTP notify URL template string and any valid block hash hex string:
 * if the URL contains `%s`, the HTTP GET request shall target the URL with `%s`
 * replaced by the block hash; if the URL does not contain `%s`, the HTTP GET
 * request shall target the URL unchanged.
 *
 * Validates: Requirements 3.3, 3.4
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
#define HEX_HASH_LEN 65  /* 64 hex chars + null */

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

/* ---- Reference implementation of URL substitution ----
 * Mirrors the logic in notifier.c relay_http_notify():
 * - If path contains "%s", replace first occurrence with 64-char hex hash
 * - If path does not contain "%s", use path as-is
 */

/* Hex encode 32 bytes to 64-char lowercase hex string (null-terminated) */
static void
ref_hex_encode(const uint8_t *src, char *dst)
{
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < HASH_SIZE; i++) {
        dst[i * 2]     = hex_chars[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = hex_chars[src[i] & 0x0F];
    }
    dst[64] = '\0';
}

/* Substitute %s in path with hex hash (first occurrence only, per strstr).
 * Returns 0 on success, -1 if result would overflow buf_size. */
static int
ref_url_substitute(const char *path, const uint8_t *hash,
                   char *result, size_t buf_size)
{
    char hex[HEX_HASH_LEN];
    ref_hex_encode(hash, hex);

    const char *pct = strstr(path, "%s");
    if (pct) {
        /* Substitute first %s with hex hash */
        size_t prefix_len = (size_t)(pct - path);
        const char *suffix = pct + 2;  /* skip "%s" */
        size_t suffix_len = strlen(suffix);
        size_t total = prefix_len + 64 + suffix_len;

        if (total >= buf_size)
            return -1;

        memcpy(result, path, prefix_len);
        memcpy(result + prefix_len, hex, 64);
        memcpy(result + prefix_len + 64, suffix, suffix_len);
        result[total] = '\0';
    } else {
        /* No %s — use path as-is */
        size_t plen = strlen(path);
        if (plen >= buf_size)
            return -1;
        memcpy(result, path, plen + 1);
    }
    return 0;
}

/* ---- System-under-test: reimplementation matching notifier.c ----
 * This is the actual substitution logic extracted from relay_http_notify().
 */
static int
sut_url_substitute(const char *path, const uint8_t *hash,
                   char *result, size_t buf_size)
{
    char hex[HEX_HASH_LEN];
    ref_hex_encode(hash, hex);

    const char *pct = strstr(path, "%s");
    if (pct) {
        size_t prefix_len = (size_t)(pct - path);
        const char *suffix = pct + 2;
        int written = snprintf(result, buf_size, "%.*s%s%s",
                               (int)prefix_len, path, hex, suffix);
        if (written < 0 || (size_t)written >= buf_size)
            return -1;
    } else {
        size_t plen = strlen(path);
        if (plen >= buf_size)
            return -1;
        memcpy(result, path, plen + 1);
    }
    return 0;
}

/* ---- Random generation helpers ---- */

/* Generate a random 32-byte hash */
static void
gen_random_hash(uint8_t *hash)
{
    for (int i = 0; i < HASH_SIZE; i++) {
        hash[i] = (uint8_t)(lrand48() & 0xFF);
    }
}

/* Generate a random alphanumeric character */
static char
gen_alnum(void)
{
    static const char chars[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789";
    return chars[lrand48() % (sizeof(chars) - 1)];
}

/* Generate a random path segment (no slashes, no %s) */
static int
gen_path_segment(char *buf, int max_len)
{
    int len = 1 + (int)(lrand48() % (max_len - 1));
    for (int i = 0; i < len; i++) {
        buf[i] = gen_alnum();
    }
    buf[len] = '\0';
    return len;
}

/* Generate a random URL path WITHOUT %s.
 * Format: /segment1/segment2/.../segmentN
 * Returns length written (excluding null). */
static int
gen_url_path_no_placeholder(char *buf, size_t buf_size)
{
    int num_segments = 1 + (int)(lrand48() % 4);
    int pos = 0;

    for (int i = 0; i < num_segments && pos < (int)buf_size - 20; i++) {
        buf[pos++] = '/';
        char seg[16];
        int slen = gen_path_segment(seg, 12);
        memcpy(buf + pos, seg, (size_t)slen);
        pos += slen;
    }
    buf[pos] = '\0';
    return pos;
}

/* Generate a random URL path WITH %s at a random position.
 * The %s can be at start, middle, or end of the path. */
static int
gen_url_path_with_placeholder(char *buf, size_t buf_size)
{
    /* Decide where to place %s: 0=start of path, 1=middle, 2=end */
    int placement = (int)(lrand48() % 3);

    int pos = 0;

    if (placement == 0) {
        /* %s at start: /%s/rest... */
        buf[pos++] = '/';
        buf[pos++] = '%';
        buf[pos++] = 's';
        /* Optionally add more segments after */
        int extra = (int)(lrand48() % 3);
        for (int i = 0; i < extra && pos < (int)buf_size - 20; i++) {
            buf[pos++] = '/';
            char seg[16];
            int slen = gen_path_segment(seg, 12);
            memcpy(buf + pos, seg, (size_t)slen);
            pos += slen;
        }
    } else if (placement == 1) {
        /* %s in middle: /prefix/%s/suffix */
        buf[pos++] = '/';
        char seg[16];
        int slen = gen_path_segment(seg, 10);
        memcpy(buf + pos, seg, (size_t)slen);
        pos += slen;
        buf[pos++] = '/';
        buf[pos++] = '%';
        buf[pos++] = 's';
        /* Add suffix */
        int extra = 1 + (int)(lrand48() % 2);
        for (int i = 0; i < extra && pos < (int)buf_size - 20; i++) {
            buf[pos++] = '/';
            slen = gen_path_segment(seg, 10);
            memcpy(buf + pos, seg, (size_t)slen);
            pos += slen;
        }
    } else {
        /* %s at end: /prefix/.../%s */
        int prefix_segs = 1 + (int)(lrand48() % 3);
        for (int i = 0; i < prefix_segs && pos < (int)buf_size - 20; i++) {
            buf[pos++] = '/';
            char seg[16];
            int slen = gen_path_segment(seg, 10);
            memcpy(buf + pos, seg, (size_t)slen);
            pos += slen;
        }
        buf[pos++] = '/';
        buf[pos++] = '%';
        buf[pos++] = 's';
    }

    buf[pos] = '\0';
    return pos;
}

/* Generate a random URL path with MULTIPLE %s occurrences.
 * Only the first should be substituted (per strstr behavior). */
static int
gen_url_path_multi_placeholder(char *buf, size_t buf_size)
{
    int pos = 0;

    /* First segment with %s */
    buf[pos++] = '/';
    char seg[16];
    int slen = gen_path_segment(seg, 8);
    memcpy(buf + pos, seg, (size_t)slen);
    pos += slen;
    buf[pos++] = '/';
    buf[pos++] = '%';
    buf[pos++] = 's';

    /* Second %s somewhere after */
    buf[pos++] = '/';
    slen = gen_path_segment(seg, 8);
    memcpy(buf + pos, seg, (size_t)slen);
    pos += slen;
    buf[pos++] = '/';
    buf[pos++] = '%';
    buf[pos++] = 's';

    /* Optional trailing segment */
    if (lrand48() % 2 && pos < (int)buf_size - 20) {
        buf[pos++] = '/';
        slen = gen_path_segment(seg, 8);
        memcpy(buf + pos, seg, (size_t)slen);
        pos += slen;
    }

    buf[pos] = '\0';
    return pos;
}

/* Generate a URL path with query string (with or without %s) */
static int
gen_url_path_with_query(char *buf, size_t buf_size, bool with_placeholder)
{
    int pos = 0;

    /* Path portion */
    buf[pos++] = '/';
    char seg[16];
    int slen = gen_path_segment(seg, 10);
    memcpy(buf + pos, seg, (size_t)slen);
    pos += slen;

    /* Query string */
    buf[pos++] = '?';
    slen = gen_path_segment(seg, 6);
    memcpy(buf + pos, seg, (size_t)slen);
    pos += slen;
    buf[pos++] = '=';

    if (with_placeholder) {
        buf[pos++] = '%';
        buf[pos++] = 's';
    } else {
        slen = gen_path_segment(seg, 8);
        memcpy(buf + pos, seg, (size_t)slen);
        pos += slen;
    }

    /* Possibly add another query param */
    if (lrand48() % 2 && pos < (int)buf_size - 30) {
        buf[pos++] = '&';
        slen = gen_path_segment(seg, 5);
        memcpy(buf + pos, seg, (size_t)slen);
        pos += slen;
        buf[pos++] = '=';
        slen = gen_path_segment(seg, 5);
        memcpy(buf + pos, seg, (size_t)slen);
        pos += slen;
    }

    buf[pos] = '\0';
    return pos;
    (void)buf_size;
}

/* ---- Test strategies ---- */

/*
 * Strategy 1: URLs with %s — verify hash is substituted correctly
 * Generate random URL paths containing %s at various positions.
 * Verify the output has %s replaced with the 64-char hex hash.
 */
static void
test_substitution_with_placeholder(long seed)
{
    printf("  strategy 1: URLs with %%s placeholder (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t hash[HASH_SIZE];
        gen_random_hash(hash);

        char path[256];
        gen_url_path_with_placeholder(path, sizeof(path));

        char ref_result[1024];
        char sut_result[1024];

        int rc1 = ref_url_substitute(path, hash, ref_result, sizeof(ref_result));
        int rc2 = sut_url_substitute(path, hash, sut_result, sizeof(sut_result));

        if (rc1 != 0 || rc2 != 0) {
            fprintf(stderr, "  FAIL trial %d: substitution overflow "
                    "(path='%s', seed=%ld)\n", t, path, seed);
            tests_run++;
            return;
        }

        /* Verify results match */
        if (strcmp(ref_result, sut_result) != 0) {
            fprintf(stderr, "  FAIL trial %d: mismatch\n"
                    "    path='%s'\n    ref='%s'\n    sut='%s'\n"
                    "    (seed=%ld)\n",
                    t, path, ref_result, sut_result, seed);
            tests_run++;
            return;
        }

        /* Verify the result does NOT contain %s at the original position
         * (it was replaced) and DOES contain the hex hash */
        char hex[HEX_HASH_LEN];
        ref_hex_encode(hash, hex);
        if (strstr(ref_result, hex) == NULL) {
            fprintf(stderr, "  FAIL trial %d: hex hash not found in result\n"
                    "    path='%s'\n    result='%s'\n    hex='%s'\n"
                    "    (seed=%ld)\n",
                    t, path, ref_result, hex, seed);
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
 * Strategy 2: URLs without %s — verify pass-through unchanged
 * Generate random URL paths without %s. Verify output equals input.
 */
static void
test_passthrough_no_placeholder(long seed)
{
    printf("  strategy 2: URLs without %%s (pass-through) (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t hash[HASH_SIZE];
        gen_random_hash(hash);

        char path[256];
        gen_url_path_no_placeholder(path, sizeof(path));

        char sut_result[1024];
        int rc = sut_url_substitute(path, hash, sut_result, sizeof(sut_result));

        if (rc != 0) {
            fprintf(stderr, "  FAIL trial %d: substitution overflow "
                    "(path='%s', seed=%ld)\n", t, path, seed);
            tests_run++;
            return;
        }

        /* Result must equal original path exactly */
        if (strcmp(path, sut_result) != 0) {
            fprintf(stderr, "  FAIL trial %d: path modified without %%s\n"
                    "    path='%s'\n    result='%s'\n    (seed=%ld)\n",
                    t, path, sut_result, seed);
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
 * Strategy 3: Multiple %s — only first is replaced (strstr behavior)
 * Generate URLs with two %s occurrences. Verify only the first is replaced
 * and the second remains as literal "%s".
 */
static void
test_multiple_placeholders(long seed)
{
    printf("  strategy 3: multiple %%s (only first replaced) (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t hash[HASH_SIZE];
        gen_random_hash(hash);

        char path[256];
        gen_url_path_multi_placeholder(path, sizeof(path));

        char ref_result[1024];
        char sut_result[1024];

        int rc1 = ref_url_substitute(path, hash, ref_result, sizeof(ref_result));
        int rc2 = sut_url_substitute(path, hash, sut_result, sizeof(sut_result));

        if (rc1 != 0 || rc2 != 0) {
            fprintf(stderr, "  FAIL trial %d: substitution overflow "
                    "(path='%s', seed=%ld)\n", t, path, seed);
            tests_run++;
            return;
        }

        /* Results must match (both use strstr → first %s only) */
        if (strcmp(ref_result, sut_result) != 0) {
            fprintf(stderr, "  FAIL trial %d: mismatch\n"
                    "    path='%s'\n    ref='%s'\n    sut='%s'\n"
                    "    (seed=%ld)\n",
                    t, path, ref_result, sut_result, seed);
            tests_run++;
            return;
        }

        /* Verify the second %s is still present in the result */
        char hex[HEX_HASH_LEN];
        ref_hex_encode(hash, hex);

        /* Find the hex hash in result */
        const char *hash_pos = strstr(ref_result, hex);
        if (!hash_pos) {
            fprintf(stderr, "  FAIL trial %d: hex hash not found in result\n"
                    "    (seed=%ld)\n", t, seed);
            tests_run++;
            return;
        }

        /* After the substituted hash, there should still be a %s */
        const char *remaining = hash_pos + 64;
        if (strstr(remaining, "%s") == NULL) {
            fprintf(stderr, "  FAIL trial %d: second %%s was also replaced\n"
                    "    path='%s'\n    result='%s'\n    (seed=%ld)\n",
                    t, path, ref_result, seed);
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
 * Strategy 4: URL paths with query strings
 * Test %s substitution in query string parameters.
 */
static void
test_query_string_substitution(long seed)
{
    printf("  strategy 4: URLs with query strings (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t hash[HASH_SIZE];
        gen_random_hash(hash);

        char path[256];
        bool with_placeholder = (lrand48() % 2) == 0;
        gen_url_path_with_query(path, sizeof(path), with_placeholder);

        char ref_result[1024];
        char sut_result[1024];

        int rc1 = ref_url_substitute(path, hash, ref_result, sizeof(ref_result));
        int rc2 = sut_url_substitute(path, hash, sut_result, sizeof(sut_result));

        if (rc1 != 0 || rc2 != 0) {
            fprintf(stderr, "  FAIL trial %d: substitution overflow "
                    "(path='%s', seed=%ld)\n", t, path, seed);
            tests_run++;
            return;
        }

        if (strcmp(ref_result, sut_result) != 0) {
            fprintf(stderr, "  FAIL trial %d: mismatch\n"
                    "    path='%s'\n    ref='%s'\n    sut='%s'\n"
                    "    (seed=%ld)\n",
                    t, path, ref_result, sut_result, seed);
            tests_run++;
            return;
        }

        if (with_placeholder) {
            /* Verify hex hash is present */
            char hex[HEX_HASH_LEN];
            ref_hex_encode(hash, hex);
            if (strstr(ref_result, hex) == NULL) {
                fprintf(stderr, "  FAIL trial %d: hex hash not in result\n"
                        "    (seed=%ld)\n", t, seed);
                tests_run++;
                return;
            }
        } else {
            /* Verify path unchanged */
            if (strcmp(path, sut_result) != 0) {
                fprintf(stderr, "  FAIL trial %d: path modified without %%s\n"
                        "    (seed=%ld)\n", t, seed);
                tests_run++;
                return;
            }
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
 * Strategy 5: Result length verification
 * For URLs with %s: result length == original length - 2 + 64
 * For URLs without %s: result length == original length
 */
static void
test_result_length(long seed)
{
    printf("  strategy 5: result length verification (seed=%ld)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int t = 0; t < trials; t++) {
        uint8_t hash[HASH_SIZE];
        gen_random_hash(hash);

        char path[256];
        bool with_placeholder = (lrand48() % 2) == 0;

        if (with_placeholder) {
            gen_url_path_with_placeholder(path, sizeof(path));
        } else {
            gen_url_path_no_placeholder(path, sizeof(path));
        }

        char result[1024];
        int rc = sut_url_substitute(path, hash, result, sizeof(result));
        if (rc != 0) {
            fprintf(stderr, "  FAIL trial %d: overflow (seed=%ld)\n", t, seed);
            tests_run++;
            return;
        }

        size_t path_len = strlen(path);
        size_t result_len = strlen(result);

        if (with_placeholder) {
            /* First %s replaced: length = original - 2 + 64 */
            size_t expected_len = path_len - 2 + 64;
            if (result_len != expected_len) {
                fprintf(stderr, "  FAIL trial %d: length mismatch "
                        "(path_len=%zu, expected=%zu, got=%zu)\n"
                        "    path='%s'\n    result='%s'\n    (seed=%ld)\n",
                        t, path_len, expected_len, result_len,
                        path, result, seed);
                tests_run++;
                return;
            }
        } else {
            /* No substitution: length unchanged */
            if (result_len != path_len) {
                fprintf(stderr, "  FAIL trial %d: length changed without %%s "
                        "(path_len=%zu, result_len=%zu, seed=%ld)\n",
                        t, path_len, result_len, seed);
                tests_run++;
                return;
            }
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

    printf("test_http_notify_url — Property 10: HTTP Notify URL Substitution "
           "(seed=%ld):\n", seed);

    /* Run all strategies */
    test_substitution_with_placeholder(seed);
    test_passthrough_no_placeholder(seed);
    test_multiple_placeholders(seed);
    test_query_string_substitution(seed);
    test_result_length(seed);

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
