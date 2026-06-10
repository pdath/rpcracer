/* notifier.c — ZMQ subscriptions, deduplication, and block notification relay */

#include "notifier.h"
#include "log.h"
#include "rpc_proxy.h"
#include "stats.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <zmq.h>

/* ZMQ hashblock topic: 9 bytes, no null terminator */
#define ZMQ_TOPIC_HASHBLOCK     "hashblock"
#define ZMQ_TOPIC_HASHBLOCK_LEN 9

/* Expected hash size in bytes */
#define HASH_SIZE 32

/* Silence detection: warn if a node hasn't reported a block within this
 * many nanoseconds after another node reported it. (60 seconds) */
#define SILENCE_THRESHOLD_NS (60ULL * 1000000000ULL)

/* How often to check for silent nodes (10 seconds) */
#define SILENCE_CHECK_INTERVAL_MS 10000

/* Grace period after accepting a new block notify: suppress notifies from
 * non-winning nodes for this duration (10 seconds). */
#define NOTIFY_GRACE_PERIOD_NS (10ULL * 1000000000ULL)

/* Hex string buffer: 32 bytes * 2 + null */
#define HEX_HASH_LEN 65

/* HTTP notify connection states */
#define HTTP_STATE_DISCONNECTED 0
#define HTTP_STATE_CONNECTED    1

struct notifier {
    /* ZMQ subscriber sockets (one per node with ZMQ configured) */
    void *zmq_ctx;
    void *zmq_subs[MAX_NODES];
    int zmq_sub_count;
    int zmq_fds[MAX_NODES];         /* file descriptors for epoll integration */

    /* ZMQ publisher socket (for downstream relay) */
    void *zmq_pub;

    /* ZMQ PUB sequence counter (monotonically increasing, 4-byte LE) */
    uint32_t zmq_pub_seq;

    /* Deduplication — stored as 32 bytes in display/RPC byte order */
    uint8_t last_hash[HASH_SIZE];
    bool has_last_hash;

    /* Downstream HTTP notify relay state */
    int notify_http_fd;
    int notify_http_state;
    char notify_http_host[256];
    uint16_t notify_http_port;
    char notify_http_path[512];     /* path portion of URL (with %s or as-is) */
    bool notify_http_configured;

    /* CK socket relay state */
    int ck_fd;                    /* Unix socket fd, -1 if disconnected */
    char ck_path[108];            /* configured socket path */
    bool ck_configured;           /* true if path is non-empty */

    /* Silence detection: per-node tracking.
     * node_map[i] maps zmq_sub index i to the config node index.
     * node_last_hash[node_idx] = last block hash reported by that node.
     * node_hash_changed_ns[node_idx] = monotonic time when node last reported
     *   a hash different from its previous one (i.e. pipeline activity).
     * first_notify_ns = monotonic timestamp of first notify for current block. */
    int node_map[MAX_NODES];        /* zmq_sub index → config node index */
    uint8_t node_last_hash[MAX_NODES][HASH_SIZE]; /* per-node: last reported hash */
    bool node_has_hash[MAX_NODES];  /* per-node: has reported at least one hash */
    uint64_t node_hash_changed_ns[MAX_NODES]; /* per-node: last hash change time */
    uint64_t first_notify_ns;       /* timestamp of first notify for current block */
    bool silence_warned[MAX_NODES]; /* already warned for current block event? */
    int total_notify_sources;       /* number of nodes with ZMQ or HTTP notify */

    /* Callback */
    notify_cb on_notify;
    void *cb_data;

    /* Grace period: after accepting a new block notify, suppress notifies
     * from other nodes for 10s. Only the winning notify node (the node that
     * triggered the accepted notify) can break through the grace period. */
    bool grace_active;
    uint64_t grace_start_ns;
    int grace_winner_node_idx;      /* config node index of winning notify node */

    /* References */
    event_loop_t *loop;
    config_t *cfg;
    stats_t *stats;  /* optional, for recording notify wins */
    struct rpc_proxy *proxy;  /* optional, for IBD state checks */
};

/* ---- internal helpers ---- */

/* Check if a hash is a duplicate of the last seen hash.
 * Returns true if duplicate (should suppress), false if new. */
static bool
is_duplicate(notifier_t *n, const uint8_t *hash)
{
    if (!n->has_last_hash)
        return false;
    return memcmp(n->last_hash, hash, HASH_SIZE) == 0;
}

/* Drain all pending ZMQ messages from a SUB socket.
 * For each valid hashblock message, perform dedup and invoke callback. */
static void
drain_zmq_socket(notifier_t *n, void *sock, int sock_idx)
{
    zmq_msg_t msg;

    for (;;) {
        /* Receive topic frame */
        zmq_msg_init(&msg);
        int rc = zmq_msg_recv(&msg, sock, ZMQ_DONTWAIT);
        if (rc < 0) {
            zmq_msg_close(&msg);
            break;  /* No more messages */
        }

        /* Validate topic */
        size_t topic_size = zmq_msg_size(&msg);
        void *topic_data = zmq_msg_data(&msg);

        if (topic_size != ZMQ_TOPIC_HASHBLOCK_LEN ||
            memcmp(topic_data, ZMQ_TOPIC_HASHBLOCK, ZMQ_TOPIC_HASHBLOCK_LEN) != 0) {
            /* Unknown topic — skip remaining frames */
            zmq_msg_close(&msg);
            /* Drain any remaining parts of this multipart message */
            int more = 0;
            size_t more_size = sizeof(more);
            zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &more_size);
            while (more) {
                zmq_msg_t discard;
                zmq_msg_init(&discard);
                zmq_msg_recv(&discard, sock, ZMQ_DONTWAIT);
                zmq_msg_close(&discard);
                zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &more_size);
            }
            continue;
        }

        /* Check if there are more frames (hash frame expected) */
        int more = 0;
        size_t more_size = sizeof(more);
        zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &more_size);
        zmq_msg_close(&msg);

        if (!more) {
            log_msg(LOG_WARN, "[notifier] [zmq] hashblock message missing hash frame");
            continue;
        }

        /* Receive hash frame (32 bytes, display/RPC byte order) */
        zmq_msg_init(&msg);
        rc = zmq_msg_recv(&msg, sock, ZMQ_DONTWAIT);
        if (rc < 0) {
            zmq_msg_close(&msg);
            break;
        }

        size_t hash_size = zmq_msg_size(&msg);
        if (hash_size != HASH_SIZE) {
            log_msg(LOG_WARN, "[notifier] [zmq] hashblock hash frame unexpected "
                    "size: %zu (expected %d)", hash_size, HASH_SIZE);
            zmq_msg_close(&msg);
            /* Drain remaining frames */
            zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &more_size);
            while (more) {
                zmq_msg_t discard;
                zmq_msg_init(&discard);
                zmq_msg_recv(&discard, sock, ZMQ_DONTWAIT);
                zmq_msg_close(&discard);
                zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &more_size);
            }
            continue;
        }

        const uint8_t *hash = (const uint8_t *)zmq_msg_data(&msg);

        /* Log received hash */
        char hex[HEX_HASH_LEN];
        hex_encode(hash, HASH_SIZE, hex);
        log_msg(LOG_DEBUG, "[notifier] [zmq] hashblock received: %s", hex);

        /* Deduplication and callback (source tracking handled inside) */
        int node_idx = n->node_map[sock_idx];
        notifier_process_hash(n, hash, n->cfg->nodes[node_idx].label, "zmq");

        zmq_msg_close(&msg);

        /* Drain the sequence number frame (4 bytes LE) if present */
        zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &more_size);
        while (more) {
            zmq_msg_t discard;
            zmq_msg_init(&discard);
            zmq_msg_recv(&discard, sock, ZMQ_DONTWAIT);
            zmq_msg_close(&discard);
            zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &more_size);
        }
    }
}

/* Epoll callback for a ZMQ socket fd.
 * On EPOLLIN: check ZMQ_EVENTS for ZMQ_POLLIN, then drain.
 * Note: All ZMQ sockets in the same context share the same underlying fd,
 * so when any socket has data, we must check ALL sockets. */
static void
zmq_fd_event_cb(event_loop_t *loop, int fd, uint32_t events, void *data)
{
    (void)loop;
    (void)fd;
    (void)events;

    notifier_t *n = (notifier_t *)data;

    /* With edge-triggered epoll and a shared ZMQ fd, we must re-check
     * all sockets in a loop until none have pending messages. Reading
     * from one socket can make messages available on another (libzmq
     * shares internal signaling state across sockets in the same context). */
    bool activity;
    do {
        activity = false;
        for (int i = 0; i < n->zmq_sub_count; i++) {
            int zmq_events = 0;
            size_t opt_len = sizeof(zmq_events);
            if (zmq_getsockopt(n->zmq_subs[i], ZMQ_EVENTS, &zmq_events,
                               &opt_len) < 0) {
                log_msg(LOG_WARN, "[notifier] [zmq] zmq_getsockopt(ZMQ_EVENTS) "
                        "failed for socket %d", i);
                continue;
            }

            if (zmq_events & ZMQ_POLLIN) {
                drain_zmq_socket(n, n->zmq_subs[i], i);
                activity = true;
            }
        }
    } while (activity);
}

/* ---- downstream relay helpers ---- */

/* Publish block hash on ZMQ PUB socket (Bitcoin Core format).
 * 3-frame multipart: topic "hashblock" (9 bytes), hash (32 bytes),
 * sequence number (4 bytes LE, monotonically increasing). */
static void
relay_zmq_pub(notifier_t *n, const uint8_t *hash)
{
    if (!n->zmq_pub)
        return;

    /* Frame 1: topic "hashblock" (9 bytes, no null terminator) */
    zmq_msg_t topic_msg;
    zmq_msg_init_size(&topic_msg, ZMQ_TOPIC_HASHBLOCK_LEN);
    memcpy(zmq_msg_data(&topic_msg), ZMQ_TOPIC_HASHBLOCK,
           ZMQ_TOPIC_HASHBLOCK_LEN);

    int rc = zmq_msg_send(&topic_msg, n->zmq_pub, ZMQ_SNDMORE | ZMQ_DONTWAIT);
    if (rc < 0) {
        zmq_msg_close(&topic_msg);
        log_msg(LOG_WARN, "[notifier] [zmq] PUB send topic failed: %s",
                zmq_strerror(errno));
        return;
    }

    /* Frame 2: hash (32 bytes, display/RPC byte order) */
    zmq_msg_t hash_msg;
    zmq_msg_init_size(&hash_msg, HASH_SIZE);
    memcpy(zmq_msg_data(&hash_msg), hash, HASH_SIZE);

    rc = zmq_msg_send(&hash_msg, n->zmq_pub, ZMQ_SNDMORE | ZMQ_DONTWAIT);
    if (rc < 0) {
        zmq_msg_close(&hash_msg);
        log_msg(LOG_WARN, "[notifier] [zmq] PUB send hash failed: %s",
                zmq_strerror(errno));
        return;
    }

    /* Frame 3: sequence number (4 bytes LE, monotonically increasing) */
    uint32_t seq = n->zmq_pub_seq++;
    zmq_msg_t seq_msg;
    zmq_msg_init_size(&seq_msg, sizeof(seq));
    memcpy(zmq_msg_data(&seq_msg), &seq, sizeof(seq));

    rc = zmq_msg_send(&seq_msg, n->zmq_pub, ZMQ_DONTWAIT);
    if (rc < 0) {
        zmq_msg_close(&seq_msg);
        log_msg(LOG_WARN, "[notifier] [zmq] PUB send seq failed: %s",
                zmq_strerror(errno));
        return;
    }

    char hex[HEX_HASH_LEN];
    hex_encode(hash, HASH_SIZE, hex);
    log_msg(LOG_INFO, "[notifier] [zmq] PUB relay: %s (seq=%u)", hex, seq);
}

/* Parse an HTTP URL into host, port, and path components.
 * Expects format: http://host:port/path or http://host/path (default port 80).
 * Returns 0 on success, -1 on failure. */
static int
parse_http_url(const char *url, char *host, size_t host_len,
               uint16_t *port, char *path, size_t path_len)
{
    /* Skip "http://" prefix */
    const char *p = url;
    if (strncmp(p, "http://", 7) != 0)
        return -1;
    p += 7;

    /* Extract host (up to ':' or '/' or end) */
    const char *host_start = p;
    while (*p && *p != ':' && *p != '/')
        p++;

    size_t hlen = (size_t)(p - host_start);
    if (hlen == 0 || hlen >= host_len)
        return -1;
    memcpy(host, host_start, hlen);
    host[hlen] = '\0';

    /* Extract port if present */
    *port = 80;
    if (*p == ':') {
        p++;
        char *end = NULL;
        long pval = strtol(p, &end, 10);
        if (end == p || pval <= 0 || pval > 65535)
            return -1;
        *port = (uint16_t)pval;
        p = end;
    }

    /* Extract path (rest of URL, or "/" if none) */
    if (*p == '/') {
        size_t plen = strlen(p);
        if (plen >= path_len)
            return -1;
        memcpy(path, p, plen + 1);
    } else {
        if (path_len < 2)
            return -1;
        path[0] = '/';
        path[1] = '\0';
    }

    return 0;
}

/* Set a file descriptor to non-blocking mode. */
static int
set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Attempt to connect to the CK stratifier socket.
 * Returns the connected fd (set to O_NONBLOCK), or -1 on failure. */
static int
ck_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    log_msg(LOG_INFO, "[notifier] [ck] connected to %s", path);
    return fd;
}

/* Send the "update" command to ckpool via the CK socket relay.
 * Handles write errors, reconnection, and logging. */
static void
relay_ck_notify(notifier_t *n, const uint8_t *hash)
{
    if (!n->ck_configured)
        return;

    static const uint8_t CK_UPDATE_MSG[10] = {
        0x06, 0x00, 0x00, 0x00,       /* length prefix: 6 in LE */
        'u', 'p', 'd', 'a', 't', 'e'  /* payload */
    };

    /* Reconnect on demand if disconnected */
    if (n->ck_fd < 0) {
        n->ck_fd = ck_connect(n->ck_path);
        if (n->ck_fd < 0) {
            log_msg(LOG_WARN, "[notifier] [ck] reconnect to %s failed: %s",
                    n->ck_path, strerror(errno));
            return;
        }
    }

    /* Single non-blocking write, no retry */
    ssize_t ret = write(n->ck_fd, CK_UPDATE_MSG, sizeof(CK_UPDATE_MSG));

    if (ret == (ssize_t)sizeof(CK_UPDATE_MSG)) {
        /* Success */
        char hex[HEX_HASH_LEN];
        hex_encode(hash, HASH_SIZE, hex);
        log_msg(LOG_INFO, "[notifier] [ck] relay: %s", hex);
    } else if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            log_msg(LOG_WARN, "[notifier] [ck] write would block, dropping notification");
        } else {
            log_msg(LOG_WARN, "[notifier] [ck] write failed: %s", strerror(errno));
            close(n->ck_fd);
            n->ck_fd = -1;
        }
    } else {
        /* Partial write (0 < ret < 10) */
        log_msg(LOG_WARN, "[notifier] [ck] partial write (%zd/10), disconnecting",
                ret);
        close(n->ck_fd);
        n->ck_fd = -1;
    }
}

/* Attempt to establish a non-blocking TCP connection to the HTTP notify host.
 * Returns the fd on success (may be in-progress), -1 on failure. */
static int
http_notify_connect(notifier_t *n)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(n->notify_http_port);

    if (inet_pton(AF_INET, n->notify_http_host, &addr.sin_addr) != 1) {
        /* Try resolving hostname */
        struct hostent *he = gethostbyname(n->notify_http_host);
        if (!he) {
            log_msg(LOG_WARN, "[notifier] [http] cannot resolve '%s'",
                    n->notify_http_host);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_msg(LOG_WARN, "[notifier] [http] socket() failed: %s",
                strerror(errno));
        return -1;
    }

    if (set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    /* Set TCP_NODELAY for low-latency writes */
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        log_msg(LOG_WARN, "[notifier] [http] connect() failed: %s",
                strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* Relay block hash via HTTP GET to the configured notify URL.
 * Non-blocking write; drops notification if write would block. */
static void
relay_http_notify(notifier_t *n, const uint8_t *hash)
{
    if (!n->notify_http_configured)
        return;

    /* Build the request path with hash substitution */
    char path_buf[1024];
    char hex[HEX_HASH_LEN];
    hex_encode(hash, HASH_SIZE, hex);

    /* Check if path contains %s for substitution */
    const char *pct = strstr(n->notify_http_path, "%s");
    if (pct) {
        /* Substitute %s with 64-char hex hash */
        size_t prefix_len = (size_t)(pct - n->notify_http_path);
        const char *suffix = pct + 2;  /* skip "%s" */
        int written = snprintf(path_buf, sizeof(path_buf), "%.*s%s%s",
                               (int)prefix_len, n->notify_http_path,
                               hex, suffix);
        if (written < 0 || (size_t)written >= sizeof(path_buf)) {
            log_msg(LOG_WARN, "[notifier] [http] path too long");
            return;
        }
    } else {
        /* Use path as-is */
        size_t plen = strlen(n->notify_http_path);
        if (plen >= sizeof(path_buf)) {
            log_msg(LOG_WARN, "[notifier] [http] path too long");
            return;
        }
        memcpy(path_buf, n->notify_http_path, plen + 1);
    }

    /* Build HTTP GET request */
    char request[2048];
    int req_len = snprintf(request, sizeof(request),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s:%u\r\n"
                           "Connection: keep-alive\r\n"
                           "\r\n",
                           path_buf, n->notify_http_host,
                           (unsigned)n->notify_http_port);
    if (req_len < 0 || (size_t)req_len >= sizeof(request)) {
        log_msg(LOG_WARN, "[notifier] [http] request too long");
        return;
    }

    /* If not connected, attempt to connect */
    if (n->notify_http_fd < 0 ||
        n->notify_http_state == HTTP_STATE_DISCONNECTED) {
        if (n->notify_http_fd >= 0) {
            close(n->notify_http_fd);
            n->notify_http_fd = -1;
        }
        n->notify_http_fd = http_notify_connect(n);
        if (n->notify_http_fd < 0) {
            log_msg(LOG_WARN, "[notifier] [http] connection failed, "
                    "dropping notification for %s", hex);
            return;
        }
        n->notify_http_state = HTTP_STATE_CONNECTED;
    }

    /* Non-blocking write — drop if would block */
    ssize_t sent = write(n->notify_http_fd, request, (size_t)req_len);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            log_msg(LOG_WARN, "[notifier] [http] write would block, "
                    "dropping notification for %s", hex);
        } else {
            /* Connection broken — close and try reconnect on next notify */
            log_msg(LOG_WARN, "[notifier] [http] write failed (%s), "
                    "closing connection", strerror(errno));
            close(n->notify_http_fd);
            n->notify_http_fd = -1;
            n->notify_http_state = HTTP_STATE_DISCONNECTED;
        }
        return;
    }

    if (sent < req_len) {
        /* Partial write — treat as would-block, drop remainder.
         * The connection may be in a bad state now; close it. */
        log_msg(LOG_WARN, "[notifier] [http] partial write (%zd/%d), "
                "closing connection", sent, req_len);
        close(n->notify_http_fd);
        n->notify_http_fd = -1;
        n->notify_http_state = HTTP_STATE_DISCONNECTED;
        return;
    }

    log_msg(LOG_INFO, "[notifier] [http] relay: %s", hex);

    /* Drain any pending response data (non-blocking read, discard) */
    char discard_buf[4096];
    for (;;) {
        ssize_t rd = read(n->notify_http_fd, discard_buf, sizeof(discard_buf));
        if (rd <= 0)
            break;  /* EAGAIN or EOF */
    }

    /* Check if remote closed the connection (read returned 0) */
    /* If read returned 0 (EOF), the remote closed — mark disconnected */
    /* Note: if read returned -1 with EAGAIN, connection is still alive */
}

/* ---- silence detection timer callback ---- */

/* Periodic timer callback: check if any notification sources have gone
 * silent — i.e. have not reported any new (different) hash within 60 seconds
 * of a block event. A node that reports *any* new hash (even a different one)
 * is considered healthy. Only nodes whose pipeline appears dead are warned. */
static void
silence_check_cb(event_loop_t *loop, void *data)
{
    (void)loop;
    notifier_t *n = (notifier_t *)data;

    /* Nothing to check if we haven't received any notification yet */
    if (n->first_notify_ns == 0)
        return;

    uint64_t now = clock_monotonic_ns();
    uint64_t elapsed = now - n->first_notify_ns;

    /* Only check after 60 seconds have passed since the block event */
    if (elapsed < SILENCE_THRESHOLD_NS)
        return;

    /* Check each node that has a notification source configured */
    for (int i = 0; i < n->cfg->node_count; i++) {
        /* Skip nodes without ZMQ configured (they use HTTP which is
         * triggered externally and we can't track per-node) */
        if (n->cfg->nodes[i].zmq_port == 0)
            continue;

        /* Skip nodes in IBD — they don't publish ZMQ during initial sync */
        if (n->proxy && rpc_proxy_is_node_ibd((rpc_proxy_t *)n->proxy, i))
            continue;

        /* Already warned for this block event? */
        if (n->silence_warned[i])
            continue;

        /* A node is healthy if it reported any new hash (different from its
         * previous) since the block event. Check if node_hash_changed_ns
         * is at or after the block event timestamp. */
        if (n->node_has_hash[i] &&
            n->node_hash_changed_ns[i] >= n->first_notify_ns)
            continue;  /* Node reported something new — pipeline is alive */

        /* This node's pipeline appears dead — no new hash in 60s */
        char hex[HEX_HASH_LEN];
        hex_encode(n->last_hash, HASH_SIZE, hex);
        log_msg(LOG_WARN, "[notifier] Node '%s' did not report any new block "
                "within 60s (last accepted: %s)",
                n->cfg->nodes[i].label, hex);
        n->silence_warned[i] = true;
    }
}

/* ---- public API ---- */

notifier_t *
notifier_create(event_loop_t *loop, config_t *cfg, notify_cb cb, void *data)
{
    if (!loop || !cfg || !cb)
        return NULL;

    notifier_t *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;

    n->loop = loop;
    n->cfg = cfg;
    n->on_notify = cb;
    n->cb_data = data;
    n->has_last_hash = false;
    n->zmq_sub_count = 0;
    n->zmq_pub = NULL;
    n->zmq_pub_seq = 0;
    n->notify_http_fd = -1;
    n->notify_http_state = HTTP_STATE_DISCONNECTED;
    n->notify_http_configured = false;
    n->first_notify_ns = 0;
    n->total_notify_sources = 0;
    n->grace_active = false;
    n->grace_start_ns = 0;
    n->grace_winner_node_idx = -1;
    n->stats = NULL;
    n->proxy = NULL;

    /* Initialize per-node silence tracking */
    for (int i = 0; i < MAX_NODES; i++) {
        memset(n->node_last_hash[i], 0, HASH_SIZE);
        n->node_has_hash[i] = false;
        n->node_hash_changed_ns[i] = 0;
        n->silence_warned[i] = false;
        n->node_map[i] = -1;
    }

    /* Create ZMQ context */
    n->zmq_ctx = zmq_ctx_new();
    if (!n->zmq_ctx) {
        log_msg(LOG_CRIT, "[notifier] [zmq] Failed to create context");
        free(n);
        return NULL;
    }

    /* Create SUB sockets for each node with a zmq_port configured */
    for (int i = 0; i < cfg->node_count; i++) {
        if (cfg->nodes[i].zmq_port == 0)
            continue;

        void *sock = zmq_socket(n->zmq_ctx, ZMQ_SUB);
        if (!sock) {
            log_msg(LOG_CRIT, "[notifier] [zmq] Failed to create SUB socket "
                    "for node '%s'", cfg->nodes[i].label);
            goto fail;
        }

        /* Subscribe to "hashblock" topic */
        if (zmq_setsockopt(sock, ZMQ_SUBSCRIBE, ZMQ_TOPIC_HASHBLOCK,
                           ZMQ_TOPIC_HASHBLOCK_LEN) < 0) {
            log_msg(LOG_CRIT, "[notifier] [zmq] Failed to subscribe to hashblock "
                    "for node '%s'", cfg->nodes[i].label);
            zmq_close(sock);
            goto fail;
        }

        /* Construct and connect to the node's ZMQ endpoint */
        char endpoint[300];
        snprintf(endpoint, sizeof(endpoint), "tcp://%s:%u",
                 cfg->nodes[i].host, cfg->nodes[i].zmq_port);
        if (zmq_connect(sock, endpoint) < 0) {
            log_msg(LOG_CRIT, "[notifier] [zmq] Failed to connect SUB to '%s' "
                    "for node '%s'", endpoint, cfg->nodes[i].label);
            zmq_close(sock);
            goto fail;
        }

        /* Get the underlying fd for epoll integration */
        int zmq_fd = -1;
        size_t fd_len = sizeof(zmq_fd);
        if (zmq_getsockopt(sock, ZMQ_FD, &zmq_fd, &fd_len) < 0 ||
            zmq_fd < 0) {
            log_msg(LOG_CRIT, "[notifier] [zmq] Failed to get ZMQ_FD for node '%s'",
                    cfg->nodes[i].label);
            zmq_close(sock);
            goto fail;
        }

        /* Register the ZMQ fd with epoll (edge-triggered).
         * All ZMQ sockets in the same context share the same fd,
         * so only register it once (first socket). */
        if (n->zmq_sub_count == 0) {
            if (event_loop_add_fd(loop, zmq_fd, EPOLLIN | EPOLLET,
                                  zmq_fd_event_cb, n) < 0) {
                log_msg(LOG_CRIT, "[notifier] [zmq] Failed to register fd with "
                        "event loop for node '%s'", cfg->nodes[i].label);
                zmq_close(sock);
                goto fail;
            }
        }

        int idx = n->zmq_sub_count;
        n->zmq_subs[idx] = sock;
        n->zmq_fds[idx] = zmq_fd;
        n->node_map[idx] = i;  /* map sub index → config node index */
        n->zmq_sub_count++;
        n->total_notify_sources++;

        log_msg(LOG_INFO, "[notifier] [zmq] Subscribed to hashblock on '%s' (%s)",
                cfg->nodes[i].label, endpoint);
    }

    if (n->zmq_sub_count == 0) {
        log_msg(LOG_WARN, "[notifier] [zmq] No subscriptions configured");
    }

    /* Initialize ZMQ PUB socket for downstream relay (Req 3.2) */
    if (cfg->zmq_server_port != 0) {
        n->zmq_pub = zmq_socket(n->zmq_ctx, ZMQ_PUB);
        if (!n->zmq_pub) {
            log_msg(LOG_CRIT, "[notifier] [zmq] Failed to create PUB socket");
            goto fail;
        }

        char endpoint[128];
        snprintf(endpoint, sizeof(endpoint), "tcp://%s:%u",
                 cfg->zmq_server_bind, cfg->zmq_server_port);

        if (zmq_bind(n->zmq_pub, endpoint) < 0) {
            log_msg(LOG_CRIT, "[notifier] [zmq] Failed to bind PUB to '%s': %s",
                    endpoint, zmq_strerror(errno));
            goto fail;
        }

        log_msg(LOG_INFO, "[notifier] [zmq] PUB bound on '%s'", endpoint);
    }

    /* Parse HTTP notify URL for downstream relay (Req 3.3, 3.4) */
    if (cfg->notify_http_url[0] != '\0') {
        if (parse_http_url(cfg->notify_http_url,
                           n->notify_http_host, sizeof(n->notify_http_host),
                           &n->notify_http_port,
                           n->notify_http_path,
                           sizeof(n->notify_http_path)) < 0) {
            log_msg(LOG_CRIT, "[notifier] [http] Failed to parse notify_http_url: '%s'",
                    cfg->notify_http_url);
            goto fail;
        }
        n->notify_http_configured = true;
        log_msg(LOG_INFO, "[notifier] [http] notify configured: %s:%u%s",
                n->notify_http_host, (unsigned)n->notify_http_port,
                n->notify_http_path);
    }

    /* Initialize CK socket relay state */
    memcpy(n->ck_path, cfg->ck_notify_socket, sizeof(n->ck_path));
    n->ck_configured = (n->ck_path[0] != '\0');
    n->ck_fd = -1;

    /* Attempt initial CK socket connection */
    if (n->ck_configured) {
        n->ck_fd = ck_connect(n->ck_path);
        if (n->ck_fd < 0) {
            log_msg(LOG_WARN, "[notifier] [ck] initial connect to %s failed: %s",
                    n->ck_path, strerror(errno));
            /* Non-fatal — will retry on first notification */
        }
    }

    /* Register silence detection timer (checks every 10s) */
    if (n->total_notify_sources > 1) {
        if (event_loop_add_timer(loop, SILENCE_CHECK_INTERVAL_MS,
                                 silence_check_cb, n) < 0) {
            log_msg(LOG_WARN, "[notifier] Failed to create silence "
                    "detection timer");
            /* Non-fatal — continue without silence detection */
        }
    }

    return n;

fail:
    notifier_destroy(n);
    return NULL;
}

void
notifier_destroy(notifier_t *n)
{
    if (!n)
        return;

    /* Remove ZMQ fds from event loop and close sockets */
    for (int i = 0; i < n->zmq_sub_count; i++) {
        if (n->zmq_fds[i] >= 0 && n->loop) {
            event_loop_del_fd(n->loop, n->zmq_fds[i]);
        }
        if (n->zmq_subs[i]) {
            zmq_close(n->zmq_subs[i]);
        }
    }

    /* Close PUB socket if created */
    if (n->zmq_pub) {
        zmq_close(n->zmq_pub);
    }

    /* Close HTTP notify socket if open */
    if (n->notify_http_fd >= 0) {
        close(n->notify_http_fd);
    }

    /* Close CK socket relay if connected */
    if (n->ck_fd >= 0) {
        close(n->ck_fd);
    }

    /* Destroy ZMQ context */
    if (n->zmq_ctx) {
        zmq_ctx_destroy(n->zmq_ctx);
    }

    free(n);
}

void
notifier_set_stats(notifier_t *n, stats_t *stats)
{
    if (n)
        n->stats = stats;
}

void
notifier_set_proxy(notifier_t *n, struct rpc_proxy *proxy)
{
    if (n)
        n->proxy = proxy;
}

int
notifier_process_hash(notifier_t *n, const uint8_t *hash,
                      const char *node_label, const char *protocol)
{
    if (!n || !hash)
        return 0;

    /* Deduplication check */
    if (is_duplicate(n, hash)) {
        uint64_t now = clock_monotonic_ns();
        uint64_t offset_us = 0;
        if (n->first_notify_ns > 0)
            offset_us = (now - n->first_notify_ns) / 1000;

        char hex[HEX_HASH_LEN];
        hex_encode(hash, HASH_SIZE, hex);
        log_msg(LOG_INFO, "[notifier] [%s] Duplicate block notify from %s: %s "
                "(offset_us=%llu)",
                protocol ? protocol : "?",
                node_label ? node_label : "unknown", hex,
                (unsigned long long)offset_us);

        /* Still record that this node reported the block (for silence detection) */
        if (node_label) {
            for (int i = 0; i < n->cfg->node_count; i++) {
                if (strcmp(n->cfg->nodes[i].label, node_label) == 0) {
                    /* If this hash differs from the node's previous hash,
                     * update the change timestamp (pipeline is alive). */
                    if (!n->node_has_hash[i] ||
                        memcmp(n->node_last_hash[i], hash, HASH_SIZE) != 0) {
                        n->node_hash_changed_ns[i] = now;
                    }
                    memcpy(n->node_last_hash[i], hash, HASH_SIZE);
                    n->node_has_hash[i] = true;
                    break;
                }
            }
        }
        return 0;
    }

    /* Store as last_hash (tentative — may be reverted if suppressed) */
    /* We need the hash in last_hash for the duplicate check to work on
     * subsequent messages, but first_notify_ns should only update on accept. */

    /* Record timestamp for this event */
    uint64_t now = clock_monotonic_ns();

    /* Resolve the source node index and update per-node tracking.
     * This happens regardless of grace suppression so that silence
     * detection knows the node's pipeline is alive. */
    int source_node_idx = -1;
    if (node_label) {
        for (int i = 0; i < n->cfg->node_count; i++) {
            if (strcmp(n->cfg->nodes[i].label, node_label) == 0) {
                source_node_idx = i;
                /* Update changed timestamp if hash differs from node's previous */
                if (!n->node_has_hash[i] ||
                    memcmp(n->node_last_hash[i], hash, HASH_SIZE) != 0) {
                    n->node_hash_changed_ns[i] = now;
                }
                memcpy(n->node_last_hash[i], hash, HASH_SIZE);
                n->node_has_hash[i] = true;
                break;
            }
        }
    }

    /* Grace period check: if a recent notify was accepted, suppress notifies
     * from nodes other than the winning notify node for 10 seconds.
     * This prevents late/stale notifies from remote nodes triggering
     * unnecessary GBT races. */
    if (n->grace_active) {
        uint64_t elapsed = now - n->grace_start_ns;
        if (elapsed < NOTIFY_GRACE_PERIOD_NS &&
            source_node_idx != n->grace_winner_node_idx) {
            /* Suppressed — log and relay downstream but don't invoke callback.
             * Do NOT update last_hash or first_notify_ns for suppressed events. */
            char hex[HEX_HASH_LEN];
            hex_encode(hash, HASH_SIZE, hex);
            log_msg(LOG_INFO, "[notifier] [%s] Suppressed block notify from %s: "
                    "%s (grace period, winning node: %s)",
                    protocol ? protocol : "?",
                    node_label ? node_label : "unknown", hex,
                    n->grace_winner_node_idx >= 0
                        ? n->cfg->nodes[n->grace_winner_node_idx].label
                        : "unknown");

            /* Still relay downstream (subscribers may want all notifications) */
            relay_zmq_pub(n, hash);
            relay_http_notify(n, hash);
            relay_ck_notify(n, hash);
            return 0;
        }
        /* Grace period expired or winning node reported — fall through to accept */
    }

    /* Notification accepted — update global state */
    memcpy(n->last_hash, hash, HASH_SIZE);
    n->has_last_hash = true;
    n->first_notify_ns = now;
    for (int i = 0; i < MAX_NODES; i++) {
        n->silence_warned[i] = false;
    }

    /* Log the new block notification (Req 9.5) */
    char hex[HEX_HASH_LEN];
    hex_encode(hash, HASH_SIZE, hex);
    log_msg(LOG_INFO, "[notifier] [%s] New block notify from %s: %s",
            protocol ? protocol : "?",
            node_label ? node_label : "unknown", hex);

    /* Start/reset grace period: this node is the new winning notify node */
    n->grace_active = true;
    n->grace_start_ns = now;
    n->grace_winner_node_idx = source_node_idx;

    /* Record notify win in stats */
    if (n->stats && source_node_idx >= 0) {
        stats_record_notify_win(n->stats, source_node_idx);
    }

    /* Invoke callback */
    n->on_notify(hash, n->cb_data);

    /* Downstream relay: ZMQ PUB (Req 3.1, 3.2) */
    relay_zmq_pub(n, hash);

    /* Downstream relay: HTTP notify (Req 3.3, 3.4) */
    relay_http_notify(n, hash);

    /* Downstream relay: CK socket notify */
    relay_ck_notify(n, hash);

    return 1;
}
