/* test_ck_path_overlen.c — Property test for over-length path rejection (Property 2)
 *
 * Property 2: Over-length path rejection
 * For any string of length 108 or greater, if that string is placed in a
 * JSON config as the "ck_notify_socket" value and parsed by config_load,
 * then config_load SHALL return NULL.
 *
 * Validates: Requirements 1.3
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand48/lrand48),
 * 200 trials, seed printed for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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

static const char *TMP_PATH = "/tmp/test_rpcrace_ck_overlen.json";

/* Generate a random printable ASCII char suitable for JSON string embedding.
 * Range 0x21–0x7E excluding double-quote (0x22) and backslash (0x5C). */
static char
gen_safe_char(void)
{
    /* Printable ASCII 0x21–0x7E = 94 chars, minus 2 excluded = 92 chars */
    static const char charset[] =
        "!#$%&'()*+,-./0123456789:;<=>?@"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`"
        "abcdefghijklmnopqrstuvwxyz{|}~";
    /* charset length = 92 */
    return charset[lrand48() % 92];
}

/* Generate a random string of the given length using safe printable ASCII.
 * buf must have space for at least len+1 bytes. */
static void
gen_random_path(char *buf, int len)
{
    for (int i = 0; i < len; i++) {
        buf[i] = gen_safe_char();
    }
    buf[len] = '\0';
}

/* Write a JSON config file with the given ck_notify_socket value and all
 * required fields. Returns 0 on success, -1 on failure. */
static int
write_config_with_path(const char *path_value, const char *file_path)
{
    FILE *f = fopen(file_path, "w");
    if (!f) return -1;

    fprintf(f,
        "{\n"
        "  \"nodes\": [{\"host\": \"127.0.0.1\", \"rpc_port\": 8332, \"label\": \"test\"}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 0,\n"
        "  \"ck_notify_socket\": \"%s\"\n"
        "}\n",
        path_value);

    fclose(f);
    return 0;
}

/*
 * Property 2: Over-length path rejection
 *
 * For any string of length 108 or greater, if that string is placed in a
 * JSON config as the "ck_notify_socket" value and parsed by config_load,
 * then config_load SHALL return NULL.
 *
 * Validates: Requirements 1.3
 */
static void
test_property_overlen_rejection(long seed)
{
    printf("  property: over-length path rejection (seed=%ld, 200 trials)\n", seed);
    srand48(seed);

    int trials = 200;
    int passed = 0;
    char path_buf[513]; /* max generated length is 512 + NUL */

    for (int i = 0; i < trials; i++) {
        /* Generate random length in [108, 512] */
        int len = 108 + (int)(lrand48() % (512 - 108 + 1));

        /* Generate random string of that length */
        gen_random_path(path_buf, len);

        /* Write config to temp file */
        if (write_config_with_path(path_buf, TMP_PATH) != 0) {
            fprintf(stderr, "  FAIL: could not write config at trial %d\n", i);
            tests_run++;
            return;
        }

        /* Parse with config_load — must return NULL */
        config_t *cfg = config_load(TMP_PATH);
        if (cfg != NULL) {
            fprintf(stderr, "  FAIL trial %d: config_load accepted path of length %d\n",
                    i, len);
            fprintf(stderr, "  (seed=%ld, trial=%d, len=%d)\n", seed, i, len);
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
        printf("    %d/%d trials passed (SOME FAILED)\n", passed, trials);
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

    printf("test_ck_path_overlen (seed=%ld):\n", seed);

    /* Initialize logging at CRIT level to suppress noise during tests */
    log_init(LOG_CRIT);

    /* Run property test */
    test_property_overlen_rejection(seed);

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
