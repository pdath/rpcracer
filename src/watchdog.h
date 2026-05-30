/* watchdog.h — systemd watchdog notification (sd_notify protocol) */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "event_loop.h"

/*
 * Initialize systemd watchdog integration.
 *
 * Checks $NOTIFY_SOCKET and $WATCHDOG_USEC environment variables.
 * If $NOTIFY_SOCKET is set:
 *   - Creates a Unix datagram socket connected to the notify socket
 *   - Sends "READY=1\n" immediately (call after all subsystems are initialized)
 * If $WATCHDOG_USEC is also set:
 *   - Registers a recurring timer at WatchdogSec/2 interval that sends
 *     "WATCHDOG=1\n" to the notify socket (non-blocking)
 *
 * Returns 0 on success (or if watchdog is not configured — no-op).
 * Returns -1 on error (socket creation failure, etc.).
 */
int watchdog_init(event_loop_t *loop);

/*
 * Clean up watchdog resources (close socket).
 * Safe to call even if watchdog was not initialized.
 */
void watchdog_destroy(void);

#endif /* WATCHDOG_H */
