/* test_util.c — Unit tests for util.c (clock helpers, hex encoding) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../src/util.h"

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

static void
test_clock_monotonic_ns(void)
{
    printf("  test_clock_monotonic_ns\n");

    uint64_t t1 = clock_monotonic_ns();
    /* Busy-wait a tiny bit */
    volatile int x = 0;
    for (int i = 0; i < 10000; i++) x += i;
    (void)x;
    uint64_t t2 = clock_monotonic_ns();

    ASSERT(t1 > 0, "clock_monotonic_ns returns non-zero");
    ASSERT(t2 >= t1, "clock_monotonic_ns is non-decreasing");
    ASSERT(t2 - t1 < 1000000000ULL, "elapsed time is reasonable (< 1s)");
}

static void
test_clock_realtime_us(void)
{
    printf("  test_clock_realtime_us\n");

    uint64_t t1 = clock_realtime_us();

    /* Sanity check: should be after 2020-01-01 in microseconds */
    /* 2020-01-01 00:00:00 UTC = 1577836800 seconds = 1577836800000000 us */
    ASSERT(t1 > 1577836800000000ULL, "clock_realtime_us is after 2020");

    uint64_t t2 = clock_realtime_us();
    ASSERT(t2 >= t1, "clock_realtime_us is non-decreasing");
}

static void
test_hex_encode_basic(void)
{
    printf("  test_hex_encode_basic\n");

    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    char hex[9];

    int ret = hex_encode(data, 4, hex);
    ASSERT(ret == 0, "hex_encode returns 0");
    ASSERT(strcmp(hex, "deadbeef") == 0, "hex_encode produces correct output");
}

static void
test_hex_encode_empty(void)
{
    printf("  test_hex_encode_empty\n");

    char hex[1];
    int ret = hex_encode(NULL, 0, hex);
    ASSERT(ret == 0, "hex_encode with len=0 returns 0");
    ASSERT(hex[0] == '\0', "hex_encode with len=0 produces empty string");
}

static void
test_hex_encode_block_hash(void)
{
    printf("  test_hex_encode_block_hash\n");

    /* Simulate a 32-byte block hash */
    uint8_t hash[32];
    for (int i = 0; i < 32; i++) hash[i] = (uint8_t)i;

    char hex[65];
    int ret = hex_encode(hash, 32, hex);
    ASSERT(ret == 0, "hex_encode 32 bytes returns 0");
    ASSERT(strlen(hex) == 64, "hex_encode 32 bytes produces 64-char string");
    ASSERT(strncmp(hex, "000102030405", 12) == 0, "hex_encode first bytes correct");
}

static void
test_hex_decode_basic(void)
{
    printf("  test_hex_decode_basic\n");

    uint8_t out[4];
    int ret = hex_decode("deadbeef", 8, out);
    ASSERT(ret == 0, "hex_decode returns 0");
    ASSERT(out[0] == 0xDE, "hex_decode byte 0 correct");
    ASSERT(out[1] == 0xAD, "hex_decode byte 1 correct");
    ASSERT(out[2] == 0xBE, "hex_decode byte 2 correct");
    ASSERT(out[3] == 0xEF, "hex_decode byte 3 correct");
}

static void
test_hex_decode_uppercase(void)
{
    printf("  test_hex_decode_uppercase\n");

    uint8_t out[4];
    int ret = hex_decode("DEADBEEF", 8, out);
    ASSERT(ret == 0, "hex_decode uppercase returns 0");
    ASSERT(out[0] == 0xDE, "hex_decode uppercase byte 0 correct");
    ASSERT(out[3] == 0xEF, "hex_decode uppercase byte 3 correct");
}

static void
test_hex_decode_mixed_case(void)
{
    printf("  test_hex_decode_mixed_case\n");

    uint8_t out[4];
    int ret = hex_decode("DeAdBeEf", 8, out);
    ASSERT(ret == 0, "hex_decode mixed case returns 0");
    ASSERT(out[0] == 0xDE, "hex_decode mixed byte 0 correct");
    ASSERT(out[3] == 0xEF, "hex_decode mixed byte 3 correct");
}

static void
test_hex_decode_invalid_odd_length(void)
{
    printf("  test_hex_decode_invalid_odd_length\n");

    uint8_t out[4];
    int ret = hex_decode("deadbee", 7, out);
    ASSERT(ret == -1, "hex_decode odd length returns -1");
}

static void
test_hex_decode_invalid_chars(void)
{
    printf("  test_hex_decode_invalid_chars\n");

    uint8_t out[4];
    int ret = hex_decode("deadbexf", 8, out);
    ASSERT(ret == -1, "hex_decode invalid char returns -1");

    ret = hex_decode("zz112233", 8, out);
    ASSERT(ret == -1, "hex_decode 'z' char returns -1");

    ret = hex_decode("dead be", 7, out);
    ASSERT(ret == -1, "hex_decode space char returns -1");
}

static void
test_hex_decode_empty(void)
{
    printf("  test_hex_decode_empty\n");

    uint8_t out[1];
    int ret = hex_decode("", 0, out);
    ASSERT(ret == -1, "hex_decode empty string returns -1");
}

static void
test_hex_roundtrip(void)
{
    printf("  test_hex_roundtrip\n");

    /* 32-byte block hash roundtrip */
    uint8_t original[32];
    for (int i = 0; i < 32; i++) original[i] = (uint8_t)(i * 7 + 13);

    char hex[65];
    hex_encode(original, 32, hex);

    uint8_t decoded[32];
    int ret = hex_decode(hex, 64, decoded);
    ASSERT(ret == 0, "hex_decode roundtrip returns 0");
    ASSERT(memcmp(original, decoded, 32) == 0, "hex roundtrip preserves data");
}

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("test_util:\n");

    test_clock_monotonic_ns();
    test_clock_realtime_us();
    test_hex_encode_basic();
    test_hex_encode_empty();
    test_hex_encode_block_hash();
    test_hex_decode_basic();
    test_hex_decode_uppercase();
    test_hex_decode_mixed_case();
    test_hex_decode_invalid_odd_length();
    test_hex_decode_invalid_chars();
    test_hex_decode_empty();
    test_hex_roundtrip();

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
