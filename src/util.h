/* util.h — Clock helpers, hex encoding, buffer utilities */

#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Return current CLOCK_MONOTONIC time in nanoseconds.
 * Used for timeout calculations and elapsed time measurements. */
uint64_t clock_monotonic_ns(void);

/* Return current CLOCK_REALTIME time in microseconds.
 * Used for log timestamps. */
uint64_t clock_realtime_us(void);

/* Encode binary bytes to lowercase hex string.
 * dst must have space for at least (len * 2 + 1) bytes (null-terminated).
 * Returns 0 on success. */
int hex_encode(const uint8_t *src, size_t len, char *dst);

/* Decode hex string to binary bytes.
 * src must be an even-length string of hex characters (len = number of hex chars).
 * dst must have space for at least (len / 2) bytes.
 * Returns 0 on success, -1 on invalid input (odd length, non-hex chars). */
int hex_decode(const char *src, size_t len, uint8_t *dst);

/* Slow response threshold in microseconds (5 seconds). */
#define SLOW_RESPONSE_THRESHOLD_US 5000000ULL

/* Determine whether a slow response warning should be logged.
 * Returns true (1) if elapsed_us > 5 seconds, false (0) otherwise. */
int should_log_slow_response(uint64_t elapsed_us);

#endif /* UTIL_H */
