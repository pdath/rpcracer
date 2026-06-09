/* rpc_conn.c — Upstream HTTP/1.1 connection management */

#include "rpc_conn.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

/* ---- internal helpers ---- */

static void
configure_socket(int fd, const char *label)
{
    int flag = 1;

    /* TCP_NODELAY — disable Nagle's algorithm */
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0)
        log_msg(LOG_WARN, "[%s] setsockopt TCP_NODELAY failed: %s",
                label, strerror(errno));

    /* SO_KEEPALIVE — detect dead peers */
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) < 0)
        log_msg(LOG_WARN, "[%s] setsockopt SO_KEEPALIVE failed: %s",
                label, strerror(errno));

    /* SO_SNDBUF */
    int bufsize = SOCKET_BUF_SIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) < 0)
        log_msg(LOG_WARN, "[%s] setsockopt SO_SNDBUF failed: %s",
                label, strerror(errno));

    int actual = 0;
    socklen_t optlen = sizeof(actual);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual, &optlen) == 0) {
        /* Linux doubles the value internally; only warn if actual < requested
         * (meaning the kernel couldn't allocate what we asked for) */
        if (actual < bufsize)
            log_msg(LOG_WARN, "[%s] SO_SNDBUF: requested %d, got %d "
                    "(check net.core.wmem_max)", label, bufsize, actual);
    }

    /* SO_RCVBUF */
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0)
        log_msg(LOG_WARN, "[%s] setsockopt SO_RCVBUF failed: %s",
                label, strerror(errno));

    actual = 0;
    optlen = sizeof(actual);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &optlen) == 0) {
        if (actual < bufsize)
            log_msg(LOG_WARN, "[%s] SO_RCVBUF: requested %d, got %d "
                    "(check net.core.rmem_max)", label, bufsize, actual);
    }
}

/* Parse HTTP headers from recv_buf. Returns 1 if headers are complete. */
static int
parse_headers(upstream_conn_t *conn)
{
    if (conn->headers_complete)
        return 1;

    /* Look for end of headers: \r\n\r\n */
    const char *buf = (const char *)conn->recv_buf;
    const char *end = NULL;

    for (size_t i = 0; i + 3 < conn->recv_len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            end = buf + i + 4;
            break;
        }
    }

    if (!end)
        return 0;  /* headers not yet complete */

    conn->header_len = (size_t)(end - buf);
    conn->headers_complete = 1;

    /* Parse Content-Length */
    conn->content_length = 0;
    const char *cl = strcasestr(buf, "Content-Length:");
    if (cl && cl < end) {
        cl += 15;  /* skip "Content-Length:" */
        while (*cl == ' ' || *cl == '\t')
            cl++;
        conn->content_length = (size_t)strtoull(cl, NULL, 10);
    }

    /* Parse Connection: close */
    conn->connection_close = 0;
    const char *ch = strcasestr(buf, "Connection:");
    if (ch && ch < end) {
        ch += 11;  /* skip "Connection:" */
        while (*ch == ' ' || *ch == '\t')
            ch++;
        if (strncasecmp(ch, "close", 5) == 0)
            conn->connection_close = 1;
    }

    return 1;
}

/* Return a human-readable name for a connection state. */
static const char *
conn_state_name(conn_state_t state)
{
    switch (state) {
    case CONN_DISCONNECTED: return "DISCONNECTED";
    case CONN_CONNECTING:   return "CONNECTING";
    case CONN_CONNECTED:    return "CONNECTED";
    case CONN_SENDING:      return "SENDING";
    case CONN_RECEIVING:    return "RECEIVING";
    default:                return "UNKNOWN";
    }
}

/* epoll event handler for this connection's fd */
static void
conn_epoll_cb(event_loop_t *loop, int fd, uint32_t events, void *data)
{
    (void)loop;
    (void)fd;

    upstream_conn_t *conn = (upstream_conn_t *)data;

    if (events & (EPOLLERR | EPOLLHUP)) {
        /* Connection error or hangup */
        log_msg(LOG_WARN, "[%s] Connection error/hangup (state=%s)",
                conn->config->label, conn_state_name(conn->state));
        event_loop_del_fd(conn->loop, conn->fd);
        close(conn->fd);
        conn->fd = -1;

        conn_state_t prev_state = conn->state;
        conn->state = CONN_DISCONNECTED;

        /* Mid-transfer disconnect: treat as error */
        if (prev_state == CONN_SENDING || prev_state == CONN_RECEIVING) {
            if (conn->cb.on_error)
                conn->cb.on_error(conn, conn->cb.data);
        }

        return;
    }

    switch (conn->state) {
    case CONN_CONNECTING:
        if (events & EPOLLOUT) {
            /* Check if connect() succeeded */
            int err = 0;
            socklen_t errlen = sizeof(err);
            if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0
                || err != 0) {
                log_msg(LOG_WARN, "[%s] connect() failed: %s",
                        conn->config->label, strerror(err ? err : errno));
                event_loop_del_fd(conn->loop, conn->fd);
                close(conn->fd);
                conn->fd = -1;
                conn->state = CONN_DISCONNECTED;
                return;
            }

            /* Connection established */
            conn->state = CONN_CONNECTED;

            /* Switch to EPOLLIN for receiving; EPOLLOUT added when sending */
            event_loop_mod_fd(conn->loop, conn->fd, EPOLLIN);

            log_msg(LOG_DEBUG, "[%s] Connected to %s:%u",
                    conn->config->label,
                    conn->config->host,
                    conn->config->rpc_port);

            if (conn->cb.on_connected)
                conn->cb.on_connected(conn, conn->cb.data);
        }
        break;

    case CONN_SENDING:
        if (events & EPOLLOUT) {
            /* Continue sending */
            size_t remaining = conn->send_len - conn->send_offset;
            ssize_t n = send(conn->fd,
                             conn->send_buf + conn->send_offset,
                             remaining, MSG_NOSIGNAL);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;  /* try again on next EPOLLOUT */
                log_msg(LOG_WARN, "[%s] send() failed: %s",
                        conn->config->label, strerror(errno));
                event_loop_del_fd(conn->loop, conn->fd);
                close(conn->fd);
                conn->fd = -1;
                conn->state = CONN_DISCONNECTED;
                if (conn->cb.on_error)
                    conn->cb.on_error(conn, conn->cb.data);
                return;
            }

            conn->send_offset += (size_t)n;

            if (conn->send_offset >= conn->send_len) {
                /* All bytes sent — transition to RECEIVING */
                conn->state = CONN_RECEIVING;
                conn->recv_len = 0;
                conn->headers_complete = 0;
                conn->content_length = 0;
                conn->header_len = 0;
                conn->connection_close = 0;

                /* Only need EPOLLIN now */
                event_loop_mod_fd(conn->loop, conn->fd, EPOLLIN);

                if (conn->cb.on_send_complete)
                    conn->cb.on_send_complete(conn, conn->cb.data);
            }
        }
        break;

    case CONN_RECEIVING:
        if (events & EPOLLIN) {
            /* Read available data */
            size_t space = conn->recv_cap - conn->recv_len;
            if (space == 0) {
                log_msg(LOG_WARN, "[%s] recv_buf full (%zu bytes)",
                        conn->config->label, conn->recv_cap);
                /* Treat as error — response too large */
                event_loop_del_fd(conn->loop, conn->fd);
                close(conn->fd);
                conn->fd = -1;
                conn->state = CONN_DISCONNECTED;
                if (conn->cb.on_error)
                    conn->cb.on_error(conn, conn->cb.data);
                return;
            }

            ssize_t n = recv(conn->fd,
                             conn->recv_buf + conn->recv_len,
                             space, 0);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                log_msg(LOG_WARN, "[%s] recv() failed: %s",
                        conn->config->label, strerror(errno));
                event_loop_del_fd(conn->loop, conn->fd);
                close(conn->fd);
                conn->fd = -1;
                conn->state = CONN_DISCONNECTED;
                if (conn->cb.on_error)
                    conn->cb.on_error(conn, conn->cb.data);
                return;
            }

            if (n == 0) {
                /* Peer closed connection mid-transfer */
                log_msg(LOG_WARN, "[%s] Connection closed mid-receive",
                        conn->config->label);
                event_loop_del_fd(conn->loop, conn->fd);
                close(conn->fd);
                conn->fd = -1;
                conn->state = CONN_DISCONNECTED;
                if (conn->cb.on_error)
                    conn->cb.on_error(conn, conn->cb.data);
                return;
            }

            conn->recv_len += (size_t)n;

            /* Try to parse headers and check if response is complete */
            if (parse_headers(conn) && rpc_conn_response_complete(conn)) {
                if (conn->cb.on_response)
                    conn->cb.on_response(conn, conn->cb.data);
            }
        }
        break;

    case CONN_CONNECTED:
        /* Unexpected data on idle connection (e.g., server closing) */
        if (events & EPOLLIN) {
            char tmp[1];
            ssize_t n = recv(conn->fd, tmp, sizeof(tmp), MSG_PEEK);
            if (n == 0) {
                /* Server closed the connection */
                log_msg(LOG_INFO, "[%s] Server closed idle connection",
                        conn->config->label);
                event_loop_del_fd(conn->loop, conn->fd);
                close(conn->fd);
                conn->fd = -1;
                conn->state = CONN_DISCONNECTED;
            }
        }
        break;

    case CONN_DISCONNECTED:
        /* Should not receive events in this state */
        break;
    }
}

/* ---- public API ---- */

int
rpc_conn_init(upstream_conn_t *conn, event_loop_t *loop,
              node_config_t *config, int node_index,
              const conn_callbacks_t *callbacks)
{
    if (!conn || !loop || !config)
        return -1;

    memset(conn, 0, sizeof(*conn));

    conn->fd = -1;
    conn->state = CONN_DISCONNECTED;
    conn->config = config;
    conn->loop = loop;
    conn->node_index = node_index;

    conn->send_buf = NULL;
    conn->send_len = 0;
    conn->send_offset = 0;

    conn->recv_buf = malloc(SOCKET_BUF_SIZE);
    if (!conn->recv_buf)
        return -1;
    conn->recv_len = 0;
    conn->recv_cap = SOCKET_BUF_SIZE;

    conn->content_length = 0;
    conn->header_len = 0;
    conn->headers_complete = 0;
    conn->connection_close = 0;

    conn->request_start_ns = 0;

    if (callbacks)
        conn->cb = *callbacks;

    return 0;
}

int
rpc_conn_connect(upstream_conn_t *conn)
{
    if (!conn || conn->state != CONN_DISCONNECTED)
        return -1;

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_msg(LOG_WARN, "[%s] socket() failed: %s",
                conn->config->label, strerror(errno));
        return -1;
    }

    configure_socket(fd, conn->config->label);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(conn->config->rpc_port);

    if (inet_pton(AF_INET, conn->config->host, &addr.sin_addr) != 1) {
        log_msg(LOG_WARN, "[%s] Invalid address: %s",
                conn->config->label, conn->config->host);
        close(fd);
        return -1;
    }

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        log_msg(LOG_WARN, "[%s] connect() failed: %s",
                conn->config->label, strerror(errno));
        close(fd);
        return -1;
    }

    conn->fd = fd;

    if (ret == 0) {
        /* Immediate connection (e.g., localhost) */
        conn->state = CONN_CONNECTED;

        if (event_loop_add_fd(conn->loop, fd, EPOLLIN,
                              conn_epoll_cb, conn) < 0) {
            log_msg(LOG_WARN, "[%s] event_loop_add_fd failed",
                    conn->config->label);
            close(fd);
            conn->fd = -1;
            conn->state = CONN_DISCONNECTED;
            return -1;
        }

        log_msg(LOG_DEBUG, "[%s] Connected to %s:%u (immediate)",
                conn->config->label,
                conn->config->host,
                conn->config->rpc_port);

        if (conn->cb.on_connected)
            conn->cb.on_connected(conn, conn->cb.data);
    } else {
        /* Connection in progress — wait for EPOLLOUT */
        conn->state = CONN_CONNECTING;

        if (event_loop_add_fd(conn->loop, fd, EPOLLOUT,
                              conn_epoll_cb, conn) < 0) {
            log_msg(LOG_WARN, "[%s] event_loop_add_fd failed",
                    conn->config->label);
            close(fd);
            conn->fd = -1;
            conn->state = CONN_DISCONNECTED;
            return -1;
        }

        log_msg(LOG_DEBUG, "[%s] Connecting to %s:%u...",
                conn->config->label,
                conn->config->host,
                conn->config->rpc_port);
    }

    return 0;
}

int
rpc_conn_send(upstream_conn_t *conn, const uint8_t *buf, size_t len)
{
    if (!conn || !buf || len == 0)
        return -1;

    if (conn->state != CONN_CONNECTED)
        return -1;

    conn->send_buf = buf;
    conn->send_len = len;
    conn->send_offset = 0;
    conn->request_start_ns = clock_monotonic_ns();
    conn->state = CONN_SENDING;

    /* Enable EPOLLOUT to start sending */
    event_loop_mod_fd(conn->loop, conn->fd, EPOLLIN | EPOLLOUT);

    return 0;
}

int
rpc_conn_response_complete(const upstream_conn_t *conn)
{
    if (!conn || !conn->headers_complete)
        return 0;

    /* Response is complete when we have header_len + content_length bytes */
    size_t expected = conn->header_len + conn->content_length;
    return conn->recv_len >= expected;
}

int
rpc_conn_get_response(const upstream_conn_t *conn,
                      const uint8_t **body, size_t *body_len)
{
    if (!conn || !body || !body_len)
        return -1;

    if (!rpc_conn_response_complete(conn))
        return -1;

    *body = conn->recv_buf + conn->header_len;
    *body_len = conn->content_length;
    return 0;
}

void
rpc_conn_disconnect(upstream_conn_t *conn)
{
    if (!conn)
        return;

    if (conn->fd >= 0) {
        event_loop_del_fd(conn->loop, conn->fd);
        close(conn->fd);
        conn->fd = -1;
    }

    conn->state = CONN_DISCONNECTED;
    conn->send_buf = NULL;
    conn->send_len = 0;
    conn->send_offset = 0;
    conn->recv_len = 0;
    conn->headers_complete = 0;
    conn->content_length = 0;
    conn->header_len = 0;
    conn->connection_close = 0;
}

void
rpc_conn_reset(upstream_conn_t *conn)
{
    if (!conn)
        return;

    /* Clear send buffer pointer (zero-copy: we don't own it) */
    conn->send_buf = NULL;
    conn->send_len = 0;
    conn->send_offset = 0;

    /* Reset receive state */
    conn->recv_len = 0;
    conn->headers_complete = 0;
    conn->content_length = 0;
    conn->header_len = 0;
    conn->request_start_ns = 0;

    if (conn->connection_close) {
        /* Server requested close — disconnect only; rotation handles reconnect */
        log_msg(LOG_INFO, "[%s] Server sent Connection: close, disconnecting",
                conn->config->label);
        conn->connection_close = 0;
        rpc_conn_disconnect(conn);
    } else if (conn->state == CONN_RECEIVING || conn->state == CONN_SENDING) {
        /* Return to connected state */
        conn->state = CONN_CONNECTED;
        /* Ensure we're only watching for EPOLLIN */
        if (conn->fd >= 0)
            event_loop_mod_fd(conn->loop, conn->fd, EPOLLIN);
    }
    /* If already CONNECTED or DISCONNECTED, no state change needed */
}

void
rpc_conn_destroy(upstream_conn_t *conn)
{
    if (!conn)
        return;

    rpc_conn_disconnect(conn);

    free(conn->recv_buf);
    conn->recv_buf = NULL;
    conn->recv_cap = 0;
}
