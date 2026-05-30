/* rpc_proxy.c — RPC proxy: client connection, request parsing, race dispatch */

#include "rpc_proxy.h"
#include "rpc_conn.h"
#include "log.h"
#include "util.h"

#include "yyjson.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* ---- Race state ---- */
typedef enum {
    RACE_IDLE,
    RACE_FANOUT,
    RACE_STICKY
} race_state_t;

/* ---- Internal proxy structure ---- */
struct rpc_proxy {
    /* Event loop and config references */
    event_loop_t *loop;
    config_t *cfg;

    /* Statistics (optional, set via rpc_proxy_set_stats) */
    stats_t *stats;

    /* Listener socket */
    int listen_fd;

    /* Race state */
    race_state_t state;
    int sticky_node_idx;        /* index into node array, -1 if none */
    int64_t last_block_height;  /* last known block height */
    bool notify_pending;        /* true after notify, cleared after first GBT race */
    uint64_t last_notify_ns;    /* CLOCK_MONOTONIC timestamp of last block notify */

    /* Active race tracking */
    int responses_pending;      /* count of nodes still in-flight */
    int winner_idx;             /* index of winning node, -1 if none yet */
    int last_error_idx;         /* index of last error response, for fallback */
    bool all_must_complete;     /* true for submitblock/sendrawtransaction */

    /* GBT height race tracking */
    int64_t best_gbt_height;    /* highest height seen so far in this GBT race */
    int best_gbt_node_idx;      /* node index with best height, -1 if none */
    bool gbt_height_matched;    /* true if exact height match found */

    /* Client connection */
    int client_fd;
    uint8_t *client_recv_buf;   /* pre-allocated at startup, SOCKET_BUF_SIZE */
    size_t client_recv_len;
    size_t client_recv_cap;
    bool client_connected;

    /* Parsed request info */
    char method[128];           /* extracted JSON-RPC method name */

    /* Upstream connections */
    upstream_conn_t upstreams[MAX_NODES];
    int upstream_count;

    /* RPC timeout timer (one-shot timerfd, -1 when inactive) */
    int rpc_timeout_timer_fd;
};

/* ---- Method routing strategies ---- */
typedef enum {
    ROUTE_RACE,         /* Fan-out to all, first success wins */
    ROUTE_BROADCAST,    /* Fan-out to all, all must complete */
    ROUTE_STICKY        /* Send to sticky node only */
} route_strategy_t;

/* ---- Forward declarations ---- */
static void listener_cb(event_loop_t *loop, int fd, uint32_t events, void *data);
static void client_cb(event_loop_t *loop, int fd, uint32_t events, void *data);
static void close_client(rpc_proxy_t *proxy);
static int parse_http_request(rpc_proxy_t *proxy);
static int extract_json_rpc_method(rpc_proxy_t *proxy);
static route_strategy_t classify_method(rpc_proxy_t *proxy);
static int dispatch_fanout(rpc_proxy_t *proxy);
static int dispatch_sticky(rpc_proxy_t *proxy);
static void send_rpc_error_to_client(rpc_proxy_t *proxy, int code,
                                     const char *message);

/* Race response handling callbacks */
static void on_upstream_response(upstream_conn_t *conn, void *data);
static void on_upstream_error(upstream_conn_t *conn, void *data);
static int response_is_error(const upstream_conn_t *conn);
static int64_t parse_gbt_height(const upstream_conn_t *conn);
static void send_upstream_response_to_client(rpc_proxy_t *proxy,
                                             upstream_conn_t *conn);
static void race_complete(rpc_proxy_t *proxy);

/* RPC timeout helpers */
static void rpc_timeout_cb(event_loop_t *loop, int fd, uint32_t events,
                           void *data);
static void start_rpc_timeout(rpc_proxy_t *proxy);
static void cancel_rpc_timeout(rpc_proxy_t *proxy);

/* ---- Listener setup ---- */

static int
setup_listener(rpc_proxy_t *proxy)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_msg(LOG_CRIT, "[rpc_proxy] socket() failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_msg(LOG_WARN, "[rpc_proxy] setsockopt SO_REUSEADDR failed: %s",
                strerror(errno));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(proxy->cfg->rpc_server_port);

    if (inet_pton(AF_INET, proxy->cfg->rpc_server_bind, &addr.sin_addr) != 1) {
        log_msg(LOG_CRIT, "[rpc_proxy] Invalid bind address: %s",
                proxy->cfg->rpc_server_bind);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg(LOG_CRIT, "[rpc_proxy] bind(%s:%u) failed: %s",
                proxy->cfg->rpc_server_bind, proxy->cfg->rpc_server_port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        log_msg(LOG_CRIT, "[rpc_proxy] listen() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (event_loop_add_fd(proxy->loop, fd, EPOLLIN, listener_cb, proxy) < 0) {
        log_msg(LOG_CRIT, "[rpc_proxy] event_loop_add_fd for listener failed");
        close(fd);
        return -1;
    }

    proxy->listen_fd = fd;
    log_msg(LOG_INFO, "[rpc_proxy] Listening on %s:%u",
            proxy->cfg->rpc_server_bind, proxy->cfg->rpc_server_port);

    return 0;
}

/* ---- Listener callback ---- */

static void
listener_cb(event_loop_t *loop, int fd, uint32_t events, void *data)
{
    (void)loop;
    (void)fd;
    (void)events;

    rpc_proxy_t *proxy = (rpc_proxy_t *)data;

    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);

    int client_fd = accept4(proxy->listen_fd, (struct sockaddr *)&peer_addr,
                            &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            log_msg(LOG_WARN, "[rpc_proxy] accept4() failed: %s",
                    strerror(errno));
        return;
    }

    char peer_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip));

    /* Req 13.1: If a client is already connected, drop it and accept the new one */
    if (proxy->client_connected) {
        log_msg(LOG_INFO, "[rpc_proxy] New client from %s:%u — dropping existing client",
                peer_ip, ntohs(peer_addr.sin_port));
        close_client(proxy);
    } else {
        log_msg(LOG_INFO, "[rpc_proxy] Client connected from %s:%u",
                peer_ip, ntohs(peer_addr.sin_port));
    }

    /* Configure client socket */
    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    /* Register with event loop */
    if (event_loop_add_fd(proxy->loop, client_fd, EPOLLIN,
                          client_cb, proxy) < 0) {
        log_msg(LOG_WARN, "[rpc_proxy] event_loop_add_fd for client failed");
        close(client_fd);
        return;
    }

    proxy->client_fd = client_fd;
    proxy->client_connected = true;
    proxy->client_recv_len = 0;
}

/* ---- Client connection management ---- */

static void
close_client(rpc_proxy_t *proxy)
{
    if (!proxy->client_connected)
        return;

    event_loop_del_fd(proxy->loop, proxy->client_fd);
    close(proxy->client_fd);
    proxy->client_fd = -1;
    proxy->client_connected = false;
    proxy->client_recv_len = 0;

    log_msg(LOG_DEBUG, "[rpc_proxy] Client disconnected");
}

/* ---- Client data callback ---- */

static void
client_cb(event_loop_t *loop, int fd, uint32_t events, void *data)
{
    (void)loop;
    (void)fd;

    rpc_proxy_t *proxy = (rpc_proxy_t *)data;

    if (events & (EPOLLERR | EPOLLHUP)) {
        /* Req 13.2: Client disconnect mid-race — let upstream complete,
         * discard responses. We just mark client as gone. */
        log_msg(LOG_INFO, "[rpc_proxy] Client connection error/hangup");
        close_client(proxy);
        return;
    }

    if (!(events & EPOLLIN))
        return;

    /* Read available data into client_recv_buf */
    size_t space = proxy->client_recv_cap - proxy->client_recv_len;
    if (space == 0) {
        log_msg(LOG_WARN, "[rpc_proxy] Client recv buffer full (%zu bytes), "
                "dropping connection", proxy->client_recv_cap);
        close_client(proxy);
        return;
    }

    ssize_t n = recv(proxy->client_fd,
                     proxy->client_recv_buf + proxy->client_recv_len,
                     space, 0);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        log_msg(LOG_WARN, "[rpc_proxy] recv() from client failed: %s",
                strerror(errno));
        close_client(proxy);
        return;
    }

    if (n == 0) {
        /* Client closed connection gracefully */
        /* Req 13.2: let upstream requests complete, discard responses */
        log_msg(LOG_INFO, "[rpc_proxy] Client disconnected (EOF)");
        close_client(proxy);
        return;
    }

    proxy->client_recv_len += (size_t)n;

    /* Try to parse a complete HTTP request */
    if (parse_http_request(proxy)) {
        /* Single request in-flight enforcement:
         * If a race or sticky request is already active, drop this request */
        if (proxy->state != RACE_IDLE) {
            log_msg(LOG_WARN, "[rpc_proxy] Request received while race/sticky "
                    "active (state=%d) — dropping request", proxy->state);
            /* Reset recv buffer for next request (discard this one) */
            proxy->client_recv_len = 0;
            return;
        }

        /* Extract JSON-RPC method name */
        if (extract_json_rpc_method(proxy) < 0) {
            log_msg(LOG_WARN, "[rpc_proxy] Failed to extract JSON-RPC method "
                    "from request — dropping");
            proxy->client_recv_len = 0;
            return;
        }

        /* Req 9.7: Log RPC method name for every request */
        log_msg(LOG_INFO, "[rpc_proxy] RPC request: method=%s", proxy->method);

        /* Record request in stats */
        if (proxy->stats)
            stats_record_request(proxy->stats);

        /* Classify method and dispatch */
        route_strategy_t strategy = classify_method(proxy);

        switch (strategy) {
        case ROUTE_RACE:
            proxy->state = RACE_FANOUT;
            proxy->all_must_complete = false;
            proxy->winner_idx = -1;
            proxy->last_error_idx = -1;
            proxy->best_gbt_height = -1;
            proxy->best_gbt_node_idx = -1;
            proxy->gbt_height_matched = false;
            if (dispatch_fanout(proxy) <= 0) {
                /* No nodes available — return error immediately (Req 13.3) */
                log_msg(LOG_CRIT, "[rpc_proxy] All nodes unreachable for "
                        "method=%s", proxy->method);
                send_rpc_error_to_client(proxy, -1,
                    "All upstream nodes unreachable");
                proxy->state = RACE_IDLE;
            } else {
                start_rpc_timeout(proxy);
            }
            break;

        case ROUTE_BROADCAST:
            proxy->state = RACE_FANOUT;
            proxy->all_must_complete = true;
            proxy->winner_idx = -1;
            proxy->last_error_idx = -1;
            proxy->best_gbt_height = -1;
            proxy->best_gbt_node_idx = -1;
            proxy->gbt_height_matched = false;
            if (dispatch_fanout(proxy) <= 0) {
                log_msg(LOG_CRIT, "[rpc_proxy] All nodes unreachable for "
                        "method=%s", proxy->method);
                send_rpc_error_to_client(proxy, -1,
                    "All upstream nodes unreachable");
                proxy->state = RACE_IDLE;
            } else {
                start_rpc_timeout(proxy);
            }
            break;

        case ROUTE_STICKY:
            proxy->state = RACE_STICKY;
            proxy->all_must_complete = false;
            proxy->winner_idx = -1;
            proxy->last_error_idx = -1;
            proxy->best_gbt_height = -1;
            proxy->best_gbt_node_idx = -1;
            proxy->gbt_height_matched = false;
            if (dispatch_sticky(proxy) < 0) {
                /* dispatch_sticky handles error responses internally */
                proxy->state = RACE_IDLE;
            } else {
                start_rpc_timeout(proxy);
            }
            break;
        }

        /* Reset recv buffer for next request */
        proxy->client_recv_len = 0;
    }
}

/* ---- HTTP request parsing ---- */

/* Check if we have a complete HTTP request in client_recv_buf.
 * Returns 1 if complete, 0 if still waiting for more data. */
static int
parse_http_request(rpc_proxy_t *proxy)
{
    const char *buf = (const char *)proxy->client_recv_buf;
    size_t len = proxy->client_recv_len;

    /* Find end of HTTP headers: \r\n\r\n */
    const char *header_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            header_end = buf + i + 4;
            break;
        }
    }

    if (!header_end)
        return 0;  /* Headers not yet complete */

    size_t header_len = (size_t)(header_end - buf);

    /* Parse Content-Length to determine body size */
    size_t content_length = 0;
    const char *cl = strcasestr(buf, "Content-Length:");
    if (cl && cl < header_end) {
        cl += 15;  /* skip "Content-Length:" */
        while (*cl == ' ' || *cl == '\t')
            cl++;
        content_length = (size_t)strtoull(cl, NULL, 10);
    }

    /* Check if we have the full body */
    size_t total_expected = header_len + content_length;
    if (len < total_expected)
        return 0;  /* Still waiting for body data */

    return 1;  /* Complete request available */
}

/* ---- JSON-RPC method extraction ---- */

/* Extract the "method" field from the JSON-RPC body.
 * The body starts after the HTTP headers in client_recv_buf.
 * Returns 0 on success (method stored in proxy->method), -1 on failure. */
static int
extract_json_rpc_method(rpc_proxy_t *proxy)
{
    const char *buf = (const char *)proxy->client_recv_buf;
    size_t len = proxy->client_recv_len;

    /* Find start of body (after \r\n\r\n) */
    const char *body = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            body = buf + i + 4;
            break;
        }
    }

    if (!body)
        return -1;

    size_t body_len = len - (size_t)(body - buf);
    if (body_len == 0)
        return -1;

    /* Parse JSON using yyjson (immutable doc, read-only) */
    yyjson_doc *doc = yyjson_read(body, body_len, 0);
    if (!doc) {
        log_msg(LOG_WARN, "[rpc_proxy] Failed to parse JSON-RPC body");
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        log_msg(LOG_WARN, "[rpc_proxy] JSON-RPC body is not an object");
        yyjson_doc_free(doc);
        return -1;
    }

    yyjson_val *method_val = yyjson_obj_get(root, "method");
    if (!method_val || !yyjson_is_str(method_val)) {
        log_msg(LOG_WARN, "[rpc_proxy] JSON-RPC missing 'method' field");
        yyjson_doc_free(doc);
        return -1;
    }

    const char *method_str = yyjson_get_str(method_val);
    size_t method_len = yyjson_get_len(method_val);

    if (method_len >= sizeof(proxy->method)) {
        log_msg(LOG_WARN, "[rpc_proxy] Method name too long (%zu bytes)",
                method_len);
        yyjson_doc_free(doc);
        return -1;
    }

    memcpy(proxy->method, method_str, method_len);
    proxy->method[method_len] = '\0';

    yyjson_doc_free(doc);
    return 0;
}

/* ---- Method routing classification ---- */

/* Known method names for routing decisions */
static const char METHOD_GBT[] = "getblocktemplate";
static const char METHOD_SUBMITBLOCK[] = "submitblock";
static const char METHOD_SENDRAWTX[] = "sendrawtransaction";
static const char METHOD_PRECIOUSBLOCK[] = "preciousblock";
static const char METHOD_VALIDATEADDRESS[] = "validateaddress";
static const char METHOD_DECODERAWTX[] = "decoderawtransaction";

/* Classify the current method into a routing strategy.
 * Also logs unexpected methods at LOG_WARN (Req 4.5). */
static route_strategy_t
classify_method(rpc_proxy_t *proxy)
{
    const char *method = proxy->method;

    if (strcmp(method, METHOD_GBT) == 0) {
        /* getblocktemplate: race if notify_pending or no sticky, else sticky */
        if (proxy->notify_pending || proxy->sticky_node_idx == -1) {
            /* First GBT after notify (or startup with no sticky) → race */
            proxy->notify_pending = false;
            return ROUTE_RACE;
        }
        /* Subsequent GBT → sticky (Req 5.4) */
        return ROUTE_STICKY;
    }

    if (strcmp(method, METHOD_SUBMITBLOCK) == 0) {
        /* submitblock: broadcast to all, all must complete (Req 6.1) */
        return ROUTE_BROADCAST;
    }

    if (strcmp(method, METHOD_SENDRAWTX) == 0) {
        /* sendrawtransaction: broadcast to all, all must complete (Req 7.1) */
        return ROUTE_BROADCAST;
    }

    if (strcmp(method, METHOD_PRECIOUSBLOCK) == 0) {
        /* preciousblock: sticky only (Req 8.1) */
        return ROUTE_STICKY;
    }

    if (strcmp(method, METHOD_VALIDATEADDRESS) == 0) {
        /* validateaddress: known method, fan-out race (Req 4.1) */
        return ROUTE_RACE;
    }

    if (strcmp(method, METHOD_DECODERAWTX) == 0) {
        /* decoderawtransaction: known method, fan-out race (Req 4.1) */
        return ROUTE_RACE;
    }

    /* Unknown/other method: log warning, use fan-out race (Req 4.5) */
    log_msg(LOG_WARN, "[rpc_proxy] Unexpected RPC method: %s — "
            "processing via fan-out race", method);
    return ROUTE_RACE;
}

/* ---- Fan-out dispatch ---- */

/* Send the client request to all connected upstream nodes.
 * Returns the number of nodes dispatched to (0 if none available). */
static int
dispatch_fanout(rpc_proxy_t *proxy)
{
    int sent = 0;

    for (int i = 0; i < proxy->upstream_count; i++) {
        upstream_conn_t *conn = &proxy->upstreams[i];

        if (conn->state != CONN_CONNECTED) {
            log_msg(LOG_DEBUG, "[rpc_proxy] Upstream[%d] (%s) not connected "
                    "(state=%d), skipping",
                    i, conn->config->label, conn->state);
            continue;
        }

        if (rpc_conn_send(conn, proxy->client_recv_buf,
                          proxy->client_recv_len) == 0) {
            sent++;
            log_msg(LOG_DEBUG, "[rpc_proxy] Dispatched to upstream[%d] (%s)",
                    i, conn->config->label);
        } else {
            log_msg(LOG_WARN, "[rpc_proxy] rpc_conn_send failed for "
                    "upstream[%d] (%s)", i, conn->config->label);
        }
    }

    proxy->responses_pending = sent;

    log_msg(LOG_DEBUG, "[rpc_proxy] Fan-out dispatched to %d/%d nodes "
            "(method=%s, all_must_complete=%d)",
            sent, proxy->upstream_count, proxy->method,
            proxy->all_must_complete);

    return sent;
}

/* ---- Sticky dispatch ---- */

/* Send the client request to the sticky node only.
 * For preciousblock with no sticky: returns RPC error (Req 8.2).
 * For GBT with sticky unreachable: falls back to fan-out race.
 * Returns 0 on success, -1 on failure (error already sent to client). */
static int
dispatch_sticky(rpc_proxy_t *proxy)
{
    const char *method = proxy->method;
    bool is_preciousblock = (strcmp(method, METHOD_PRECIOUSBLOCK) == 0);

    /* Req 8.2: preciousblock with no sticky → error */
    if (proxy->sticky_node_idx == -1) {
        if (is_preciousblock) {
            log_msg(LOG_WARN, "[rpc_proxy] preciousblock with no sticky node "
                    "— returning error");
            send_rpc_error_to_client(proxy, -32000,
                "No sticky node designated — cannot route preciousblock");
            return -1;
        }
        /* GBT with no sticky (shouldn't happen via classify, but handle) */
        log_msg(LOG_INFO, "[rpc_proxy] No sticky node for method=%s, "
                "falling back to fan-out race", method);
        proxy->state = RACE_FANOUT;
        proxy->all_must_complete = false;
        proxy->best_gbt_height = -1;
        proxy->best_gbt_node_idx = -1;
        proxy->gbt_height_matched = false;
        int sent = dispatch_fanout(proxy);
        if (sent <= 0) {
            log_msg(LOG_CRIT, "[rpc_proxy] All nodes unreachable for "
                    "method=%s", method);
            send_rpc_error_to_client(proxy, -1,
                "All upstream nodes unreachable");
            return -1;
        }
        return 0;
    }

    /* Check if sticky node is reachable */
    upstream_conn_t *sticky = &proxy->upstreams[proxy->sticky_node_idx];

    if (sticky->state != CONN_CONNECTED) {
        /* Req 8.3: preciousblock with sticky unreachable → error */
        if (is_preciousblock) {
            log_msg(LOG_WARN, "[rpc_proxy] Sticky node[%d] (%s) unreachable "
                    "for preciousblock — returning error",
                    proxy->sticky_node_idx, sticky->config->label);
            send_rpc_error_to_client(proxy, -32001,
                "Sticky node unreachable — cannot route preciousblock");
            return -1;
        }

        /* GBT with sticky unreachable → fall back to fan-out race */
        log_msg(LOG_INFO, "[rpc_proxy] Sticky node[%d] (%s) unreachable for "
                "method=%s — falling back to fan-out race",
                proxy->sticky_node_idx, sticky->config->label, method);
        proxy->state = RACE_FANOUT;
        proxy->all_must_complete = false;
        proxy->best_gbt_height = -1;
        proxy->best_gbt_node_idx = -1;
        proxy->gbt_height_matched = false;
        int sent = dispatch_fanout(proxy);
        if (sent <= 0) {
            log_msg(LOG_CRIT, "[rpc_proxy] All nodes unreachable for "
                    "method=%s", method);
            send_rpc_error_to_client(proxy, -1,
                "All upstream nodes unreachable");
            return -1;
        }
        return 0;
    }

    /* Send to sticky node */
    if (rpc_conn_send(sticky, proxy->client_recv_buf,
                      proxy->client_recv_len) < 0) {
        log_msg(LOG_WARN, "[rpc_proxy] rpc_conn_send failed for sticky "
                "node[%d] (%s)", proxy->sticky_node_idx,
                sticky->config->label);

        if (is_preciousblock) {
            send_rpc_error_to_client(proxy, -32001,
                "Sticky node send failed — cannot route preciousblock");
            return -1;
        }

        /* GBT: fall back to fan-out */
        proxy->state = RACE_FANOUT;
        proxy->all_must_complete = false;
        proxy->best_gbt_height = -1;
        proxy->best_gbt_node_idx = -1;
        proxy->gbt_height_matched = false;
        int sent = dispatch_fanout(proxy);
        if (sent <= 0) {
            send_rpc_error_to_client(proxy, -1,
                "All upstream nodes unreachable");
            return -1;
        }
        return 0;
    }

    proxy->responses_pending = 1;
    log_msg(LOG_DEBUG, "[rpc_proxy] Sticky dispatch to node[%d] (%s) "
            "for method=%s",
            proxy->sticky_node_idx, sticky->config->label, method);

    return 0;
}

/* ---- RPC error response to client ---- */

/* Send an HTTP response with a JSON-RPC error body to the client.
 * Used for preciousblock with no sticky, all-nodes-unreachable, etc. */
static void
send_rpc_error_to_client(rpc_proxy_t *proxy, int code, const char *message)
{
    if (!proxy->client_connected)
        return;

    /* Build JSON-RPC error response */
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{\"result\":null,\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":null}",
        code, message);

    if (body_len < 0 || (size_t)body_len >= sizeof(body))
        body_len = (int)strlen(body);

    /* Build HTTP response */
    char response[768];
    int resp_len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        body_len, body);

    if (resp_len < 0 || (size_t)resp_len >= sizeof(response))
        return;

    /* Best-effort send to client (non-blocking) */
    ssize_t n = send(proxy->client_fd, response, (size_t)resp_len, MSG_NOSIGNAL);
    if (n < 0) {
        log_msg(LOG_WARN, "[rpc_proxy] Failed to send error response to "
                "client: %s", strerror(errno));
    }
}

/* ---- Race response handling ---- */

/* Check if an upstream response indicates an error.
 * An error is: HTTP status != 200, or JSON body has non-null "error" field.
 * Returns 1 if error, 0 if success. */
static int
response_is_error(const upstream_conn_t *conn)
{
    if (!conn->headers_complete || conn->recv_len == 0)
        return 1;  /* No valid response — treat as error */

    /* Parse HTTP status line: "HTTP/1.1 <status> ..." */
    const char *buf = (const char *)conn->recv_buf;
    int http_status = 0;

    if (conn->recv_len >= 12 && strncmp(buf, "HTTP/", 5) == 0) {
        /* Find the space after version, then parse status code */
        const char *sp = memchr(buf, ' ', conn->recv_len < 16 ? conn->recv_len : 16);
        if (sp)
            http_status = atoi(sp + 1);
    }

    if (http_status != 200)
        return 1;  /* HTTP-level error */

    /* Parse JSON body for "error" field */
    const uint8_t *body = NULL;
    size_t body_len = 0;

    if (rpc_conn_get_response(conn, &body, &body_len) < 0)
        return 1;  /* Can't get body — treat as error */

    if (body_len == 0)
        return 1;  /* Empty body — treat as error */

    yyjson_doc *doc = yyjson_read((const char *)body, body_len, 0);
    if (!doc)
        return 1;  /* Can't parse JSON — treat as error */

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return 1;
    }

    yyjson_val *error_val = yyjson_obj_get(root, "error");

    /* "error": null means success; any non-null value means RPC error */
    int is_error = (error_val && !yyjson_is_null(error_val)) ? 1 : 0;

    yyjson_doc_free(doc);
    return is_error;
}

/* Parse the RPC error code from a response body.
 * Returns the error code (e.g. -10 for IBD), or 0 if not an error or unparseable. */
static int
parse_rpc_error_code(const upstream_conn_t *conn)
{
    const uint8_t *body = NULL;
    size_t body_len = 0;

    if (rpc_conn_get_response(conn, &body, &body_len) < 0 || body_len == 0)
        return 0;

    yyjson_doc *doc = yyjson_read((const char *)body, body_len, 0);
    if (!doc)
        return 0;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return 0;
    }

    yyjson_val *error_val = yyjson_obj_get(root, "error");
    if (!error_val || !yyjson_is_obj(error_val)) {
        yyjson_doc_free(doc);
        return 0;
    }

    yyjson_val *code_val = yyjson_obj_get(error_val, "code");
    int code = 0;
    if (code_val && yyjson_is_int(code_val))
        code = (int)yyjson_get_sint(code_val);

    yyjson_doc_free(doc);
    return code;
}

/* Parse the "height" field from a GBT response body.
 * The height is inside the "result" object: {"result": {"height": N, ...}, ...}
 * Returns the height value on success, -1 on failure (parse error, missing field). */
static int64_t
parse_gbt_height(const upstream_conn_t *conn)
{
    const uint8_t *body = NULL;
    size_t body_len = 0;

    if (rpc_conn_get_response(conn, &body, &body_len) < 0)
        return -1;

    if (body_len == 0)
        return -1;

    yyjson_doc *doc = yyjson_read((const char *)body, body_len, 0);
    if (!doc)
        return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return -1;
    }

    yyjson_val *result_val = yyjson_obj_get(root, "result");
    if (!result_val || !yyjson_is_obj(result_val)) {
        yyjson_doc_free(doc);
        return -1;
    }

    yyjson_val *height_val = yyjson_obj_get(result_val, "height");
    if (!height_val || !yyjson_is_int(height_val)) {
        yyjson_doc_free(doc);
        return -1;
    }

    int64_t height = yyjson_get_sint(height_val);
    yyjson_doc_free(doc);
    return height;
}

/* Parse the transaction count from a GBT response body.
 * The transactions array is inside "result": {"result": {"transactions": [...], ...}, ...}
 * Returns the array size on success, 0 on failure (parse error, missing field). */
static uint32_t
parse_gbt_tx_count(const upstream_conn_t *conn)
{
    const uint8_t *body = NULL;
    size_t body_len = 0;

    if (rpc_conn_get_response(conn, &body, &body_len) < 0)
        return 0;

    if (body_len == 0)
        return 0;

    yyjson_doc *doc = yyjson_read((const char *)body, body_len, 0);
    if (!doc)
        return 0;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return 0;
    }

    yyjson_val *result_val = yyjson_obj_get(root, "result");
    if (!result_val || !yyjson_is_obj(result_val)) {
        yyjson_doc_free(doc);
        return 0;
    }

    yyjson_val *tx_arr = yyjson_obj_get(result_val, "transactions");
    if (!tx_arr || !yyjson_is_arr(tx_arr)) {
        yyjson_doc_free(doc);
        return 0;
    }

    uint32_t count = (uint32_t)yyjson_arr_size(tx_arr);
    yyjson_doc_free(doc);
    return count;
}

/* Send the full HTTP response from an upstream connection to the client.
 * Writes the entire recv_buf content (headers + body) to the client fd. */
static void
send_upstream_response_to_client(rpc_proxy_t *proxy, upstream_conn_t *conn)
{
    if (!proxy->client_connected)
        return;

    /* The recv_buf contains the full HTTP response (headers + body) */
    size_t total = conn->header_len + conn->content_length;
    if (total == 0 || total > conn->recv_len)
        return;

    /* Best-effort non-blocking send to client */
    size_t sent = 0;
    while (sent < total) {
        ssize_t n = send(proxy->client_fd, conn->recv_buf + sent,
                         total - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;  /* Can't send more right now — partial send */
            log_msg(LOG_WARN, "[rpc_proxy] Failed to send response to "
                    "client: %s", strerror(errno));
            break;
        }
        sent += (size_t)n;
    }
}

/* ---- RPC timeout helpers ---- */

/* Start a one-shot timeout timer for the current race. */
static void
start_rpc_timeout(rpc_proxy_t *proxy)
{
    if (proxy->cfg->rpc_timeout_ms == 0)
        return;  /* timeout disabled */

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        log_msg(LOG_WARN, "[rpc_proxy] timerfd_create for rpc timeout failed: %s",
                strerror(errno));
        return;
    }

    struct itimerspec its = {0};
    uint32_t ms = proxy->cfg->rpc_timeout_ms;
    its.it_value.tv_sec = (time_t)(ms / 1000);
    its.it_value.tv_nsec = (long)((ms % 1000) * 1000000);
    /* it_interval left at zero → one-shot */

    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        log_msg(LOG_WARN, "[rpc_proxy] timerfd_settime for rpc timeout failed: %s",
                strerror(errno));
        close(tfd);
        return;
    }

    if (event_loop_add_fd(proxy->loop, tfd, EPOLLIN, rpc_timeout_cb, proxy) < 0) {
        log_msg(LOG_WARN, "[rpc_proxy] event_loop_add_fd for rpc timeout failed");
        close(tfd);
        return;
    }

    proxy->rpc_timeout_timer_fd = tfd;
}

/* Cancel and close the timeout timer if active. */
static void
cancel_rpc_timeout(rpc_proxy_t *proxy)
{
    if (proxy->rpc_timeout_timer_fd < 0)
        return;

    event_loop_del_fd(proxy->loop, proxy->rpc_timeout_timer_fd);
    close(proxy->rpc_timeout_timer_fd);
    proxy->rpc_timeout_timer_fd = -1;
}

/* Timeout callback: fires when the race exceeds rpc_timeout_ms. */
static void
rpc_timeout_cb(event_loop_t *loop, int fd, uint32_t events, void *data)
{
    (void)loop;
    (void)events;

    rpc_proxy_t *proxy = (rpc_proxy_t *)data;

    /* Acknowledge the timerfd expiration */
    uint64_t expirations = 0;
    ssize_t r = read(fd, &expirations, sizeof(expirations));
    (void)r;

    /* Only act if a race is still in progress */
    if (proxy->state == RACE_IDLE)
        return;

    log_msg(LOG_WARN, "[rpc_proxy] RPC timeout (%u ms) fired for method=%s, "
            "%d responses still pending",
            proxy->cfg->rpc_timeout_ms, proxy->method,
            proxy->responses_pending);

    /* Treat all pending nodes as errors */
    proxy->responses_pending = 0;

    /* If no winner was found, send error to client */
    if (proxy->winner_idx == -1) {
        send_rpc_error_to_client(proxy, -32000,
            "RPC timeout: upstream nodes did not respond in time");
    }

    race_complete(proxy);
}

/* Complete the race: reset all upstream connections, return to IDLE state. */
static void
race_complete(rpc_proxy_t *proxy)
{
    /* Cancel the timeout timer if active */
    cancel_rpc_timeout(proxy);

    /* Reset all upstream connections that participated */
    for (int i = 0; i < proxy->upstream_count; i++) {
        upstream_conn_t *conn = &proxy->upstreams[i];
        /* Only reset connections that have a send_buf set (participated in race) */
        if (conn->send_buf != NULL || conn->state == CONN_RECEIVING ||
            conn->state == CONN_SENDING) {
            rpc_conn_reset(conn);
        }
    }

    proxy->state = RACE_IDLE;
    proxy->responses_pending = 0;
    proxy->winner_idx = -1;
    proxy->last_error_idx = -1;
    proxy->best_gbt_height = -1;
    proxy->best_gbt_node_idx = -1;
    proxy->gbt_height_matched = false;

    log_msg(LOG_DEBUG, "[rpc_proxy] Race complete, state → IDLE");
}

/* Callback: upstream connection delivered a complete HTTP response. */
static void
on_upstream_response(upstream_conn_t *conn, void *data)
{
    rpc_proxy_t *proxy = (rpc_proxy_t *)data;
    int node_idx = conn->node_index;

    /* Calculate elapsed time */
    uint64_t now_ns = clock_monotonic_ns();
    uint64_t elapsed_ns = (conn->request_start_ns > 0)
                          ? (now_ns - conn->request_start_ns) : 0;
    uint64_t elapsed_us = elapsed_ns / 1000;

    /* Req 9.9: Log warning if elapsed > 5s identifying slow node */
    if (should_log_slow_response(elapsed_us)) {
        log_msg(LOG_WARN, "[%s] Slow response: elapsed=%.1fs method=%s",
                conn->config->label,
                (double)elapsed_us / 1000000.0,
                proxy->method);
    }

    /* Decrement pending count */
    proxy->responses_pending--;

    /* Check if this response is an error */
    int is_error = response_is_error(conn);

    /* Determine if this is a GBT race (fan-out for getblocktemplate) */
    bool is_gbt_race = (strcmp(proxy->method, METHOD_GBT) == 0 &&
                        proxy->state == RACE_FANOUT);

    if (is_error) {
        /* Track last error for all-error fallback */
        proxy->last_error_idx = node_idx;

        /* Detect IBD (error code -10) on GBT requests */
        if (is_gbt_race) {
            int err_code = parse_rpc_error_code(conn);
            if (err_code == -10) {
                if (!conn->in_ibd) {
                    conn->in_ibd = true;
                    log_msg(LOG_WARN, "[%s] Node entered IBD state "
                            "(error -10: Bitcoin is downloading blocks)",
                            conn->config->label);
                }
            }
        }

        log_msg(LOG_DEBUG, "[rpc_proxy] Upstream[%d] (%s) responded with "
                "error (method=%s, elapsed_us=%llu)",
                node_idx, conn->config->label, proxy->method,
                (unsigned long long)elapsed_us);
    } else if (is_gbt_race) {
        /* ---- GBT height validation logic (Req 5.2, 5.3, 5.5, 5.7) ---- */

        /* Detect IBD recovery */
        if (conn->in_ibd) {
            conn->in_ibd = false;
            log_msg(LOG_INFO, "[%s] Node exited IBD state "
                    "(GBT response successful)", conn->config->label);
        }

        int64_t height = parse_gbt_height(conn);

        if (height < 0) {
            /* Could not parse height — treat as valid but with no height info.
             * For startup (Req 5.7): first valid response wins. */
            log_msg(LOG_WARN, "[rpc_proxy] Upstream[%d] (%s) GBT response "
                    "missing height field", node_idx, conn->config->label);
        }

        log_msg(LOG_DEBUG, "[rpc_proxy] Upstream[%d] (%s) GBT height=%lld "
                "(expected=%lld, best=%lld, elapsed_us=%llu)",
                node_idx, conn->config->label,
                (long long)height,
                (long long)(proxy->last_block_height + 1),
                (long long)proxy->best_gbt_height,
                (unsigned long long)elapsed_us);

        /* Log each GBT response at INFO with time since notify */
        {
            uint64_t since_notify_us = 0;
            if (proxy->last_notify_ns > 0) {
                since_notify_us = (now_ns - proxy->last_notify_ns) / 1000;
            }
            log_msg(LOG_INFO, "[%s] GBT response: height=%lld elapsed_us=%llu "
                    "since_notify_us=%llu",
                    conn->config->label, (long long)height,
                    (unsigned long long)elapsed_us,
                    (unsigned long long)since_notify_us);
        }

        int64_t expected_height = proxy->last_block_height + 1;

        if (!proxy->gbt_height_matched && height == expected_height) {
            /* Req 5.2: First response matching expected height wins immediately */
            proxy->gbt_height_matched = true;
            proxy->winner_idx = node_idx;
            proxy->sticky_node_idx = node_idx;

            /* Req 5.5: Update last_block_height (monotonicity) */
            if (height > proxy->last_block_height)
                proxy->last_block_height = height;

            /* Req 9.6: Log race winner */
            log_msg(LOG_INFO, "[%s] GBT race winner (height match): "
                    "height=%lld elapsed_us=%llu",
                    conn->config->label, (long long)height,
                    (unsigned long long)elapsed_us);

            /* Req 9.8: Log additional detail for GBT */
            uint32_t tx_count = parse_gbt_tx_count(conn);
            log_msg(LOG_INFO, "[%s] getblocktemplate response: elapsed_us=%llu "
                    "tx_count=%u",
                    conn->config->label,
                    (unsigned long long)elapsed_us,
                    (unsigned)tx_count);

            /* Record stats */
            if (proxy->stats) {
                uint64_t since_notify_us = 0;
                if (proxy->last_notify_ns > 0)
                    since_notify_us = (now_ns - proxy->last_notify_ns) / 1000;
                stats_record_gbt(proxy->stats, node_idx, elapsed_us, tx_count, since_notify_us);
                stats_record_race_win(proxy->stats, node_idx);
                stats_record_notify_to_gbt(proxy->stats, since_notify_us);
            }

            /* Send response to client immediately */
            send_upstream_response_to_client(proxy, conn);

        } else if (!proxy->gbt_height_matched) {
            /* No exact match yet — track best height (Req 5.3) */
            /* Highest height wins; if tied, last received wins */
            if (height >= proxy->best_gbt_height) {
                proxy->best_gbt_height = height;
                proxy->best_gbt_node_idx = node_idx;
            }
        } else {
            /* Already have an exact match winner — discard */
            log_msg(LOG_DEBUG, "[rpc_proxy] Upstream[%d] (%s) GBT response "
                    "discarded (height match winner already: node[%d])",
                    node_idx, conn->config->label, proxy->winner_idx);
        }
    } else {
        /* ---- Generic race logic (non-GBT methods) ---- */
        if (proxy->winner_idx == -1) {
            /* First non-error response wins the race */
            proxy->winner_idx = node_idx;

            /* Req 9.6: Log race winner */
            log_msg(LOG_INFO, "[%s] %s: method=%s elapsed_us=%llu",
                    conn->config->label,
                    (proxy->state == RACE_STICKY) ? "Sticky response" : "Race winner",
                    proxy->method,
                    (unsigned long long)elapsed_us);

            /* Req 9.8: Log additional detail for submitblock */
            if (strcmp(proxy->method, METHOD_SUBMITBLOCK) == 0) {
                log_msg(LOG_INFO, "[%s] %s response: elapsed_us=%llu "
                        "response_bytes=%zu",
                        conn->config->label, proxy->method,
                        (unsigned long long)elapsed_us,
                        conn->header_len + conn->content_length);
            }

            /* Send response to client */
            send_upstream_response_to_client(proxy, conn);
        } else {
            /* Subsequent non-error: discard (already have a winner) */
            log_msg(LOG_DEBUG, "[rpc_proxy] Upstream[%d] (%s) response "
                    "discarded (winner already selected: node[%d])",
                    node_idx, conn->config->label, proxy->winner_idx);
        }
    }

    /* Determine if race is over */
    if (proxy->responses_pending <= 0) {
        /* All responses received */
        if (proxy->winner_idx == -1) {
            if (is_gbt_race && proxy->best_gbt_node_idx >= 0) {
                /* GBT height fallback: no exact match, use best height (Req 5.3) */
                int best_idx = proxy->best_gbt_node_idx;
                upstream_conn_t *best_conn = &proxy->upstreams[best_idx];

                proxy->winner_idx = best_idx;
                proxy->sticky_node_idx = best_idx;

                /* Req 5.5: Update last_block_height (monotonicity) */
                if (proxy->best_gbt_height > proxy->last_block_height)
                    proxy->last_block_height = proxy->best_gbt_height;

                /* Req 9.6: Log race winner */
                log_msg(LOG_INFO, "[%s] GBT race winner (height fallback): "
                        "height=%lld elapsed_us=%llu",
                        best_conn->config->label,
                        (long long)proxy->best_gbt_height,
                        (unsigned long long)elapsed_us);

                /* Req 9.8: Log additional detail for GBT */
                uint32_t tx_count = parse_gbt_tx_count(best_conn);
                log_msg(LOG_INFO, "[%s] getblocktemplate response: "
                        "elapsed_us=%llu tx_count=%u",
                        best_conn->config->label,
                        (unsigned long long)elapsed_us,
                        (unsigned)tx_count);

                /* Record stats */
                if (proxy->stats) {
                    uint64_t since_notify_us = 0;
                    if (proxy->last_notify_ns > 0)
                        since_notify_us = (now_ns - proxy->last_notify_ns) / 1000;
                    stats_record_gbt(proxy->stats, best_idx, elapsed_us, tx_count, since_notify_us);
                    stats_record_race_win(proxy->stats, best_idx);
                    stats_record_notify_to_gbt(proxy->stats, since_notify_us);
                }

                /* Send best response to client */
                send_upstream_response_to_client(proxy, best_conn);
            } else if (proxy->last_error_idx >= 0) {
                /* All-error fallback: return last error (Req 4.4, 5.6, 6.4, 7.4) */
                upstream_conn_t *err_conn = &proxy->upstreams[proxy->last_error_idx];
                log_msg(LOG_WARN, "[rpc_proxy] All nodes returned errors for "
                        "method=%s — returning last error from node[%d] (%s)",
                        proxy->method, proxy->last_error_idx,
                        err_conn->config->label);
                send_upstream_response_to_client(proxy, err_conn);
            }
        }
        race_complete(proxy);
    } else if (!proxy->all_must_complete && proxy->winner_idx != -1) {
        /* For non-broadcast: we have a winner but still waiting for others.
         * Let them complete naturally (Req 4.2: allow all to complete,
         * discard late responses). Don't reset yet. */
        log_msg(LOG_DEBUG, "[rpc_proxy] Winner found, %d responses still "
                "pending (will discard)", proxy->responses_pending);
    }
    /* For broadcast (all_must_complete): always wait for all to finish */
}

/* Callback: upstream connection failed mid-transfer. */
static void
on_upstream_error(upstream_conn_t *conn, void *data)
{
    rpc_proxy_t *proxy = (rpc_proxy_t *)data;
    int node_idx = conn->node_index;

    /* Only process if we're in an active race/sticky state */
    if (proxy->state == RACE_IDLE)
        return;

    log_msg(LOG_WARN, "[%s] Upstream connection error during %s "
            "(node_index=%d)",
            conn->config->label, proxy->method, node_idx);

    /* Decrement pending count */
    proxy->responses_pending--;

    /* Track as error for fallback purposes */
    proxy->last_error_idx = node_idx;

    /* Check if race is over */
    if (proxy->responses_pending <= 0) {
        if (proxy->winner_idx == -1) {
            /* All nodes failed — Req 13.3: return error immediately */
            log_msg(LOG_CRIT, "[rpc_proxy] All nodes failed for method=%s",
                    proxy->method);
            send_rpc_error_to_client(proxy, -1,
                "All upstream nodes failed");
        }
        race_complete(proxy);
    }
}

/* ---- Reconnect timer ---- */

/* Periodic timer: check disconnected upstreams and attempt reconnection. */
static void
reconnect_timer_cb(event_loop_t *loop, void *data)
{
    (void)loop;
    rpc_proxy_t *proxy = (rpc_proxy_t *)data;

    for (int i = 0; i < proxy->upstream_count; i++) {
        rpc_conn_try_reconnect(&proxy->upstreams[i]);
    }

    /* Check if all nodes are DEAD — if so, close listener and drop client
     * to signal the stratum proxy to fail over */
    bool all_dead = true;
    for (int i = 0; i < proxy->upstream_count; i++) {
        if (proxy->upstreams[i].state != CONN_DEAD) {
            all_dead = false;
            break;
        }
    }

    if (all_dead) {
        if (proxy->client_connected) {
            log_msg(LOG_CRIT, "[rpc_proxy] All nodes DEAD — dropping client "
                    "connection to trigger failover");
            close_client(proxy);
        }
        if (proxy->listen_fd >= 0) {
            log_msg(LOG_CRIT, "[rpc_proxy] All nodes DEAD — closing listener "
                    "to refuse new connections");
            event_loop_del_fd(proxy->loop, proxy->listen_fd);
            close(proxy->listen_fd);
            proxy->listen_fd = -1;
        }
    } else if (proxy->listen_fd < 0) {
        /* At least one node is alive again — reopen listener */
        log_msg(LOG_INFO, "[rpc_proxy] Node recovered — reopening listener");
        setup_listener(proxy);
    }
}

/* ---- Public API ---- */

rpc_proxy_t *
rpc_proxy_create(event_loop_t *loop, config_t *cfg)
{
    if (!loop || !cfg)
        return NULL;

    rpc_proxy_t *proxy = calloc(1, sizeof(*proxy));
    if (!proxy)
        return NULL;

    proxy->loop = loop;
    proxy->cfg = cfg;
    proxy->listen_fd = -1;
    proxy->client_fd = -1;
    proxy->client_connected = false;

    /* Pre-allocate client receive buffer */
    proxy->client_recv_buf = malloc(SOCKET_BUF_SIZE);
    if (!proxy->client_recv_buf) {
        free(proxy);
        return NULL;
    }
    proxy->client_recv_len = 0;
    proxy->client_recv_cap = SOCKET_BUF_SIZE;

    /* Initialize race state */
    proxy->state = RACE_IDLE;
    proxy->sticky_node_idx = -1;
    proxy->last_block_height = 0;
    proxy->notify_pending = false;
    proxy->last_notify_ns = 0;
    proxy->responses_pending = 0;
    proxy->winner_idx = -1;
    proxy->last_error_idx = -1;
    proxy->all_must_complete = false;
    proxy->best_gbt_height = -1;
    proxy->best_gbt_node_idx = -1;
    proxy->gbt_height_matched = false;
    proxy->method[0] = '\0';
    proxy->stats = NULL;
    proxy->rpc_timeout_timer_fd = -1;

    /* Initialize upstream connections */
    proxy->upstream_count = cfg->node_count;

    conn_callbacks_t cb = {0};
    cb.on_response = on_upstream_response;
    cb.on_error = on_upstream_error;
    cb.data = proxy;

    for (int i = 0; i < cfg->node_count; i++) {
        if (rpc_conn_init(&proxy->upstreams[i], loop, &cfg->nodes[i], i,
                          cfg->reconnect_delay_ms, &cb) < 0) {
            log_msg(LOG_CRIT, "[rpc_proxy] Failed to init upstream[%d] (%s)",
                    i, cfg->nodes[i].label);
            /* Clean up already-initialized connections */
            for (int j = 0; j < i; j++)
                rpc_conn_destroy(&proxy->upstreams[j]);
            free(proxy->client_recv_buf);
            free(proxy);
            return NULL;
        }
    }

    /* Initiate connections to all upstream nodes immediately (Req 14.1) */
    for (int i = 0; i < cfg->node_count; i++) {
        rpc_conn_connect(&proxy->upstreams[i]);
    }

    /* Reconnect timer: check every 1 second for disconnected nodes */
    if (event_loop_add_timer(loop, 1000, reconnect_timer_cb, proxy) < 0) {
        log_msg(LOG_WARN, "[rpc_proxy] Failed to create reconnect timer");
    }

    /* Set up the RPC listener */
    if (setup_listener(proxy) < 0) {
        for (int i = 0; i < cfg->node_count; i++)
            rpc_conn_destroy(&proxy->upstreams[i]);
        free(proxy->client_recv_buf);
        free(proxy);
        return NULL;
    }

    return proxy;
}

void
rpc_proxy_destroy(rpc_proxy_t *proxy)
{
    if (!proxy)
        return;

    /* Cancel any active timeout timer */
    cancel_rpc_timeout(proxy);

    /* Close client connection */
    if (proxy->client_connected)
        close_client(proxy);

    /* Close listener */
    if (proxy->listen_fd >= 0) {
        event_loop_del_fd(proxy->loop, proxy->listen_fd);
        close(proxy->listen_fd);
        proxy->listen_fd = -1;
    }

    /* Destroy upstream connections */
    for (int i = 0; i < proxy->upstream_count; i++)
        rpc_conn_destroy(&proxy->upstreams[i]);

    /* Free client buffer */
    free(proxy->client_recv_buf);
    proxy->client_recv_buf = NULL;

    free(proxy);
}

void
rpc_proxy_on_block_notify(rpc_proxy_t *proxy, const uint8_t *hash)
{
    if (!proxy)
        return;

    (void)hash;  /* Hash used for logging; height tracking done via GBT response */

    /* Clear sticky state — next GBT will race */
    proxy->sticky_node_idx = -1;
    proxy->notify_pending = true;
    proxy->last_notify_ns = clock_monotonic_ns();

    /* Record notify time in stats */
    if (proxy->stats)
        stats_record_notify_time(proxy->stats, proxy->last_notify_ns);

    if (proxy->state == RACE_STICKY)
        proxy->state = RACE_IDLE;

    log_msg(LOG_INFO, "[rpc_proxy] Block notify received — sticky cleared, "
            "next GBT will race");
}

void
rpc_proxy_set_stats(rpc_proxy_t *proxy, stats_t *stats)
{
    if (proxy)
        proxy->stats = stats;
}

void
rpc_proxy_get_states(rpc_proxy_t *proxy, const char **out, int count)
{
    if (!proxy || !out)
        return;

    for (int i = 0; i < count && i < proxy->upstream_count; i++) {
        if (proxy->upstreams[i].in_ibd) {
            out[i] = "ibd";
            continue;
        }
        switch (proxy->upstreams[i].state) {
        case CONN_DISCONNECTED: out[i] = "disconnected"; break;
        case CONN_CONNECTING:   out[i] = "connecting";   break;
        case CONN_CONNECTED:    out[i] = "connected";    break;
        case CONN_SENDING:      out[i] = "sending";      break;
        case CONN_RECEIVING:    out[i] = "receiving";    break;
        case CONN_DEAD:         out[i] = "dead";         break;
        default:                out[i] = "unknown";      break;
        }
    }
}

bool
rpc_proxy_is_node_ibd(rpc_proxy_t *proxy, int node_idx)
{
    if (!proxy || node_idx < 0 || node_idx >= proxy->upstream_count)
        return false;
    return proxy->upstreams[node_idx].in_ibd;
}
