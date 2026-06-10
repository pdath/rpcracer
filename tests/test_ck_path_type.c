/* test_ck_path_type.c — Property test for non-string type rejection (Property 3)
 *
 * Property 3: Non-string type rejection
 * For any JSON value that is not of string type (integer, boolean, null,
 * array, or object), if that value is placed in a JSON config as the
 * "ck_notify_socket" value and parsed by config_load, then config_load
 * SHALL return NULL.
 *
 * Validates: Requirements 1.4
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand48/lrand48),
 * 100 trials, seed printed for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/config.h"
#include "../src/log.h"

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

static const char *TMP_PATH = "/tmp/test_rpcrace_ck_type.json";

/* Type categories for non-string JSON values */
enum type_category {
    TYPE_INTEGER = 0,
    TYPE_BOOLEAN = 1,
    TYPE_NULL    = 2,
    TYPE_ARRAY   = 3,
    TYPE_OBJECT  = 4,
    TYPE_COUNT   = 5
};

/* Generate a random non-string JSON value and write it into buf.
 * Returns the number of bytes written (excluding NUL). */
static int
gen_non_string_value(char *buf, size_t buf_size, int category)
{
    int n = 0;

    switch (category) {
    case TYPE_INTEGER: {
        /* Random integer: negative, zero, or positive */
        long val = lrand48() % 200000 - 100000; /* range: -100000 to 99999 */
        n = snprintf(buf, buf_size, "%ld", val);
        break;
    }
    case TYPE_BOOLEAN:
        if (lrand48() % 2 == 0)
            n = snprintf(buf, buf_size, "true");
        else
            n = snprintf(buf, buf_size, "false");
        break;
    case TYPE_NULL:
        n = snprintf(buf, buf_size, "null");
        break;
    case TYPE_ARRAY: {
        /* Random array variants */
        int variant = (int)(lrand48() % 4);
        switch (variant) {
        case 0:
            n = snprintf(buf, buf_size, "[]");
            break;
        case 1:
            n = snprintf(buf, buf_size, "[1, 2, 3]");
            break;
        case 2:
            n = snprintf(buf, buf_size, "[\"x\"]");
            break;
        case 3: {
            int a = (int)(lrand48() % 1000);
            int b = (int)(lrand48() % 1000);
            n = snprintf(buf, buf_size, "[%d, %d]", a, b);
            break;
        }
        }
        break;
    }
    case TYPE_OBJECT: {
        /* Random object variants */
        int variant = (int)(lrand48() % 4);
        switch (variant) {
        case 0:
            n = snprintf(buf, buf_size, "{}");
            break;
        case 1:
            n = snprintf(buf, buf_size, "{\"x\": 1}");
            break;
        case 2:
            n = snprintf(buf, buf_size, "{\"a\": \"b\"}");
            break;
        case 3: {
            int v = (int)(lrand48() % 1000);
            n = snprintf(buf, buf_size, "{\"k\": %d}", v);
            break;
        }
        }
        break;
    }
    }

    return n;
}

/* Write a JSON config file with ck_notify_socket set to a raw (non-string)
 * value. All other required fields are present and valid. */
static int
write_config_with_raw_value(const char *path, const char *raw_value)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f,
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"127.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 9332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 0,\n"
        "  \"ck_notify_socket\": %s\n"
        "}\n",
        raw_value);

    fclose(f);
    return 0;
}

/*
 * Property 3: Non-string type rejection
 *
 * For any JSON value that is not of string type (integer, boolean, null,
 * array, or object), if that value is placed in a JSON config as the
 * "ck_notify_socket" value and parsed by config_load, then config_load
 * SHALL return NULL.
 *
 * Validates: Requirements 1.4
 */
static void
test_property_non_string_type_rejection(long seed)
{
    printf("  property: non-string type rejection (seed=%ld, 100 trials)\n", seed);
    srand48(seed);

    int trials = 100;
    int passed = 0;

    for (int i = 0; i < trials; i++) {
        /* Randomly pick one of 5 type categories */
        int category = (int)(lrand48() % TYPE_COUNT);

        /* Generate the non-string value */
        char value_buf[256];
        gen_non_string_value(value_buf, sizeof(value_buf), category);

        /* Write config file with the non-string value */
        if (write_config_with_raw_value(TMP_PATH, value_buf) != 0) {
            fprintf(stderr, "  FAIL: write_config failed at trial %d\n", i);
            tests_run++;
            return;
        }

        /* Parse with config_load — must return NULL */
        config_t *cfg = config_load(TMP_PATH);
        if (cfg != NULL) {
            fprintf(stderr,
                "  FAIL trial %d: config_load returned non-NULL for "
                "ck_notify_socket = %s (category=%d)\n",
                i, value_buf, category);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, i);
            config_destroy(cfg);
            tests_run++;
            unlink(TMP_PATH);
            return;
        }

        passed++;
    }

    unlink(TMP_PATH);

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    } else {
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

    printf("test_ck_path_type (seed=%ld):\n", seed);

    /* Initialize logging at CRIT level to suppress noise during tests */
    log_init(LOG_CRIT);

    /* Run property test */
    test_property_non_string_type_rejection(seed);

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
