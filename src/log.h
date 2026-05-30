/* log.h — Non-blocking stderr logging with microsecond timestamps */

#ifndef LOG_H
#define LOG_H

/* Verbosity levels */
#define LOG_CRIT  0
#define LOG_WARN  1
#define LOG_INFO  2
#define LOG_DEBUG 3

/* Initialize the logging subsystem.
 * Sets stderr to O_NONBLOCK via fcntl and stores the verbosity level.
 * Messages with level > verbosity are suppressed. */
void log_init(int verbosity);

/* Log a formatted message at the given level.
 * Format: <ISO8601 with microseconds>Z [<LEVEL>] [<source>] <message>
 * The source parameter is embedded in fmt by the caller (typically via macro).
 * Uses a single write() call; discards on EAGAIN/EWOULDBLOCK. */
void log_msg(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Cache the current CLOCK_REALTIME timestamp.
 * Called once per epoll_wait dispatch iteration so that all log messages
 * within a single dispatch batch share the same timestamp. */
void log_update_time(void);

#endif /* LOG_H */
