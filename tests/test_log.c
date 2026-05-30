/* test_log.c — Property test for log message format (Property 11)
 *
 * Property 11: Log Message Format
 * For any log message at any verbosity level with any source identifier,
 * the formatted output shall contain: (1) a valid ISO 8601 timestamp with
 * microsecond precision, (2) the log level tag, and (3) the source identifier.
 *
 * Validates: Requirements 9.3, 9.4
 *
 * Uses hand-rolled randomized testing: seeded PRNG, 1000 trials,
 * seed printed for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

#include "../src/log.h"
#include "../src/util.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stdout, "  FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while (0)

/* Level tag strings matching log.c */
static const char *level_tags[] = { "CRIT", "WARN", "INFO", "DEBG" };

/* Generate a random printable string of length [1, max_len] into buf.
 * Avoids '%' to prevent printf format issues, and avoids newlines. */
static int
gen_random_string(char *buf, int max_len)
{
    int len = 1 + (int)(lrand48() % (long)max_len);
    for (int i = 0; i < len; i++) {
        /* Printable ASCII range 32-126, skip '%' (37) and '\n' */
        char c;
        do {
            c = (char)(32 + (int)(lrand48() % 95));
        } while (c == '%' || c == '\n' || c == '\r');
        buf[i] = c;
    }
    buf[len] = '\0';
    return len;
}

/* Generate a random source identifier (alphanumeric + dots/dashes, like an IP or label) */
static void
gen_random_source(char *buf, int max_len)
{
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789.-_";
    int len = 1 + (int)(lrand48() % (long)max_len);
    for (int i = 0; i < len; i++) {
        buf[i] = charset[lrand48() % (sizeof(charset) - 1)];
    }
    buf[len] = '\0';
}

/* Validate ISO 8601 timestamp with microsecond precision.
 * Expected format: YYYY-MM-DDTHH:MM:SS.ffffffZ (27 chars)
 * Returns 1 if valid, 0 otherwise. */
static int
validate_iso8601_timestamp(const char *ts)
{
    /* Must be exactly 27 characters: 2025-01-15T14:30:05.123456Z */
    if (strlen(ts) < 27)
        return 0;

    /* Check structure: YYYY-MM-DDTHH:MM:SS.ffffffZ */
    /* Positions:       0123456789012345678901234567 */
    if (ts[4] != '-' || ts[7] != '-' || ts[10] != 'T' ||
        ts[13] != ':' || ts[16] != ':' || ts[19] != '.' || ts[26] != 'Z')
        return 0;

    /* Check all digit positions */
    for (int i = 0; i < 4; i++)
        if (!isdigit((unsigned char)ts[i])) return 0;
    for (int i = 5; i < 7; i++)
        if (!isdigit((unsigned char)ts[i])) return 0;
    for (int i = 8; i < 10; i++)
        if (!isdigit((unsigned char)ts[i])) return 0;
    for (int i = 11; i < 13; i++)
        if (!isdigit((unsigned char)ts[i])) return 0;
    for (int i = 14; i < 16; i++)
        if (!isdigit((unsigned char)ts[i])) return 0;
    for (int i = 17; i < 19; i++)
        if (!isdigit((unsigned char)ts[i])) return 0;
    /* Microseconds: 6 digits after the dot */
    for (int i = 20; i < 26; i++)
        if (!isdigit((unsigned char)ts[i])) return 0;

    /* Validate ranges */
    int year  = atoi(ts);
    int month = atoi(ts + 5);
    int day   = atoi(ts + 8);
    int hour  = atoi(ts + 11);
    int min   = atoi(ts + 14);
    int sec   = atoi(ts + 17);

    if (year < 1970 || year > 2100) return 0;
    if (month < 1 || month > 12) return 0;
    if (day < 1 || day > 31) return 0;
    if (hour < 0 || hour > 23) return 0;
    if (min < 0 || min > 59) return 0;
    if (sec < 0 || sec > 60) return 0; /* 60 for leap second */

    return 1;
}

/* Redirect stderr to a pipe and return the read end fd.
 * The original stderr fd is saved to *saved_stderr. */
static int
redirect_stderr_to_pipe(int *saved_stderr)
{
    int pipefd[2];
    if (pipe(pipefd) == -1)
        return -1;

    /* Save original stderr */
    *saved_stderr = dup(STDERR_FILENO);
    if (*saved_stderr == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /* Redirect stderr to write end of pipe */
    if (dup2(pipefd[1], STDERR_FILENO) == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(*saved_stderr);
        return -1;
    }
    close(pipefd[1]);

    /* Set the pipe read end to non-blocking for safe reads */
    int flags = fcntl(pipefd[0], F_GETFL);
    if (flags != -1)
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    /* stderr itself must be non-blocking for log_msg to work */
    flags = fcntl(STDERR_FILENO, F_GETFL);
    if (flags != -1)
        fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK);

    return pipefd[0];
}

/* Restore stderr from saved fd and close the pipe read end */
static void
restore_stderr(int saved_stderr, int pipe_read_fd)
{
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    close(pipe_read_fd);
}

/* Read all available data from pipe_read_fd into buf (up to buf_size-1).
 * Returns number of bytes read. */
static int
read_pipe(int pipe_read_fd, char *buf, int buf_size)
{
    int total = 0;
    while (total < buf_size - 1) {
        ssize_t n = read(pipe_read_fd, buf + total, (size_t)(buf_size - 1 - total));
        if (n <= 0)
            break;
        total += (int)n;
    }
    buf[total] = '\0';
    return total;
}

/*
 * Property 11: Log Message Format
 *
 * For any log message at any verbosity level with any source identifier,
 * the formatted output shall contain:
 *   (1) a valid ISO 8601 timestamp with microsecond precision
 *   (2) the log level tag
 *   (3) the source identifier
 */
static void
test_property_log_message_format(long seed)
{
    printf("  property: log message format (seed=%ld, 1000 trials)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int i = 0; i < trials; i++) {
        /* Generate random level 0–3 */
        int level = (int)(lrand48() % 4);

        /* Generate random source identifier */
        char source[32];
        gen_random_source(source, 20);

        /* Generate random message body */
        char message[64];
        gen_random_string(message, 50);

        /* Redirect stderr to capture log output */
        int saved_stderr;
        int pipe_fd = redirect_stderr_to_pipe(&saved_stderr);
        if (pipe_fd == -1) {
            fprintf(stdout, "  FAIL: could not redirect stderr at trial %d\n", i);
            tests_run++;
            return;
        }

        /* Update cached timestamp and emit log message */
        log_update_time();
        log_msg(level, "[%s] %s", source, message);

        /* Read captured output */
        char output[2048];
        int out_len = read_pipe(pipe_fd, output, (int)sizeof(output));

        /* Restore stderr */
        restore_stderr(saved_stderr, pipe_fd);

        /* Verify output was produced */
        if (out_len == 0) {
            fprintf(stdout, "  FAIL: no output at trial %d (level=%d)\n", i, level);
            tests_run++;
            return;
        }

        /* (1) Verify valid ISO 8601 timestamp with microsecond precision */
        /* The timestamp is at the start of the line, 27 chars */
        if (out_len < 27) {
            fprintf(stdout, "  FAIL: output too short for timestamp at trial %d\n", i);
            tests_run++;
            return;
        }

        char ts_buf[28];
        memcpy(ts_buf, output, 27);
        ts_buf[27] = '\0';

        if (!validate_iso8601_timestamp(ts_buf)) {
            fprintf(stdout, "  FAIL: invalid ISO 8601 timestamp '%s' at trial %d\n",
                    ts_buf, i);
            tests_run++;
            return;
        }

        /* (2) Verify level tag is present */
        const char *tag = level_tags[level];
        char tag_bracket[8];
        snprintf(tag_bracket, sizeof(tag_bracket), "[%s]", tag);

        if (strstr(output, tag_bracket) == NULL) {
            fprintf(stdout, "  FAIL: level tag '%s' not found at trial %d\n"
                    "  output: %s", tag_bracket, i, output);
            tests_run++;
            return;
        }

        /* (3) Verify source identifier is present in output */
        char source_bracket[64];
        snprintf(source_bracket, sizeof(source_bracket), "[%s]", source);

        if (strstr(output, source_bracket) == NULL) {
            fprintf(stdout, "  FAIL: source '%s' not found at trial %d\n"
                    "  output: %s", source_bracket, i, output);
            tests_run++;
            return;
        }

        passed++;
    }

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

    printf("test_log (seed=%ld):\n", seed);

    /* Initialize logging at DEBUG level so all messages are emitted */
    log_init(LOG_DEBUG);

    /* Run property test */
    test_property_log_message_format(seed);

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
