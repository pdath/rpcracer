/* http_server.c — Minimal HTTP server for /NOTIFY and /stats endpoints */

#include "http_server.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_MAX_REQUEST  2048   /* max request size we'll read */
#define HTTP_MAX_RESPONSE 65536  /* max response buffer for /stats */

/* Per-client connection state */
typedef struct http_client {
    int fd;
    http_server_t *server;
    char buf[HTTP_MAX_REQUEST];
    size_t buf_len;
    char peer_addr[64];  /* client IP address string */
} http_client_t;

struct http_server {
    int listen_fd;
    event_loop_t *loop;
    config_t *cfg;
    notifier_t *notifier;
    stats_handler_cb stats_cb;
    void *stats_data;
};

/* Forward declarations */
static void http_accept_cb(event_loop_t *loop, int fd, uint32_t events, void *data);
static void http_client_cb(event_loop_t *loop, int fd, uint32_t events, void *data);
static void http_client_destroy(http_client_t *client);
static void http_send_response(http_client_t *client, int status_code,
                               const char *status_text,
                               const char *content_type,
                               const char *body, size_t body_len);
static void http_handle_request(http_client_t *client);

/* ---------- Helper: set fd non-blocking ---------- */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------- Helper: validate hex character ---------- */
static int is_hex_char(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* ---------- Public API ---------- */

http_server_t *http_server_create(event_loop_t *loop, config_t *cfg,
                                   notifier_t *notifier,
                                   stats_handler_cb stats_cb, void *stats_data)
{
    http_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv)
        return NULL;

    srv->loop = loop;
    srv->cfg = cfg;
    srv->notifier = notifier;
    srv->stats_cb = stats_cb;
    srv->stats_data = stats_data;
    srv->listen_fd = -1;

    /* Create listening socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_msg(LOG_CRIT, "[http] socket(): %s", strerror(errno));
        free(srv);
        return NULL;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (set_nonblocking(fd) < 0) {
        log_msg(LOG_CRIT, "[http] fcntl(O_NONBLOCK): %s", strerror(errno));
        close(fd);
        free(srv);
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->http_server_port);

    if (inet_pton(AF_INET, cfg->http_server_bind, &addr.sin_addr) != 1) {
        log_msg(LOG_CRIT, "[http] invalid bind address: %s", cfg->http_server_bind);
        close(fd);
        free(srv);
        return NULL;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg(LOG_CRIT, "[http] bind(%s:%u): %s",
                cfg->http_server_bind, cfg->http_server_port, strerror(errno));
        close(fd);
        free(srv);
        return NULL;
    }

    if (listen(fd, 16) < 0) {
        log_msg(LOG_CRIT, "[http] listen(): %s", strerror(errno));
        close(fd);
        free(srv);
        return NULL;
    }

    srv->listen_fd = fd;

    /* Register with event loop */
    if (event_loop_add_fd(loop, fd, EPOLLIN, http_accept_cb, srv) < 0) {
        log_msg(LOG_CRIT, "[http] event_loop_add_fd(): failed");
        close(fd);
        free(srv);
        return NULL;
    }

    log_msg(LOG_INFO, "[http] listening on %s:%u", cfg->http_server_bind, cfg->http_server_port);
    return srv;
}

void http_server_destroy(http_server_t *srv)
{
    if (!srv)
        return;

    if (srv->listen_fd >= 0) {
        event_loop_del_fd(srv->loop, srv->listen_fd);
        close(srv->listen_fd);
    }

    free(srv);
}

/* ---------- Accept callback ---------- */

static void http_accept_cb(event_loop_t *loop, int fd, uint32_t events, void *data)
{
    (void)events;
    http_server_t *srv = data;

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            log_msg(LOG_WARN, "[http] accept(): %s", strerror(errno));
            break;
        }

        if (set_nonblocking(client_fd) < 0) {
            log_msg(LOG_WARN, "[http] client fcntl(O_NONBLOCK): %s", strerror(errno));
            close(client_fd);
            continue;
        }

        http_client_t *client = calloc(1, sizeof(*client));
        if (!client) {
            close(client_fd);
            continue;
        }

        client->fd = client_fd;
        client->server = srv;
        client->buf_len = 0;
        inet_ntop(AF_INET, &peer.sin_addr, client->peer_addr,
                  sizeof(client->peer_addr));

        if (event_loop_add_fd(loop, client_fd, EPOLLIN, http_client_cb, client) < 0) {
            log_msg(LOG_WARN, "[http] event_loop_add_fd(client): failed");
            close(client_fd);
            free(client);
            continue;
        }
    }
}

/* ---------- Client read callback ---------- */

static void http_client_cb(event_loop_t *loop, int fd, uint32_t events, void *data)
{
    (void)loop;
    (void)events;
    http_client_t *client = data;

    ssize_t n = read(fd, client->buf + client->buf_len,
                     sizeof(client->buf) - client->buf_len - 1);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            http_client_destroy(client);
        }
        return;
    }

    client->buf_len += (size_t)n;
    client->buf[client->buf_len] = '\0';

    /* Check if we have a complete HTTP request (ends with \r\n\r\n) */
    if (strstr(client->buf, "\r\n\r\n") != NULL) {
        http_handle_request(client);
    } else if (client->buf_len >= sizeof(client->buf) - 1) {
        /* Request too large */
        http_send_response(client, 400, "Bad Request",
                           "text/plain", "Request too large\r\n", 19);
    }
}

/* ---------- Request handling ---------- */

static void http_handle_request(http_client_t *client)
{
    /* Parse the request line: "GET /path HTTP/1.x\r\n..." */
    char *method_end = strchr(client->buf, ' ');
    if (!method_end) {
        http_send_response(client, 400, "Bad Request",
                           "text/plain", "Bad request\r\n", 13);
        return;
    }

    /* We only handle GET */
    size_t method_len = (size_t)(method_end - client->buf);
    if (method_len != 3 || memcmp(client->buf, "GET", 3) != 0) {
        http_send_response(client, 405, "Method Not Allowed",
                           "text/plain", "Method not allowed\r\n", 20);
        return;
    }

    /* Extract path */
    char *path_start = method_end + 1;
    char *path_end = strchr(path_start, ' ');
    if (!path_end) {
        http_send_response(client, 400, "Bad Request",
                           "text/plain", "Bad request\r\n", 13);
        return;
    }

    size_t path_len = (size_t)(path_end - path_start);
    char path[512];
    if (path_len >= sizeof(path)) {
        http_send_response(client, 414, "URI Too Long",
                           "text/plain", "URI too long\r\n", 14);
        return;
    }
    memcpy(path, path_start, path_len);
    path[path_len] = '\0';

    /* Route: /NOTIFY/<hex> */
    if (strncmp(path, "/NOTIFY/", 8) == 0) {
        const char *hex_str = path + 8;
        size_t hex_len = strlen(hex_str);

        /* Validate: must be exactly 64 hex characters */
        if (hex_len != 64) {
            log_msg(LOG_WARN, "[http] /NOTIFY invalid hash length: %zu", hex_len);
            http_send_response(client, 400, "Bad Request",
                               "text/plain", "Invalid block hash\r\n", 20);
            return;
        }

        /* Validate all characters are hex */
        for (size_t i = 0; i < 64; i++) {
            if (!is_hex_char(hex_str[i])) {
                log_msg(LOG_WARN, "[http] /NOTIFY invalid hex char at pos %zu", i);
                http_send_response(client, 400, "Bad Request",
                                   "text/plain", "Invalid block hash\r\n", 20);
                return;
            }
        }

        /* Decode hex to 32 bytes */
        uint8_t hash[32];
        if (hex_decode(hex_str, 64, hash) != 0) {
            log_msg(LOG_WARN, "[http] /NOTIFY hex_decode failed");
            http_send_response(client, 400, "Bad Request",
                               "text/plain", "Invalid block hash\r\n", 20);
            return;
        }

        /* Process the hash through the notifier.
         * Try to match peer IP to a configured node label for better logging. */
        const char *node_label = client->peer_addr;
        config_t *cfg = client->server->cfg;
        for (int i = 0; i < cfg->node_count; i++) {
            if (strcmp(cfg->nodes[i].host, client->peer_addr) == 0) {
                node_label = cfg->nodes[i].label;
                break;
            }
        }

        notifier_process_hash(client->server->notifier, hash, node_label, "http");

        log_msg(LOG_INFO, "[notifier] [http] /NOTIFY from %s hash=%s",
                client->peer_addr, hex_str);
        http_send_response(client, 200, "OK",
                           "text/plain", "OK\r\n", 4);
        return;
    }

    /* Route: /stats */
    if (strcmp(path, "/stats") == 0) {
        if (!client->server->stats_cb) {
            http_send_response(client, 503, "Service Unavailable",
                               "application/json",
                               "{\"error\":\"stats not available\"}\r\n", 33);
            return;
        }

        char *stats_buf = malloc(HTTP_MAX_RESPONSE);
        if (!stats_buf) {
            http_send_response(client, 500, "Internal Server Error",
                               "text/plain", "Out of memory\r\n", 15);
            return;
        }

        int stats_len = client->server->stats_cb(stats_buf, HTTP_MAX_RESPONSE,
                                                  client->server->stats_data);
        if (stats_len < 0) {
            free(stats_buf);
            http_send_response(client, 500, "Internal Server Error",
                               "text/plain", "Stats error\r\n", 13);
            return;
        }

        http_send_response(client, 200, "OK",
                           "application/json", stats_buf, (size_t)stats_len);
        free(stats_buf);
        return;
    }

    /* Route: /NOTIFY without hash or with just a slash */
    if (strcmp(path, "/NOTIFY") == 0 || strcmp(path, "/NOTIFY/") == 0) {
        http_send_response(client, 400, "Bad Request",
                           "text/plain", "Missing block hash\r\n", 20);
        return;
    }

    /* Unknown route */
    http_send_response(client, 404, "Not Found",
                       "text/plain", "Not found\r\n", 11);
}

/* ---------- Response sending ---------- */

static void http_send_response(http_client_t *client, int status_code,
                               const char *status_text,
                               const char *content_type,
                               const char *body, size_t body_len)
{
    char header[512];
    int hdr_len = snprintf(header, sizeof(header),
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           status_code, status_text,
                           content_type, body_len);

    if (hdr_len < 0 || (size_t)hdr_len >= sizeof(header)) {
        http_client_destroy(client);
        return;
    }

    /* Best-effort write: send header + body, then close.
     * For this simple server, responses are small and the socket
     * buffer should absorb them. Failures are non-fatal. */
    ssize_t wr = write(client->fd, header, (size_t)hdr_len);
    if (wr > 0 && body && body_len > 0)
        wr = write(client->fd, body, body_len);
    (void)wr;

    http_client_destroy(client);
}

/* ---------- Client cleanup ---------- */

static void http_client_destroy(http_client_t *client)
{
    if (!client)
        return;

    event_loop_del_fd(client->server->loop, client->fd);
    close(client->fd);
    free(client);
}
