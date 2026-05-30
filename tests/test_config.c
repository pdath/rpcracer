/* test_config.c — Unit tests for config.c (JSON configuration parsing) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "../src/config.h"
#include "../src/log.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while (0)

/* Write a string to a temporary file and return the path.
 * Caller must unlink the file when done. */
static const char *TMP_PATH = "/tmp/test_rpcrace_config.json";

static void
write_tmp_config(const char *json)
{
    FILE *fp = fopen(TMP_PATH, "w");
    if (!fp) {
        fprintf(stderr, "Cannot create temp file\n");
        exit(1);
    }
    fputs(json, fp);
    fclose(fp);
}

static void
test_valid_full_config(void)
{
    printf("  test_valid_full_config\n");

    const char *json =
        "{\n"
        "  \"nodes\": [\n"
        "    {\n"
        "      \"label\": \"local-knots\",\n"
        "      \"host\": \"127.0.0.1\",\n"
        "      \"rpc_port\": 8332,\n"
        "      \"zmq_port\": 38332\n"
        "    },\n"
        "    {\n"
        "      \"label\": \"remote-core\",\n"
        "      \"host\": \"10.0.1.50\",\n"
        "      \"rpc_port\": 8333\n"
        "    }\n"
        "  ],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 37152,\n"
        "  \"zmq_server_bind\": \"0.0.0.0\",\n"
        "  \"zmq_server_port\": 28332,\n"
        "  \"notify_http_url\": \"http://127.0.0.1:7152/NOTIFY/%s\",\n"
        "  \"rpc_timeout_ms\": 30000,\n"
        "  \"reconnect_delay_ms\": 1000,\n"
        "  \"stall_threshold_ms\": 60000,\n"
        "  \"log_verbosity\": 2\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg != NULL, "config_load returns non-NULL for valid config");
    if (!cfg) return;

    ASSERT(cfg->node_count == 2, "node_count == 2");
    ASSERT(strcmp(cfg->nodes[0].label, "local-knots") == 0, "node 0 label");
    ASSERT(strcmp(cfg->nodes[0].host, "127.0.0.1") == 0, "node 0 host");
    ASSERT(cfg->nodes[0].rpc_port == 8332, "node 0 rpc_port");
    ASSERT(cfg->nodes[0].zmq_port == 38332, "node 0 zmq_port");

    ASSERT(strcmp(cfg->nodes[1].label, "remote-core") == 0, "node 1 label");
    ASSERT(strcmp(cfg->nodes[1].host, "10.0.1.50") == 0, "node 1 host");
    ASSERT(cfg->nodes[1].rpc_port == 8333, "node 1 rpc_port");
    ASSERT(cfg->nodes[1].zmq_port == 0, "node 1 zmq_port zero");

    ASSERT(strcmp(cfg->rpc_server_bind, "127.0.0.1") == 0, "rpc_server_bind");
    ASSERT(cfg->rpc_server_port == 8332, "rpc_server_port");
    ASSERT(strcmp(cfg->http_server_bind, "0.0.0.0") == 0, "http_server_bind");
    ASSERT(cfg->http_server_port == 37152, "http_server_port");
    ASSERT(strcmp(cfg->zmq_server_bind, "0.0.0.0") == 0, "zmq_server_bind");
    ASSERT(cfg->zmq_server_port == 28332, "zmq_server_port");
    ASSERT(strcmp(cfg->notify_http_url, "http://127.0.0.1:7152/NOTIFY/%s") == 0, "notify_http_url");
    ASSERT(cfg->rpc_timeout_ms == 30000, "rpc_timeout_ms");
    ASSERT(cfg->reconnect_delay_ms == 1000, "reconnect_delay_ms");
    ASSERT(cfg->stall_threshold_ms == 60000, "stall_threshold_ms");
    ASSERT(cfg->log_verbosity == 2, "log_verbosity");

    config_destroy(cfg);
    unlink(TMP_PATH);
}

static void
test_minimal_config(void)
{
    printf("  test_minimal_config\n");

    /* Minimal: 1 node, no optional notify endpoints */
    const char *json =
        "{\n"
        "  \"nodes\": [\n"
        "    { \"label\": \"node1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332 }\n"
        "  ],\n"
        "  \"rpc_server_bind\": \"0.0.0.0\",\n"
        "  \"rpc_server_port\": 9332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"reconnect_delay_ms\": 500,\n"
        "  \"stall_threshold_ms\": 30000,\n"
        "  \"log_verbosity\": 0\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg != NULL, "minimal config loads successfully");
    if (!cfg) return;

    ASSERT(cfg->node_count == 1, "node_count == 1");
    ASSERT(cfg->zmq_server_port == 0, "no zmq_server_port");
    ASSERT(cfg->notify_http_url[0] == '\0', "no notify_http_url");
    ASSERT(cfg->log_verbosity == 0, "log_verbosity == 0");

    config_destroy(cfg);
    unlink(TMP_PATH);
}

static void
test_empty_nodes_array(void)
{
    printf("  test_empty_nodes_array\n");

    const char *json =
        "{\n"
        "  \"nodes\": [],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"reconnect_delay_ms\": 500,\n"
        "  \"stall_threshold_ms\": 30000,\n"
        "  \"log_verbosity\": 2\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "empty nodes array rejected");
    unlink(TMP_PATH);
}

static void
test_duplicate_labels(void)
{
    printf("  test_duplicate_labels\n");

    const char *json =
        "{\n"
        "  \"nodes\": [\n"
        "    { \"label\": \"same\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332 },\n"
        "    { \"label\": \"same\", \"host\": \"10.0.0.2\", \"rpc_port\": 8332 }\n"
        "  ],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"reconnect_delay_ms\": 500,\n"
        "  \"stall_threshold_ms\": 30000,\n"
        "  \"log_verbosity\": 2\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "duplicate labels rejected");
    unlink(TMP_PATH);
}

static void
test_zero_port(void)
{
    printf("  test_zero_port\n");

    const char *json =
        "{\n"
        "  \"nodes\": [\n"
        "    { \"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332 }\n"
        "  ],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 0,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"reconnect_delay_ms\": 500,\n"
        "  \"stall_threshold_ms\": 30000,\n"
        "  \"log_verbosity\": 2\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "zero rpc_server_port rejected");
    unlink(TMP_PATH);
}

static void
test_zero_timeout(void)
{
    printf("  test_zero_timeout\n");

    const char *json =
        "{\n"
        "  \"nodes\": [\n"
        "    { \"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332 }\n"
        "  ],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 0,\n"
        "  \"reconnect_delay_ms\": 500,\n"
        "  \"stall_threshold_ms\": 30000,\n"
        "  \"log_verbosity\": 2\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "zero rpc_timeout_ms rejected");
    unlink(TMP_PATH);
}

static void
test_invalid_json(void)
{
    printf("  test_invalid_json\n");

    write_tmp_config("{ this is not valid json }}}");
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "invalid JSON rejected");
    unlink(TMP_PATH);
}

static void
test_missing_file(void)
{
    printf("  test_missing_file\n");

    config_t *cfg = config_load("/tmp/nonexistent_rpcrace_config_xyz.json");
    ASSERT(cfg == NULL, "missing file returns NULL");
}

static void
test_null_path(void)
{
    printf("  test_null_path\n");

    config_t *cfg = config_load(NULL);
    ASSERT(cfg == NULL, "NULL path returns NULL");
}

static void
test_empty_path(void)
{
    printf("  test_empty_path\n");

    config_t *cfg = config_load("");
    ASSERT(cfg == NULL, "empty path returns NULL");
}

static void
test_invalid_verbosity(void)
{
    printf("  test_invalid_verbosity\n");

    const char *json =
        "{\n"
        "  \"nodes\": [\n"
        "    { \"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332 }\n"
        "  ],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"reconnect_delay_ms\": 500,\n"
        "  \"stall_threshold_ms\": 30000,\n"
        "  \"log_verbosity\": 5\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "verbosity > 3 rejected");
    unlink(TMP_PATH);
}

static void
test_too_many_nodes(void)
{
    printf("  test_too_many_nodes\n");

    /* Build JSON with 17 nodes (exceeds MAX_NODES=16) */
    char json[8192];
    int off = 0;
    off += snprintf(json + off, sizeof(json) - (size_t)off, "{ \"nodes\": [\n");
    for (int i = 0; i < 17; i++) {
        off += snprintf(json + off, sizeof(json) - (size_t)off,
                        "  { \"label\": \"n%d\", \"host\": \"10.0.0.%d\", \"rpc_port\": 8332 }%s\n",
                        i, i + 1, (i < 16) ? "," : "");
    }
    off += snprintf(json + off, sizeof(json) - (size_t)off,
                    "],\n"
                    "\"rpc_server_bind\": \"127.0.0.1\", \"rpc_server_port\": 8332,\n"
                    "\"http_server_bind\": \"0.0.0.0\", \"http_server_port\": 7152,\n"
                    "\"rpc_timeout_ms\": 5000, \"reconnect_delay_ms\": 500,\n"
                    "\"stall_threshold_ms\": 30000, \"log_verbosity\": 2 }\n");
    (void)off;

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "too many nodes rejected");
    unlink(TMP_PATH);
}

static void
test_config_destroy_null(void)
{
    printf("  test_config_destroy_null\n");

    /* Should not crash */
    config_destroy(NULL);
    ASSERT(1, "config_destroy(NULL) does not crash");
}

static void
test_max_nodes(void)
{
    printf("  test_max_nodes\n");

    /* Build JSON with exactly 16 nodes (MAX_NODES) */
    char json[8192];
    int off = 0;
    off += snprintf(json + off, sizeof(json) - (size_t)off, "{ \"nodes\": [\n");
    for (int i = 0; i < 16; i++) {
        off += snprintf(json + off, sizeof(json) - (size_t)off,
                        "  { \"label\": \"node%d\", \"host\": \"10.0.0.%d\", \"rpc_port\": %d }%s\n",
                        i, i + 1, 8332 + i, (i < 15) ? "," : "");
    }
    off += snprintf(json + off, sizeof(json) - (size_t)off,
                    "],\n"
                    "\"rpc_server_bind\": \"127.0.0.1\", \"rpc_server_port\": 8332,\n"
                    "\"http_server_bind\": \"0.0.0.0\", \"http_server_port\": 7152,\n"
                    "\"rpc_timeout_ms\": 5000, \"reconnect_delay_ms\": 500,\n"
                    "\"stall_threshold_ms\": 30000, \"log_verbosity\": 2 }\n");
    (void)off;

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg != NULL, "16 nodes (MAX_NODES) accepted");
    if (cfg) {
        ASSERT(cfg->node_count == 16, "node_count == 16");
        config_destroy(cfg);
    }
    unlink(TMP_PATH);
}

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Initialize logging (suppress most output during tests) */
    log_init(LOG_CRIT);

    printf("test_config:\n");

    test_valid_full_config();
    test_minimal_config();
    test_empty_nodes_array();
    test_duplicate_labels();
    test_zero_port();
    test_zero_timeout();
    test_invalid_json();
    test_missing_file();
    test_null_path();
    test_empty_path();
    test_invalid_verbosity();
    test_too_many_nodes();
    test_config_destroy_null();
    test_max_nodes();

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
