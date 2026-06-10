/* config.c — JSON configuration parsing via yyjson immutable API */

#include "config.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

/* Helper: safely copy a JSON string value into a fixed-size buffer.
 * Returns 0 on success, -1 if the value is not a string or too long. */
static int
json_str_copy(yyjson_val *val, char *dst, size_t dst_size, const char *field_name)
{
    if (!val || !yyjson_is_str(val)) {
        log_msg(LOG_CRIT, "[config] Missing or invalid string field: %s", field_name);
        return -1;
    }
    const char *s = yyjson_get_str(val);
    size_t len = strlen(s);
    if (len >= dst_size) {
        log_msg(LOG_CRIT, "[config] Field '%s' too long (%zu >= %zu)",
                field_name, len, dst_size);
        return -1;
    }
    memcpy(dst, s, len + 1);
    return 0;
}

/* Helper: read a JSON integer value as uint16_t port.
 * Returns 0 on success, -1 on error. */
static int
json_uint16(yyjson_val *val, uint16_t *dst, const char *field_name)
{
    if (!val || !yyjson_is_int(val)) {
        log_msg(LOG_CRIT, "[config] Missing or invalid integer field: %s", field_name);
        return -1;
    }
    int64_t v = yyjson_get_sint(val);
    if (v <= 0 || v > 65535) {
        log_msg(LOG_CRIT, "[config] Field '%s' out of range: %lld (need 1-65535)",
                field_name, (long long)v);
        return -1;
    }
    *dst = (uint16_t)v;
    return 0;
}

/* Helper: read a JSON integer value as uint32_t.
 * Returns 0 on success, -1 on error. */
static int
json_uint32(yyjson_val *val, uint32_t *dst, const char *field_name)
{
    if (!val || !yyjson_is_int(val)) {
        log_msg(LOG_CRIT, "[config] Missing or invalid integer field: %s", field_name);
        return -1;
    }
    int64_t v = yyjson_get_sint(val);
    if (v <= 0 || v > (int64_t)UINT32_MAX) {
        log_msg(LOG_CRIT, "[config] Field '%s' out of range: %lld",
                field_name, (long long)v);
        return -1;
    }
    *dst = (uint32_t)v;
    return 0;
}

/* Parse the nodes array from the JSON root object.
 * Returns 0 on success, -1 on error. */
static int
parse_nodes(yyjson_val *root, config_t *cfg)
{
    yyjson_val *nodes_val = yyjson_obj_get(root, "nodes");
    if (!nodes_val || !yyjson_is_arr(nodes_val)) {
        log_msg(LOG_CRIT, "[config] Missing or invalid 'nodes' array");
        return -1;
    }

    size_t count = yyjson_arr_size(nodes_val);
    if (count == 0) {
        log_msg(LOG_CRIT, "[config] 'nodes' array is empty (at least 1 node required)");
        return -1;
    }
    if (count > MAX_NODES) {
        log_msg(LOG_CRIT, "[config] Too many nodes: %zu (max %d)", count, MAX_NODES);
        return -1;
    }

    cfg->node_count = (int)count;

    yyjson_val *node_val;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(nodes_val, &iter);
    int idx = 0;

    while ((node_val = yyjson_arr_iter_next(&iter)) != NULL) {
        if (!yyjson_is_obj(node_val)) {
            log_msg(LOG_CRIT, "[config] Node %d is not an object", idx);
            return -1;
        }

        node_config_t *n = &cfg->nodes[idx];

        /* host (required) */
        if (json_str_copy(yyjson_obj_get(node_val, "host"),
                          n->host, sizeof(n->host), "host") != 0)
            return -1;

        /* rpc_port (required) */
        if (json_uint16(yyjson_obj_get(node_val, "rpc_port"),
                        &n->rpc_port, "rpc_port") != 0)
            return -1;

        /* zmq_port (optional, default 0 = not configured) */
        yyjson_val *zmq_val = yyjson_obj_get(node_val, "zmq_port");
        if (zmq_val) {
            if (json_uint16(zmq_val, &n->zmq_port, "zmq_port") != 0)
                return -1;
        } else {
            n->zmq_port = 0;
        }

        /* label (required) */
        if (json_str_copy(yyjson_obj_get(node_val, "label"),
                          n->label, sizeof(n->label), "label") != 0)
            return -1;

        idx++;
    }

    return 0;
}

/* Validate that all node labels are unique.
 * Returns 0 on success, -1 on duplicate. */
static int
validate_unique_labels(const config_t *cfg)
{
    for (int i = 0; i < cfg->node_count; i++) {
        for (int j = i + 1; j < cfg->node_count; j++) {
            if (strcmp(cfg->nodes[i].label, cfg->nodes[j].label) == 0) {
                log_msg(LOG_CRIT, "[config] Duplicate node label: '%s'",
                        cfg->nodes[i].label);
                return -1;
            }
        }
    }
    return 0;
}

config_t *
config_load(const char *path)
{
    if (!path || path[0] == '\0') {
        log_msg(LOG_CRIT, "[config] No configuration file path specified");
        return NULL;
    }

    /* Read the file into memory */
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        log_msg(LOG_CRIT, "[config] Cannot open '%s': %s", path, strerror(errno));
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        log_msg(LOG_CRIT, "[config] Configuration file '%s' is empty", path);
        fclose(fp);
        return NULL;
    }

    char *file_data = malloc((size_t)file_size + 1);
    if (!file_data) {
        log_msg(LOG_CRIT, "[config] Out of memory reading '%s'", path);
        fclose(fp);
        return NULL;
    }

    size_t nread = fread(file_data, 1, (size_t)file_size, fp);
    fclose(fp);

    if ((long)nread != file_size) {
        log_msg(LOG_CRIT, "[config] Short read on '%s'", path);
        free(file_data);
        return NULL;
    }
    file_data[nread] = '\0';

    /* Parse JSON */
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts(file_data, nread, 0, NULL, &err);

    if (!doc) {
        /* Convert byte position to line number */
        size_t line = 1;
        size_t col = 1;
        for (size_t i = 0; i < err.pos && i < nread; i++) {
            if (file_data[i] == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
        }
        log_msg(LOG_CRIT, "[config] JSON parse error in '%s' at line %zu col %zu: %s",
                path, line, col, err.msg);
        free(file_data);
        return NULL;
    }
    free(file_data);

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        log_msg(LOG_CRIT, "[config] Root element in '%s' is not a JSON object", path);
        yyjson_doc_free(doc);
        return NULL;
    }

    /* Allocate config struct */
    config_t *cfg = calloc(1, sizeof(config_t));
    if (!cfg) {
        log_msg(LOG_CRIT, "[config] Out of memory");
        yyjson_doc_free(doc);
        return NULL;
    }

    /* Parse nodes array */
    if (parse_nodes(root, cfg) != 0)
        goto fail;

    /* rpc_server_bind (required) */
    if (json_str_copy(yyjson_obj_get(root, "rpc_server_bind"),
                      cfg->rpc_server_bind, sizeof(cfg->rpc_server_bind), "rpc_server_bind") != 0)
        goto fail;

    /* rpc_server_port (required) */
    if (json_uint16(yyjson_obj_get(root, "rpc_server_port"),
                    &cfg->rpc_server_port, "rpc_server_port") != 0)
        goto fail;

    /* http_server_bind (required) */
    if (json_str_copy(yyjson_obj_get(root, "http_server_bind"),
                      cfg->http_server_bind, sizeof(cfg->http_server_bind), "http_server_bind") != 0)
        goto fail;

    /* http_server_port (required) */
    if (json_uint16(yyjson_obj_get(root, "http_server_port"),
                    &cfg->http_server_port, "http_server_port") != 0)
        goto fail;

    /* zmq_server_bind (optional, default "0.0.0.0") */
    yyjson_val *nzmq_bind = yyjson_obj_get(root, "zmq_server_bind");
    if (nzmq_bind && yyjson_is_str(nzmq_bind)) {
        if (json_str_copy(nzmq_bind, cfg->zmq_server_bind,
                          sizeof(cfg->zmq_server_bind), "zmq_server_bind") != 0)
            goto fail;
    } else {
        snprintf(cfg->zmq_server_bind, sizeof(cfg->zmq_server_bind), "0.0.0.0");
    }

    /* zmq_server_port (optional, 0 = disabled) */
    yyjson_val *nzmq_port = yyjson_obj_get(root, "zmq_server_port");
    if (nzmq_port && yyjson_is_int(nzmq_port)) {
        int64_t v = yyjson_get_sint(nzmq_port);
        if (v < 0 || v > 65535) {
            log_msg(LOG_CRIT, "[config] Field 'zmq_server_port' out of range: %lld (need 0-65535)",
                    (long long)v);
            goto fail;
        }
        cfg->zmq_server_port = (uint16_t)v;
    } else {
        cfg->zmq_server_port = 0;
    }

    /* notify_http_url (optional) */
    yyjson_val *nhttp = yyjson_obj_get(root, "notify_http_url");
    if (nhttp && yyjson_is_str(nhttp)) {
        if (json_str_copy(nhttp, cfg->notify_http_url,
                          sizeof(cfg->notify_http_url), "notify_http_url") != 0)
            goto fail;
    } else {
        cfg->notify_http_url[0] = '\0';
    }

    /* rpc_timeout_ms (required, must be positive) */
    if (json_uint32(yyjson_obj_get(root, "rpc_timeout_ms"),
                    &cfg->rpc_timeout_ms, "rpc_timeout_ms") != 0)
        goto fail;

    /* log_verbosity (required) */
    yyjson_val *verb_val = yyjson_obj_get(root, "log_verbosity");
    if (!verb_val || !yyjson_is_int(verb_val)) {
        log_msg(LOG_CRIT, "[config] Missing or invalid integer field: log_verbosity");
        goto fail;
    }
    int64_t verb = yyjson_get_sint(verb_val);
    if (verb < 0 || verb > 3) {
        log_msg(LOG_CRIT, "[config] log_verbosity out of range: %lld (need 0-3)",
                (long long)verb);
        goto fail;
    }
    cfg->log_verbosity = (int)verb;

    /* ck_notify_socket (optional Unix socket path, empty = disabled) */
    yyjson_val *ck_val = yyjson_obj_get(root, "ck_notify_socket");
    if (ck_val) {
        if (!yyjson_is_str(ck_val)) {
            log_msg(LOG_CRIT, "[config] Field 'ck_notify_socket' has invalid type (expected string)");
            goto fail;
        }
        const char *ck_str = yyjson_get_str(ck_val);
        size_t ck_len = strlen(ck_str);
        if (ck_len == 0) {
            cfg->ck_notify_socket[0] = '\0';
        } else if (ck_len <= 107) {
            memcpy(cfg->ck_notify_socket, ck_str, ck_len + 1);
        } else {
            log_msg(LOG_CRIT, "[config] Field 'ck_notify_socket' too long "
                    "(%zu chars, max 107)", ck_len);
            goto fail;
        }
    } else {
        cfg->ck_notify_socket[0] = '\0';
    }

    /* Validation: unique labels */
    if (validate_unique_labels(cfg) != 0)
        goto fail;

    /* Validation: warn if no downstream notify method configured */
    if (cfg->zmq_server_port == 0 && cfg->notify_http_url[0] == '\0' &&
        cfg->ck_notify_socket[0] == '\0') {
        log_msg(LOG_WARN, "[config] No downstream notification method configured "
                "(neither zmq_server_port, notify_http_url, nor ck_notify_socket)");
    }

    yyjson_doc_free(doc);
    return cfg;

fail:
    free(cfg);
    yyjson_doc_free(doc);
    return NULL;
}

void
config_destroy(config_t *cfg)
{
    free(cfg);
}
