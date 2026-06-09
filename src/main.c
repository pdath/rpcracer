/* main.c — Entry point, signal handling, event loop bootstrap */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "event_loop.h"
#include "rpc_proxy.h"
#include "notifier.h"
#include "http_server.h"
#include "stats.h"
#include "watchdog.h"

#define STALL_THRESHOLD_MS 60000

/* Global event loop pointer for signal handler access */
static event_loop_t *g_loop = NULL;

/* Signal handler: request clean shutdown */
static void
signal_handler(int sig)
{
    (void)sig;
    if (g_loop)
        event_loop_stop(g_loop);
}

/* Notifier callback: relay block notification to the RPC proxy */
static void
on_block_notify(const uint8_t *hash, void *data)
{
    rpc_proxy_on_block_notify((rpc_proxy_t *)data, hash);
}

int
main(int argc, char *argv[])
{
    /* Parse command-line arguments: optional config file path */
    const char *config_path = "rpcrace.conf";
    if (argc > 1)
        config_path = argv[1];

    /* Load configuration */
    config_t *cfg = config_load(config_path);
    if (!cfg) {
        fprintf(stderr, "fatal: failed to load config from '%s'\n", config_path);
        return 1;
    }

    /* Initialize logging */
    log_init(cfg->log_verbosity);
    log_msg(LOG_INFO, "[main] rpcrace starting, config=%s", config_path);

    /* Create event loop */
    event_loop_t *loop = event_loop_create();
    if (!loop) {
        log_msg(LOG_CRIT, "[main] failed to create event loop");
        config_destroy(cfg);
        return 1;
    }
    g_loop = loop;

    /* Enable stall detection (hardcoded 60s threshold) */
    if (event_loop_enable_stall_detection(loop, STALL_THRESHOLD_MS) < 0) {
        log_msg(LOG_WARN, "[main] failed to enable stall detection");
    }

    /* Create RPC proxy (binds listener immediately — Req 14.1) */
    rpc_proxy_t *proxy = rpc_proxy_create(loop, cfg);
    if (!proxy) {
        log_msg(LOG_CRIT, "[main] failed to create rpc_proxy");
        event_loop_destroy(loop);
        config_destroy(cfg);
        return 1;
    }

    /* Create notifier (ZMQ subscriptions + callback wiring) */
    notifier_t *notifier = notifier_create(loop, cfg, on_block_notify, proxy);
    if (!notifier) {
        log_msg(LOG_CRIT, "[main] failed to create notifier");
        rpc_proxy_destroy(proxy);
        event_loop_destroy(loop);
        config_destroy(cfg);
        return 1;
    }

    /* Initialize statistics */
    stats_t *stats = stats_create(cfg->node_count);
    if (!stats) {
        log_msg(LOG_CRIT, "[main] failed to create stats");
        notifier_destroy(notifier);
        rpc_proxy_destroy(proxy);
        event_loop_destroy(loop);
        config_destroy(cfg);
        return 1;
    }

    /* Wire stats into the proxy and notifier for live recording */
    rpc_proxy_set_stats(proxy, stats);
    notifier_set_stats(notifier, stats);
    notifier_set_proxy(notifier, proxy);

    stats_serialize_ctx_t stats_ctx = {
        .stats = stats,
        .cfg = cfg,
        .proxy = proxy
    };

    /* Create HTTP server (notify + stats endpoints) */
    http_server_t *http = http_server_create(loop, cfg, notifier,
                                             stats_serialize_json, &stats_ctx);
    if (!http) {
        log_msg(LOG_CRIT, "[main] failed to create http_server");
        notifier_destroy(notifier);
        rpc_proxy_destroy(proxy);
        event_loop_destroy(loop);
        config_destroy(cfg);
        return 1;
    }

    /* Install signal handlers for clean shutdown (Req 14.2) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Ignore SIGPIPE (broken pipe on TCP writes) */
    signal(SIGPIPE, SIG_IGN);

    /* Initialize systemd watchdog (sends READY=1, starts watchdog timer) */
    if (watchdog_init(loop) < 0) {
        log_msg(LOG_WARN, "[main] watchdog init failed (non-fatal)");
    }

    log_msg(LOG_INFO, "[main] entering event loop");

    /* Run the event loop (blocks until event_loop_stop() is called) */
    event_loop_run(loop);

    /* Clean shutdown */
    log_msg(LOG_INFO, "[main] shutting down");

    watchdog_destroy();
    http_server_destroy(http);
    notifier_destroy(notifier);
    rpc_proxy_destroy(proxy);
    stats_destroy(stats);
    event_loop_destroy(loop);
    config_destroy(cfg);

    return 0;
}
