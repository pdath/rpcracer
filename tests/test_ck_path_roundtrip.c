/* test_ck_path_roundtrip.c — Property test for CK socket path round-trip (Property 1)
 *
 * Property 1: Valid socket path round-trip
 * For any string of length 1 to 107 containing valid path characters
 * (printable ASCII 0x21-0x7E excluding backslash and double-quote),
 * if that string is placed in a JSON config as the "ck_notify_socket"
 * value and parsed by config_load, then the resulting
 * config_t.ck_notify_socket field SHALL contain exactly that string
 * (byte-for-byte equal).
 *
 * Validates: Requirements 1.1
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand48/lrand48),
 * 500 trials, seed printed for reproducibility, seed accepted via argv[1].
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

static const char *TMP_PATH = "/tmp/test_rpcrace_ck_path_rt.json";

/* Valid path character set: printable ASCII 0x21-0x7E excluding
 * backslash (0x5C) and double-quote (0x22) since these break JSON strings. */
static const char VALID_CHARS[] =
    "!#$%&'()*+,-./0123456789:;<=>?@"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`"
    "abcdefghijklmnopqrstuvwxyz{|}~";

/* Generate a random path string of given length using valid path characters */
static void
gen_random_path(char *buf, int len)
{
    int charset_len = (int)(sizeof(VALID_CHARS) - 1);
    for (int i = 0; i < len; i++) {
        buf[i] = VALID_CHARS[lrand48() % charset_len];
    }
    buf[len] = '\0';
}

/*
 * Property 1: Valid socket path round-trip
 *
 * For any string of length 1 to 107 containing valid path characters,
 * serializing as JSON config with ck_notify_socket field and parsing with
 * config_load shall produce an identical ck_notify_socket value.
 *
 * Validates: Requirements 1.1
 */
int
main(int argc, char *argv[])
{
    long seed;
    if (argc > 1) {
        seed = atol(argv[1]);
    } else {
        seed = (long)time(NULL);
    }

    printf("test_ck_path_roundtrip (seed=%ld):\n", seed);

    log_init(LOG_CRIT);
    srand48(seed);

    for (int i = 0; i < 500; i++) {
        /* Generate random path length 1-107 */
        int len = 1 + (int)(lrand48() % 107);
        char path[108];
        gen_random_path(path, len);

        /* Build full valid JSON config string with snprintf */
        char json[1024];
        snprintf(json, sizeof(json),
            "{\"nodes\":[{\"label\":\"n1\",\"host\":\"10.0.0.1\",\"rpc_port\":8332}],"
            "\"rpc_server_bind\":\"127.0.0.1\",\"rpc_server_port\":8332,"
            "\"http_server_bind\":\"0.0.0.0\",\"http_server_port\":7152,"
            "\"rpc_timeout_ms\":5000,\"log_verbosity\":2,"
            "\"ck_notify_socket\":\"%s\"}", path);

        /* Write to tmp file */
        FILE *fp = fopen(TMP_PATH, "w");
        if (!fp) {
            fprintf(stderr, "  FAIL: cannot create temp file at trial %d\n", i);
            tests_run++;
            return 1;
        }
        fputs(json, fp);
        fclose(fp);

        /* Parse with config_load */
        config_t *cfg = config_load(TMP_PATH);
        if (!cfg) {
            fprintf(stderr, "  FAIL: config_load returned NULL at trial %d\n", i);
            fprintf(stderr, "    path was: \"%s\" (len=%d)\n", path, len);
            fprintf(stderr, "    (seed=%ld, trial=%d)\n", seed, i);
            tests_run++;
            unlink(TMP_PATH);
            return 1;
        }

        /* Assert byte-for-byte equality */
        char msg[128];
        snprintf(msg, sizeof(msg), "trial %d: round-trip match (len=%d)", i, len);
        ASSERT(strcmp(cfg->ck_notify_socket, path) == 0, msg);

        if (strcmp(cfg->ck_notify_socket, path) != 0) {
            fprintf(stderr, "    expected: \"%s\"\n", path);
            fprintf(stderr, "    got:      \"%s\"\n", cfg->ck_notify_socket);
            fprintf(stderr, "    (seed=%ld, trial=%d)\n", seed, i);
            config_destroy(cfg);
            unlink(TMP_PATH);
            return 1;
        }

        config_destroy(cfg);
        unlink(TMP_PATH);
    }

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
