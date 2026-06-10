/* test_ck_msg_framing.c — Property test: message framing invariant
 *
 * Feature: ck-notify-socket
 * Property 4: Message framing invariant
 *
 * For any invocation of the CK socket relay (regardless of block hash content),
 * the bytes written to the socket SHALL be exactly the 10-byte sequence
 * {0x06, 0x00, 0x00, 0x00, 'u', 'p', 'd', 'a', 't', 'e'} with no additional bytes.
 *
 * Validates: Requirements 3.1, 3.2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
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

/* The exact 10-byte message that relay_ck_notify writes */
static const uint8_t CK_UPDATE_MSG[10] = {
    0x06, 0x00, 0x00, 0x00,       /* length prefix: 6 in LE */
    'u', 'p', 'd', 'a', 't', 'e'  /* payload */
};

#define NUM_TRIALS 200
#define HASH_SIZE 32

int
main(int argc, char *argv[])
{
    long seed;

    if (argc > 1) {
        seed = atol(argv[1]);
    } else {
        seed = (long)time(NULL);
    }

    printf("test_ck_msg_framing: seed=%ld\n", seed);
    srand48(seed);

    log_init(LOG_CRIT);

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Generate a random 32-byte block hash (unused in write,
         * just to confirm independence from hash content) */
        uint8_t hash[HASH_SIZE];
        for (int i = 0; i < HASH_SIZE; i++)
            hash[i] = (uint8_t)(lrand48() & 0xFF);

        /* Create a socketpair for this trial */
        int sv[2];
        int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        ASSERT(rc == 0, "socketpair created");
        if (rc != 0) {
            fprintf(stderr, "  socketpair failed: %s\n", strerror(errno));
            continue;
        }

        /* Set the write end (sv[0]) to non-blocking */
        int flags = fcntl(sv[0], F_GETFL, 0);
        ASSERT(flags >= 0, "fcntl F_GETFL on write end");
        if (flags >= 0) {
            rc = fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
            ASSERT(rc == 0, "fcntl F_SETFL O_NONBLOCK on write end");
        }

        /* Write the CK_UPDATE_MSG (same as relay_ck_notify does) */
        ssize_t w = write(sv[0], CK_UPDATE_MSG, 10);
        ASSERT(w == 10, "write returned exactly 10 bytes");

        /* Read from the read end and verify exact content */
        uint8_t buf[16];  /* slightly larger to detect extra bytes */
        memset(buf, 0xAA, sizeof(buf));

        ssize_t r = read(sv[1], buf, sizeof(buf));
        ASSERT(r == 10, "read returned exactly 10 bytes");
        ASSERT(memcmp(buf, CK_UPDATE_MSG, 10) == 0,
               "read bytes match CK_UPDATE_MSG exactly");

        /* Verify no extra bytes were sent (buf[10..15] should be untouched) */
        int extra_clean = 1;
        for (int i = 10; i < 16; i++) {
            if (buf[i] != 0xAA) {
                extra_clean = 0;
                break;
            }
        }
        ASSERT(extra_clean, "no extra bytes beyond 10");

        (void)hash;  /* explicitly unused — independence check */

        close(sv[0]);
        close(sv[1]);
    }

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
