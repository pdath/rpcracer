/* test_config_ck_socket.c — Unit tests for ck_notify_socket config parsing */

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

static const char *TMP_PATH = "/tmp/test_rpcrace_ck_socket.json";

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
test_valid_path(void)
{
    printf("  test_valid_path\n");

    const char *json =
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 2,\n"
        "  \"ck_notify_socket\": \"/tmp/ckpool/stratifier\"\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg != NULL, "config_load returns non-NULL for valid ck_notify_socket");
    if (!cfg) return;

    ASSERT(strcmp(cfg->ck_notify_socket, "/tmp/ckpool/stratifier") == 0,
           "ck_notify_socket stores path correctly");

    config_destroy(cfg);
    unlink(TMP_PATH);
}

static void
test_absent_field(void)
{
    printf("  test_absent_field\n");

    const char *json =
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 2\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg != NULL, "config_load returns non-NULL when ck_notify_socket absent");
    if (!cfg) return;

    ASSERT(cfg->ck_notify_socket[0] == '\0',
           "ck_notify_socket is empty string when field absent");

    config_destroy(cfg);
    unlink(TMP_PATH);
}

static void
test_empty_string(void)
{
    printf("  test_empty_string\n");

    const char *json =
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 2,\n"
        "  \"ck_notify_socket\": \"\"\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg != NULL, "config_load returns non-NULL for empty ck_notify_socket");
    if (!cfg) return;

    ASSERT(cfg->ck_notify_socket[0] == '\0',
           "ck_notify_socket is empty string for empty value");

    config_destroy(cfg);
    unlink(TMP_PATH);
}

static void
test_path_exactly_107_chars(void)
{
    printf("  test_path_exactly_107_chars\n");

    /* Build a path of exactly 107 chars: "/tmp/" (5) + 102 'x' chars = 107 */
    char path107[108];
    memcpy(path107, "/tmp/", 5);
    memset(path107 + 5, 'x', 102);
    path107[107] = '\0';

    char json[1024];
    snprintf(json, sizeof(json),
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 2,\n"
        "  \"ck_notify_socket\": \"%s\"\n"
        "}\n", path107);

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg != NULL, "config_load accepts path of exactly 107 chars");
    if (!cfg) return;

    ASSERT(strcmp(cfg->ck_notify_socket, path107) == 0,
           "ck_notify_socket stores 107-char path correctly");
    ASSERT(strlen(cfg->ck_notify_socket) == 107,
           "stored path length is exactly 107");

    config_destroy(cfg);
    unlink(TMP_PATH);
}

static void
test_path_108_chars_rejected(void)
{
    printf("  test_path_108_chars_rejected\n");

    /* Build a path of exactly 108 chars: "/tmp/" (5) + 103 'x' chars = 108 */
    char path108[109];
    memcpy(path108, "/tmp/", 5);
    memset(path108 + 5, 'x', 103);
    path108[108] = '\0';

    char json[1024];
    snprintf(json, sizeof(json),
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 2,\n"
        "  \"ck_notify_socket\": \"%s\"\n"
        "}\n", path108);

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "config_load returns NULL for path of 108 chars");
    unlink(TMP_PATH);
}

static void
test_non_string_integer(void)
{
    printf("  test_non_string_integer\n");

    const char *json =
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 2,\n"
        "  \"ck_notify_socket\": 42\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "config_load returns NULL for integer ck_notify_socket");
    unlink(TMP_PATH);
}

static void
test_non_string_boolean(void)
{
    printf("  test_non_string_boolean\n");

    const char *json =
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 2,\n"
        "  \"ck_notify_socket\": true\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "config_load returns NULL for boolean ck_notify_socket");
    unlink(TMP_PATH);
}

static void
test_non_string_null(void)
{
    printf("  test_non_string_null\n");

    const char *json =
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 2,\n"
        "  \"ck_notify_socket\": null\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "config_load returns NULL for null ck_notify_socket");
    unlink(TMP_PATH);
}

static void
test_non_string_array(void)
{
    printf("  test_non_string_array\n");

    const char *json =
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 2,\n"
        "  \"ck_notify_socket\": [1, 2]\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "config_load returns NULL for array ck_notify_socket");
    unlink(TMP_PATH);
}

static void
test_non_string_object(void)
{
    printf("  test_non_string_object\n");

    const char *json =
        "{\n"
        "  \"nodes\": [{\"label\": \"n1\", \"host\": \"10.0.0.1\", \"rpc_port\": 8332}],\n"
        "  \"rpc_server_bind\": \"127.0.0.1\",\n"
        "  \"rpc_server_port\": 8332,\n"
        "  \"http_server_bind\": \"0.0.0.0\",\n"
        "  \"http_server_port\": 7152,\n"
        "  \"rpc_timeout_ms\": 5000,\n"
        "  \"log_verbosity\": 2,\n"
        "  \"ck_notify_socket\": {\"a\": 1}\n"
        "}\n";

    write_tmp_config(json);
    config_t *cfg = config_load(TMP_PATH);

    ASSERT(cfg == NULL, "config_load returns NULL for object ck_notify_socket");
    unlink(TMP_PATH);
}

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Initialize logging (suppress most output during tests) */
    log_init(LOG_CRIT);

    printf("test_config_ck_socket:\n");

    test_valid_path();
    test_absent_field();
    test_empty_string();
    test_path_exactly_107_chars();
    test_path_108_chars_rejected();
    test_non_string_integer();
    test_non_string_boolean();
    test_non_string_null();
    test_non_string_array();
    test_non_string_object();

    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
