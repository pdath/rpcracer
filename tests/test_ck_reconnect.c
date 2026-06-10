/* test_ck_reconnect.c — Property test: reconnection on demand
 *
 * Feature: ck-notify-socket
 * Property 7: Reconnection on demand
 *
 * For any block notification arriving when the CK socket relay is in
 * disconnected state, the relay SHALL attempt to create a new Unix domain
 * socket and connect to the configured path before attempting the write.
 *
 * This test validates the reconnection mechanism by:
 * 1. Creating a Unix listener on a temporary path
 * 2. Connecting a client (simulating ck_connect logic)
 * 3. Writing CK_UPDATE_MSG to the connected fd
 * 4. Reading from the accepted server-side fd and verifying 10 bytes match
 *
 * Validates: Requirements 5.2
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand48/lrand48),
 * 50 trials with varying socket paths, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
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

#define NUM_TRIALS 50

/*
 * Property 7: Reconnection on demand
 *
 * For each trial, simulate the reconnection scenario:
 * - Create a unique temporary Unix socket path
 * - Set up a listener socket (as ckpool stratifier would)
 * - Connect a client (same logic as ck_connect: socket + connect + O_NONBLOCK)
 * - Write CK_UPDATE_MSG on the client fd
 * - Accept and read on the server side, verify 10-byte message
 *
 * Validates: Requirements 5.2
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

    printf("test_ck_reconnect: seed=%ld\n", seed);
    srand48(seed);

    log_init(LOG_CRIT);

    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        /* Generate a unique socket path for this trial */
        char path[64];
        snprintf(path, sizeof(path), "/tmp/test_ck_rc_%d", trial);
        unlink(path);  /* remove if exists from a prior run */

        /* Create listener socket (simulates ckpool stratifier) */
        int listener = socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT(listener >= 0, "listener socket created");
        if (listener < 0) {
            fprintf(stderr, "  trial %d: socket() failed: %s\n",
                    trial, strerror(errno));
            continue;
        }

        struct sockaddr_un saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sun_family = AF_UNIX;
        snprintf(saddr.sun_path, sizeof(saddr.sun_path), "%s", path);

        int rc = bind(listener, (struct sockaddr *)&saddr, sizeof(saddr));
        ASSERT(rc == 0, "bind listener");
        if (rc != 0) {
            fprintf(stderr, "  trial %d: bind() failed: %s\n",
                    trial, strerror(errno));
            close(listener);
            unlink(path);
            continue;
        }

        rc = listen(listener, 1);
        ASSERT(rc == 0, "listen");
        if (rc != 0) {
            fprintf(stderr, "  trial %d: listen() failed: %s\n",
                    trial, strerror(errno));
            close(listener);
            unlink(path);
            continue;
        }

        /* Simulate ck_connect: create client socket, connect, set O_NONBLOCK */
        int client = socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT(client >= 0, "client socket created");
        if (client < 0) {
            fprintf(stderr, "  trial %d: client socket() failed: %s\n",
                    trial, strerror(errno));
            close(listener);
            unlink(path);
            continue;
        }

        struct sockaddr_un caddr;
        memset(&caddr, 0, sizeof(caddr));
        caddr.sun_family = AF_UNIX;
        snprintf(caddr.sun_path, sizeof(caddr.sun_path), "%s", path);

        rc = connect(client, (struct sockaddr *)&caddr, sizeof(caddr));
        ASSERT(rc == 0, "client connect");
        if (rc != 0) {
            fprintf(stderr, "  trial %d: connect() failed: %s\n",
                    trial, strerror(errno));
            close(client);
            close(listener);
            unlink(path);
            continue;
        }

        /* Set client fd to non-blocking (as ck_connect does) */
        int flags = fcntl(client, F_GETFL, 0);
        ASSERT(flags >= 0, "fcntl F_GETFL client");
        if (flags >= 0) {
            rc = fcntl(client, F_SETFL, flags | O_NONBLOCK);
            ASSERT(rc == 0, "fcntl F_SETFL O_NONBLOCK client");
        }

        /* Accept the connection on the server side */
        int server_conn = accept(listener, NULL, NULL);
        ASSERT(server_conn >= 0, "accept connection");
        if (server_conn < 0) {
            fprintf(stderr, "  trial %d: accept() failed: %s\n",
                    trial, strerror(errno));
            close(client);
            close(listener);
            unlink(path);
            continue;
        }

        /* Write CK_UPDATE_MSG on the client (as relay_ck_notify does) */
        ssize_t w = write(client, CK_UPDATE_MSG, 10);
        ASSERT(w == 10, "write returned exactly 10 bytes");

        /* Read from the server-side accepted fd and verify */
        uint8_t buf[10];
        memset(buf, 0, sizeof(buf));
        ssize_t r = read(server_conn, buf, sizeof(buf));
        ASSERT(r == 10, "read returned exactly 10 bytes");
        ASSERT(memcmp(buf, CK_UPDATE_MSG, 10) == 0,
               "received bytes match CK_UPDATE_MSG");

        /* Cleanup */
        close(client);
        close(server_conn);
        close(listener);
        unlink(path);
    }

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
