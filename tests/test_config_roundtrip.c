/* test_config_roundtrip.c — Property test for configuration round-trip (Property 13)
 *
 * Property 13: Configuration Round-Trip
 * For any valid configuration (node array with 1–8 nodes each having host,
 * port, and optional ZMQ endpoint; valid bind addresses; positive timeout;
 * valid verbosity level), serializing to JSON and parsing with config_load()
 * shall produce an equivalent config_t structure.
 *
 * Validates: Requirements 11.4, 11.5, 11.6, 11.7, 11.8, 11.9
 *
 * Uses hand-rolled randomized testing: seeded PRNG (srand48/lrand48),
 * 1000 trials, seed printed for reproducibility, seed accepted via argv[1].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "../src/config.h"
#include "../src/log.h"
#include "yyjson.h"

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

static const char *TMP_PATH = "/tmp/test_rpcrace_config_roundtrip.json";

/* Generate a random IP-like host string: "10.x.y.z" */
static void
gen_random_host(char *buf, size_t buf_size)
{
    int a = 1 + (int)(lrand48() % 254);
    int b = (int)(lrand48() % 256);
    int c = (int)(lrand48() % 256);
    int d = 1 + (int)(lrand48() % 254);
    snprintf(buf, buf_size, "%d.%d.%d.%d", a, b, c, d);
}

/* Generate a random port in range [1, 65535] */
static uint16_t
gen_random_port(void)
{
    return (uint16_t)(1 + (lrand48() % 65535));
}

/* Generate a unique label for a node. Uses index to guarantee uniqueness. */
static void
gen_unique_label(char *buf, size_t buf_size, int index)
{
    /* Base alphanumeric part + index suffix to guarantee uniqueness */
    char base[32];
    int base_len = 1 + (int)(lrand48() % 10);
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz";
    for (int i = 0; i < base_len; i++) {
        base[i] = charset[lrand48() % (sizeof(charset) - 1)];
    }
    base[base_len] = '\0';
    snprintf(buf, buf_size, "%s%d", base, index);
}

/* Generate a random ZMQ port: [1, 65535] or 0 (not configured) */
static uint16_t
gen_random_zmq_port(void)
{
    return (uint16_t)(1 + (lrand48() % 65535));
}

/* Generate a random bind address: one of "127.0.0.1", "0.0.0.0", or a random IP */
static void
gen_random_bind(char *buf, size_t buf_size)
{
    int choice = (int)(lrand48() % 3);
    switch (choice) {
    case 0:
        snprintf(buf, buf_size, "127.0.0.1");
        break;
    case 1:
        snprintf(buf, buf_size, "0.0.0.0");
        break;
    default:
        gen_random_host(buf, buf_size);
        break;
    }
}

/* Generate a random valid config_t and populate all fields */
static void
gen_random_config(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* Node count: 1–8 */
    cfg->node_count = 1 + (int)(lrand48() % MAX_NODES);

    for (int i = 0; i < cfg->node_count; i++) {
        node_config_t *n = &cfg->nodes[i];
        gen_random_host(n->host, sizeof(n->host));
        n->rpc_port = gen_random_port();
        gen_unique_label(n->label, sizeof(n->label), i);

        /* Optional ZMQ port (50% chance) */
        if (lrand48() % 2 == 0) {
            n->zmq_port = gen_random_zmq_port();
        } else {
            n->zmq_port = 0;
        }
    }

    /* RPC listener */
    gen_random_bind(cfg->rpc_server_bind, sizeof(cfg->rpc_server_bind));
    cfg->rpc_server_port = gen_random_port();

    /* HTTP listener */
    gen_random_bind(cfg->http_server_bind, sizeof(cfg->http_server_bind));
    cfg->http_server_port = gen_random_port();

    /* Optional zmq_server_bind + zmq_server_port (50% chance enabled) */
    if (lrand48() % 2 == 0) {
        gen_random_bind(cfg->zmq_server_bind, sizeof(cfg->zmq_server_bind));
        cfg->zmq_server_port = gen_random_port();
    } else {
        snprintf(cfg->zmq_server_bind, sizeof(cfg->zmq_server_bind), "0.0.0.0");
        cfg->zmq_server_port = 0;
    }

    /* Optional notify_http_url (50% chance) */
    if (lrand48() % 2 == 0) {
        char host[64];
        gen_random_host(host, sizeof(host));
        int port = 1 + (int)(lrand48() % 65535);
        /* Sometimes include %s placeholder, sometimes not */
        if (lrand48() % 2 == 0) {
            snprintf(cfg->notify_http_url, sizeof(cfg->notify_http_url),
                     "http://%s:%d/NOTIFY/%%s", host, port);
        } else {
            snprintf(cfg->notify_http_url, sizeof(cfg->notify_http_url),
                     "http://%s:%d/NOTIFY", host, port);
        }
    } else {
        cfg->notify_http_url[0] = '\0';
    }

    /* Timeouts: 1–100000 */
    cfg->rpc_timeout_ms = (uint32_t)(1 + (lrand48() % 100000));

    /* Verbosity: 0–3 */
    cfg->log_verbosity = (int)(lrand48() % 4);
}

/* Serialize a config_t to JSON using yyjson mutable API and write to file.
 * Returns 0 on success, -1 on failure. */
static int
serialize_config_to_file(const config_t *cfg, const char *path)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return -1;

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    /* Nodes array */
    yyjson_mut_val *nodes_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < cfg->node_count; i++) {
        const node_config_t *n = &cfg->nodes[i];
        yyjson_mut_val *node_obj = yyjson_mut_obj(doc);

        yyjson_mut_obj_add_str(doc, node_obj, "label", n->label);
        yyjson_mut_obj_add_str(doc, node_obj, "host", n->host);
        yyjson_mut_obj_add_int(doc, node_obj, "rpc_port", (int64_t)n->rpc_port);

        if (n->zmq_port != 0) {
            yyjson_mut_obj_add_int(doc, node_obj, "zmq_port", (int64_t)n->zmq_port);
        }

        yyjson_mut_arr_append(nodes_arr, node_obj);
    }
    yyjson_mut_obj_add_val(doc, root, "nodes", nodes_arr);

    /* Top-level fields */
    yyjson_mut_obj_add_str(doc, root, "rpc_server_bind", cfg->rpc_server_bind);
    yyjson_mut_obj_add_int(doc, root, "rpc_server_port", (int64_t)cfg->rpc_server_port);
    yyjson_mut_obj_add_str(doc, root, "http_server_bind", cfg->http_server_bind);
    yyjson_mut_obj_add_int(doc, root, "http_server_port", (int64_t)cfg->http_server_port);

    if (cfg->zmq_server_port != 0) {
        yyjson_mut_obj_add_str(doc, root, "zmq_server_bind", cfg->zmq_server_bind);
        yyjson_mut_obj_add_int(doc, root, "zmq_server_port", (int64_t)cfg->zmq_server_port);
    }
    if (cfg->notify_http_url[0] != '\0') {
        yyjson_mut_obj_add_str(doc, root, "notify_http_url", cfg->notify_http_url);
    }

    yyjson_mut_obj_add_int(doc, root, "rpc_timeout_ms", (int64_t)cfg->rpc_timeout_ms);
    yyjson_mut_obj_add_int(doc, root, "log_verbosity", (int64_t)cfg->log_verbosity);

    /* Write to file */
    yyjson_write_err err;
    bool ok = yyjson_mut_write_file(path, doc, YYJSON_WRITE_PRETTY, NULL, &err);
    yyjson_mut_doc_free(doc);

    if (!ok) {
        fprintf(stderr, "  yyjson_mut_write_file failed: %s\n", err.msg);
        return -1;
    }
    return 0;
}

/* Compare two config_t structures for equivalence.
 * Returns 0 if equivalent, prints first mismatch and returns -1. */
static int
compare_configs(const config_t *expected, const config_t *actual, int trial)
{
    if (expected->node_count != actual->node_count) {
        fprintf(stderr, "  FAIL trial %d: node_count %d != %d\n",
                trial, expected->node_count, actual->node_count);
        return -1;
    }

    for (int i = 0; i < expected->node_count; i++) {
        const node_config_t *e = &expected->nodes[i];
        const node_config_t *a = &actual->nodes[i];

        if (strcmp(e->host, a->host) != 0) {
            fprintf(stderr, "  FAIL trial %d: node[%d].host '%s' != '%s'\n",
                    trial, i, e->host, a->host);
            return -1;
        }
        if (e->rpc_port != a->rpc_port) {
            fprintf(stderr, "  FAIL trial %d: node[%d].rpc_port %d != %d\n",
                    trial, i, e->rpc_port, a->rpc_port);
            return -1;
        }
        if (e->zmq_port != a->zmq_port) {
            fprintf(stderr, "  FAIL trial %d: node[%d].zmq_port %u != %u\n",
                    trial, i, e->zmq_port, a->zmq_port);
            return -1;
        }
        if (strcmp(e->label, a->label) != 0) {
            fprintf(stderr, "  FAIL trial %d: node[%d].label '%s' != '%s'\n",
                    trial, i, e->label, a->label);
            return -1;
        }
    }

    if (strcmp(expected->rpc_server_bind, actual->rpc_server_bind) != 0) {
        fprintf(stderr, "  FAIL trial %d: rpc_server_bind '%s' != '%s'\n",
                trial, expected->rpc_server_bind, actual->rpc_server_bind);
        return -1;
    }
    if (expected->rpc_server_port != actual->rpc_server_port) {
        fprintf(stderr, "  FAIL trial %d: rpc_server_port %d != %d\n",
                trial, expected->rpc_server_port, actual->rpc_server_port);
        return -1;
    }
    if (strcmp(expected->http_server_bind, actual->http_server_bind) != 0) {
        fprintf(stderr, "  FAIL trial %d: http_server_bind '%s' != '%s'\n",
                trial, expected->http_server_bind, actual->http_server_bind);
        return -1;
    }
    if (expected->http_server_port != actual->http_server_port) {
        fprintf(stderr, "  FAIL trial %d: http_server_port %d != %d\n",
                trial, expected->http_server_port, actual->http_server_port);
        return -1;
    }
    if (strcmp(expected->zmq_server_bind, actual->zmq_server_bind) != 0) {
        fprintf(stderr, "  FAIL trial %d: zmq_server_bind '%s' != '%s'\n",
                trial, expected->zmq_server_bind, actual->zmq_server_bind);
        return -1;
    }
    if (expected->zmq_server_port != actual->zmq_server_port) {
        fprintf(stderr, "  FAIL trial %d: zmq_server_port %u != %u\n",
                trial, expected->zmq_server_port, actual->zmq_server_port);
        return -1;
    }
    if (strcmp(expected->notify_http_url, actual->notify_http_url) != 0) {
        fprintf(stderr, "  FAIL trial %d: notify_http_url '%s' != '%s'\n",
                trial, expected->notify_http_url, actual->notify_http_url);
        return -1;
    }
    if (expected->rpc_timeout_ms != actual->rpc_timeout_ms) {
        fprintf(stderr, "  FAIL trial %d: rpc_timeout_ms %u != %u\n",
                trial, expected->rpc_timeout_ms, actual->rpc_timeout_ms);
        return -1;
    }
    if (expected->log_verbosity != actual->log_verbosity) {
        fprintf(stderr, "  FAIL trial %d: log_verbosity %d != %d\n",
                trial, expected->log_verbosity, actual->log_verbosity);
        return -1;
    }

    return 0;
}

/*
 * Property 13: Configuration Round-Trip
 *
 * For any valid configuration (node array with 1–8 nodes each having host,
 * port, and optional ZMQ endpoint; valid bind addresses; positive timeout;
 * valid verbosity level), serializing to JSON and parsing with config_load()
 * shall produce an equivalent config_t structure.
 *
 * Validates: Requirements 11.4, 11.5, 11.6, 11.7, 11.8, 11.9
 */
static void
test_property_config_roundtrip(long seed)
{
    printf("  property: configuration round-trip (seed=%ld, 1000 trials)\n", seed);
    srand48(seed);

    int trials = 1000;
    int passed = 0;

    for (int i = 0; i < trials; i++) {
        config_t original;
        gen_random_config(&original);

        /* Serialize to JSON file */
        if (serialize_config_to_file(&original, TMP_PATH) != 0) {
            fprintf(stderr, "  FAIL: serialize failed at trial %d\n", i);
            tests_run++;
            return;
        }

        /* Parse back with config_load() */
        config_t *loaded = config_load(TMP_PATH);
        if (!loaded) {
            fprintf(stderr, "  FAIL: config_load returned NULL at trial %d\n", i);
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, i);
            tests_run++;
            unlink(TMP_PATH);
            return;
        }

        /* Compare all fields */
        if (compare_configs(&original, loaded, i) != 0) {
            fprintf(stderr, "  (seed=%ld, trial=%d)\n", seed, i);
            config_destroy(loaded);
            tests_run++;
            unlink(TMP_PATH);
            return;
        }

        config_destroy(loaded);
        passed++;
    }

    unlink(TMP_PATH);

    tests_run++;
    if (passed == trials) {
        tests_passed++;
        printf("    %d/%d trials passed\n", passed, trials);
    } else {
        printf("    %d/%d trials passed\n", passed, trials);
    }
}

int
main(int argc, char *argv[])
{
    long seed;
    if (argc > 1) {
        seed = atol(argv[1]);
    } else {
        seed = (long)time(NULL);
    }

    printf("test_config_roundtrip (seed=%ld):\n", seed);

    /* Initialize logging at CRIT level to suppress noise during tests */
    log_init(LOG_CRIT);

    /* Run property test */
    test_property_config_roundtrip(seed);

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
