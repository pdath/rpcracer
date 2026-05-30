/* util.c — Clock helpers, hex encoding, buffer utilities */

#include "util.h"
#include <time.h>

uint64_t
clock_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t
clock_realtime_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int
hex_encode(const uint8_t *src, size_t len, char *dst)
{
    static const char hex_chars[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        dst[i * 2]     = hex_chars[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = hex_chars[src[i] & 0x0F];
    }
    dst[len * 2] = '\0';
    return 0;
}

/* Convert a single hex character to its 4-bit value.
 * Returns -1 if the character is not valid hex. */
static int
hex_char_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int
should_log_slow_response(uint64_t elapsed_us)
{
    return elapsed_us > SLOW_RESPONSE_THRESHOLD_US ? 1 : 0;
}

int
hex_decode(const char *src, size_t len, uint8_t *dst)
{
    /* Must be even length */
    if (len == 0 || (len & 1) != 0)
        return -1;

    for (size_t i = 0; i < len; i += 2) {
        int hi = hex_char_val(src[i]);
        int lo = hex_char_val(src[i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        dst[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}
