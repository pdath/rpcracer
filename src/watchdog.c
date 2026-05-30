/* watchdog.c — systemd watchdog notification (sd_notify protocol) */

#include "watchdog.h"
#include "log.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Module state */
static int g_notify_fd = -1;

/* Timer callback: send WATCHDOG=1 to the notify socket */
static void
watchdog_timer_cb(event_loop_t *loop, void *data)
{
    (void)loop;
    (void)data;

    if (g_notify_fd < 0)
        return;

    static const char msg[] = "WATCHDOG=1\n";
    ssize_t r = send(g_notify_fd, msg, sizeof(msg) - 1, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log_msg(LOG_WARN, "[watchdog] sendto notify socket failed: %s",
                strerror(errno));
    }
}

/* Send a message to the notify socket (non-blocking) */
static int
notify_send(const char *msg)
{
    if (g_notify_fd < 0)
        return -1;

    size_t len = strlen(msg);
    ssize_t r = send(g_notify_fd, msg, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (r < 0) {
        log_msg(LOG_WARN, "[watchdog] notify send failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int
watchdog_init(event_loop_t *loop)
{
    const char *notify_socket = getenv("NOTIFY_SOCKET");
    if (!notify_socket || notify_socket[0] == '\0') {
        log_msg(LOG_DEBUG, "[watchdog] $NOTIFY_SOCKET not set, watchdog disabled");
        return 0;
    }

    /* Create Unix datagram socket */
    g_notify_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (g_notify_fd < 0) {
        log_msg(LOG_WARN, "[watchdog] socket() failed: %s", strerror(errno));
        return -1;
    }

    /* Build sockaddr_un from $NOTIFY_SOCKET */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (notify_socket[0] == '@') {
        /* Abstract socket: replace '@' with '\0' in sun_path */
        size_t path_len = strlen(notify_socket);
        if (path_len >= sizeof(addr.sun_path)) {
            log_msg(LOG_WARN, "[watchdog] NOTIFY_SOCKET path too long");
            close(g_notify_fd);
            g_notify_fd = -1;
            return -1;
        }
        memcpy(addr.sun_path, notify_socket, path_len);
        addr.sun_path[0] = '\0';  /* abstract socket namespace */

        socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len);
        if (connect(g_notify_fd, (struct sockaddr *)&addr, addr_len) < 0) {
            log_msg(LOG_WARN, "[watchdog] connect to abstract socket failed: %s",
                    strerror(errno));
            close(g_notify_fd);
            g_notify_fd = -1;
            return -1;
        }
    } else {
        /* Filesystem socket path */
        size_t path_len = strlen(notify_socket);
        if (path_len >= sizeof(addr.sun_path)) {
            log_msg(LOG_WARN, "[watchdog] NOTIFY_SOCKET path too long");
            close(g_notify_fd);
            g_notify_fd = -1;
            return -1;
        }
        memcpy(addr.sun_path, notify_socket, path_len + 1);

        if (connect(g_notify_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            log_msg(LOG_WARN, "[watchdog] connect to socket '%s' failed: %s",
                    notify_socket, strerror(errno));
            close(g_notify_fd);
            g_notify_fd = -1;
            return -1;
        }
    }

    log_msg(LOG_INFO, "[watchdog] connected to notify socket: %s", notify_socket);

    /* Send READY=1 to indicate initialization is complete */
    notify_send("READY=1\n");
    log_msg(LOG_INFO, "[watchdog] sent READY=1");

    /* Check $WATCHDOG_USEC for watchdog timer */
    const char *watchdog_usec_str = getenv("WATCHDOG_USEC");
    if (watchdog_usec_str && watchdog_usec_str[0] != '\0') {
        char *endptr = NULL;
        unsigned long long watchdog_usec = strtoull(watchdog_usec_str, &endptr, 10);

        if (endptr == watchdog_usec_str || watchdog_usec == 0) {
            log_msg(LOG_WARN, "[watchdog] invalid WATCHDOG_USEC value: '%s'",
                    watchdog_usec_str);
            return 0;  /* Socket is connected, just no timer */
        }

        /* Timer interval = WatchdogSec / 2 = WATCHDOG_USEC / 2 / 1000000 seconds
         * Convert to milliseconds: WATCHDOG_USEC / 2 / 1000 */
        uint64_t interval_ms = (uint64_t)(watchdog_usec / 2000ULL);
        if (interval_ms == 0)
            interval_ms = 1;

        int tfd = event_loop_add_timer(loop, interval_ms, watchdog_timer_cb, NULL);
        if (tfd < 0) {
            log_msg(LOG_WARN, "[watchdog] failed to create watchdog timer");
            return -1;
        }

        log_msg(LOG_INFO, "[watchdog] watchdog timer started: interval=%llu ms "
                "(WATCHDOG_USEC=%llu)",
                (unsigned long long)interval_ms,
                (unsigned long long)watchdog_usec);
    } else {
        log_msg(LOG_DEBUG, "[watchdog] $WATCHDOG_USEC not set, no watchdog timer");
    }

    return 0;
}

void
watchdog_destroy(void)
{
    if (g_notify_fd >= 0) {
        close(g_notify_fd);
        g_notify_fd = -1;
    }
}
