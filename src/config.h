/* config.h — JSON configuration parsing via yyjson */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define MAX_NODES       16
#define MAX_PATH_LEN    256
#define SOCKET_BUF_SIZE 4194304  /* 4 MB socket send/recv buffer */

typedef struct {
    char host[256];           /* e.g. "192.168.1.10" */
    uint16_t rpc_port;        /* e.g. 8332 */
    uint16_t zmq_port;        /* ZMQ hashblock port, 0 if not configured */
    char label[64];           /* human-readable label for logging */
} node_config_t;

typedef struct {
    /* Node array */
    node_config_t nodes[MAX_NODES];
    int node_count;

    /* RPC listener */
    char rpc_server_bind[64];     /* e.g. "127.0.0.1" */
    uint16_t rpc_server_port;     /* e.g. 8332 */

    /* HTTP listener (notify + stats) */
    char http_server_bind[64];    /* e.g. "0.0.0.0" */
    uint16_t http_server_port;    /* e.g. 7152 */

    /* Downstream notification relay */
    char zmq_server_bind[64];     /* ZMQ PUB bind address, e.g. "0.0.0.0" */
    uint16_t zmq_server_port;     /* ZMQ PUB port, 0 = disabled */
    char notify_http_url[512];    /* HTTP GET URL template for stratum proxy, %s = hash */

    /* Timeouts and tuning */
    uint32_t rpc_timeout_ms;       /* global RPC timeout in milliseconds */

    /* Logging */
    int log_verbosity;             /* LOG_CRIT..LOG_DEBUG */
} config_t;

/* Load configuration from a JSON file.
 * Returns a heap-allocated config_t on success, or NULL on failure.
 * Logs descriptive errors on parse/validation failure. */
config_t *config_load(const char *path);

/* Free a config_t allocated by config_load(). */
void config_destroy(config_t *cfg);

#endif /* CONFIG_H */
