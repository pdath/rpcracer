/* test_slow_response.c — Property test for slow response threshold warning
 *
 * Property 12: Slow Response Threshold Warning
 * For any RPC response with an elapsed time measurement, a warning shall be
 * logged if and only if the elapsed time exceeds 5 seconds.
 *
 * **Validates: Requirements 9.9**
 *
 * Uses hand-rolled randomized testing: seeded PRNG, 1000 trials, seed printed
 * for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../src/util.h"

#define NUM_TRIALS 1000
#define THRESHOLD_US 5000000ULL  /* 5 seconds in microseconds */

int
main(int argc, char *argv[])
{
    long seed;
    if (argc > 1) {
        seed = atol(argv[1]);
    } else {
        seed = (long)clock_monotonic_ns();
    }
    srand48(seed);
    printf("test_slow_response: seed=%ld\n", seed);

    int failures = 0;

    for (int i = 0; i < NUM_TRIALS; i++) {
        uint64_t elapsed_us;

        /* Generate elapsed times with a distribution that focuses on the
         * boundary region around 5 seconds, plus some far below and far above.
         *
         * Strategy:
         *   - 40% of trials: near the boundary (4.9s to 5.1s)
         *   - 20% of trials: well below threshold (0 to 4s)
         *   - 20% of trials: well above threshold (6s to 60s)
         *   - 10% of trials: exact boundary values (5000000, 4999999, 5000001)
         *   - 10% of trials: extreme values (0, UINT64_MAX, very large)
         */
        double r = drand48();

        if (r < 0.40) {
            /* Near boundary: 4,900,000 to 5,100,000 us */
            elapsed_us = 4900000ULL + (uint64_t)(lrand48() % 200001);
        } else if (r < 0.60) {
            /* Well below: 0 to 4,000,000 us */
            elapsed_us = (uint64_t)(lrand48() % 4000001);
        } else if (r < 0.80) {
            /* Well above: 6,000,000 to 60,000,000 us */
            elapsed_us = 6000000ULL + (uint64_t)(lrand48() % 54000001);
        } else if (r < 0.90) {
            /* Exact boundary values */
            int choice = (int)(lrand48() % 3);
            if (choice == 0)
                elapsed_us = THRESHOLD_US;       /* exactly 5s — should NOT warn */
            else if (choice == 1)
                elapsed_us = THRESHOLD_US - 1;   /* 4.999999s — should NOT warn */
            else
                elapsed_us = THRESHOLD_US + 1;   /* 5.000001s — should warn */
        } else {
            /* Extreme values */
            int choice = (int)(lrand48() % 4);
            if (choice == 0)
                elapsed_us = 0;
            else if (choice == 1)
                elapsed_us = 1;
            else if (choice == 2)
                elapsed_us = UINT64_MAX;
            else
                elapsed_us = UINT64_MAX / 2;
        }

        /* Expected: warning if and only if elapsed > 5s (strictly greater) */
        int expected = (elapsed_us > THRESHOLD_US) ? 1 : 0;
        int actual = should_log_slow_response(elapsed_us);

        if (actual != expected) {
            fprintf(stderr,
                    "  FAIL trial %d: elapsed_us=%llu expected=%d got=%d\n",
                    i, (unsigned long long)elapsed_us, expected, actual);
            failures++;
            if (failures >= 5) {
                fprintf(stderr, "  Too many failures, stopping early\n");
                break;
            }
        }
    }

    if (failures == 0) {
        printf("  PASS: %d trials, all correct (threshold=%llu us)\n",
               NUM_TRIALS, (unsigned long long)THRESHOLD_US);
        return 0;
    } else {
        fprintf(stderr, "  %d/%d trials failed\n", failures, NUM_TRIALS);
        return 1;
    }
}
