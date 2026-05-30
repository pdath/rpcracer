/* log.c — Non-blocking stderr logging with microsecond timestamps */

#include "log.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Maximum formatted log line length (including timestamp, tags, message) */
#define LOG_BUF_SIZE 1024

/* Module state */
static int  g_verbosity = LOG_INFO;
static char g_time_str[64]; /* "2025-01-15T14:30:05.123456Z" = 27 chars + NUL */

/* Level tag strings (indexed by level) */
static const char *level_tags[] = {
    "CRIT",
    "WARN",
    "INFO",
    "DEBG"
};

void
log_init(int verbosity)
{
    g_verbosity = verbosity;

    /* Set stderr to non-blocking */
    int flags = fcntl(STDERR_FILENO, F_GETFL);
    if (flags != -1)
        fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK);

    /* Prime the cached timestamp */
    log_update_time();
}

void
log_update_time(void)
{
    uint64_t now_us = clock_realtime_us();
    time_t secs = (time_t)(now_us / 1000000ULL);
    unsigned int usec = (unsigned int)(now_us % 1000000ULL);

    struct tm tm;
    gmtime_r(&secs, &tm);

    /* Format: 2025-01-15T14:30:05.123456Z */
    snprintf(g_time_str, sizeof(g_time_str),
             "%04d-%02d-%02dT%02d:%02d:%02d.%06uZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, usec);
}

void
log_msg(int level, const char *fmt, ...)
{
    /* Suppress messages above configured verbosity */
    if (level > g_verbosity)
        return;

    char buf[LOG_BUF_SIZE];
    int off = 0;

    /* Timestamp + level tag prefix */
    const char *tag = (level >= 0 && level <= LOG_DEBUG)
                      ? level_tags[level] : "????";
    off = snprintf(buf, sizeof(buf), "%s [%s] ", g_time_str, tag);
    if (off < 0 || (size_t)off >= sizeof(buf))
        return;

    /* User message */
    va_list ap;
    va_start(ap, fmt);
    int msg_len = vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    va_end(ap);

    if (msg_len < 0)
        return;

    off += msg_len;
    if ((size_t)off >= sizeof(buf) - 1)
        off = (int)(sizeof(buf) - 2); /* truncate, leave room for newline */

    /* Ensure newline termination */
    buf[off] = '\n';
    off++;

    /* Single write() call — discard on EAGAIN/EWOULDBLOCK */
    ssize_t ret = write(STDERR_FILENO, buf, (size_t)off);
    (void)ret; /* Intentionally ignore: non-blocking, best-effort */
}
