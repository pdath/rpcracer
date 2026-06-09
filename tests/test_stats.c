/* test_stats.c — Unit tests for stats module (opaque pointer API) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "../src/stats.h"
#include "yyjson.h"

#define BUF_SIZE 4096

/* Helper: set up a minimal config with N labels */
static void setup_config(config_t *cfg, int n)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->node_count = n;
    for (int i = 0; i < n; i++)
        snprintf(cfg->nodes[i].label, sizeof(cfg->nodes[i].label), "node%d", i);
}

static void test_stats_create_destroy(void)
{
    stats_t *s = stats_create(3);
    assert(s != NULL);
    stats_destroy(s);

    /* Zero nodes */
    s = stats_create(0);
    assert(s != NULL);
    stats_destroy(s);

    printf("  test_stats_create_destroy: OK\n");
}

static void test_stats_record_gbt(void)
{
    stats_t *s = stats_create(2);
    assert(s != NULL);

    /* Record 2 GBT responses for node 0 (each is a race win) */
    stats_record_gbt(s, 0, 50000, 1000, 75000);
    stats_record_race_win(s, 0);
    stats_record_gbt(s, 0, 60000, 1200, 85000);
    stats_record_race_win(s, 0);

    /* Serialize and verify */
    config_t cfg;
    setup_config(&cfg, 2);

    stats_serialize_ctx_t ctx = { .stats = s, .cfg = &cfg, .proxy = NULL };
    char buf[BUF_SIZE];
    int len = stats_serialize_json(buf, BUF_SIZE, &ctx);
    assert(len > 0);

    yyjson_doc *doc = yyjson_read(buf, (size_t)len, 0);
    assert(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *nodes = yyjson_obj_get(root, "nodes");
    yyjson_val *n0 = yyjson_arr_get(nodes, 0);
    yyjson_val *gbt = yyjson_obj_get(n0, "race_gbt");

    /* avg_us = (50000 + 60000) / 2 = 55000 */
    assert(yyjson_get_uint(yyjson_obj_get(gbt, "avg_us")) == 55000);
    /* last_us = 60000 */
    assert(yyjson_get_uint(yyjson_obj_get(gbt, "last_us")) == 60000);
    /* wins = 2 */
    assert(yyjson_get_uint(yyjson_obj_get(gbt, "wins")) == 2);
    /* last_tx_count = 1200 */
    assert(yyjson_get_uint(yyjson_obj_get(gbt, "last_tx_count")) == 1200);

    /* Node 1 should be untouched */
    yyjson_val *n1 = yyjson_arr_get(nodes, 1);
    yyjson_val *gbt1 = yyjson_obj_get(n1, "race_gbt");
    assert(yyjson_get_uint(yyjson_obj_get(gbt1, "wins")) == 0);

    yyjson_doc_free(doc);
    stats_destroy(s);
    printf("  test_stats_record_gbt: OK\n");
}

static void test_stats_record_race_win(void)
{
    stats_t *s = stats_create(3);
    assert(s != NULL);

    stats_record_race_win(s, 1);
    stats_record_race_win(s, 1);
    stats_record_race_win(s, 0);

    config_t cfg;
    setup_config(&cfg, 3);

    stats_serialize_ctx_t ctx = { .stats = s, .cfg = &cfg, .proxy = NULL };
    char buf[BUF_SIZE];
    int len = stats_serialize_json(buf, BUF_SIZE, &ctx);
    assert(len > 0);

    yyjson_doc *doc = yyjson_read(buf, (size_t)len, 0);
    assert(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);

    /* total_gbt_races = 3 */
    assert(yyjson_get_uint(yyjson_obj_get(root, "total_gbt_races")) == 3);

    /* Node 1 wins = 2, node 0 wins = 1 */
    yyjson_val *nodes = yyjson_obj_get(root, "nodes");
    yyjson_val *n0 = yyjson_arr_get(nodes, 0);
    yyjson_val *n1 = yyjson_arr_get(nodes, 1);
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(n0, "race_gbt"), "wins")) == 1);
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(n1, "race_gbt"), "wins")) == 2);

    yyjson_doc_free(doc);
    stats_destroy(s);
    printf("  test_stats_record_race_win: OK\n");
}

static void test_stats_record_notify_win(void)
{
    stats_t *s = stats_create(2);
    assert(s != NULL);

    stats_record_notify_win(s, 0);
    stats_record_notify_win(s, 0);
    stats_record_notify_win(s, 1);

    config_t cfg;
    setup_config(&cfg, 2);

    stats_serialize_ctx_t ctx = { .stats = s, .cfg = &cfg, .proxy = NULL };
    char buf[BUF_SIZE];
    int len = stats_serialize_json(buf, BUF_SIZE, &ctx);
    assert(len > 0);

    yyjson_doc *doc = yyjson_read(buf, (size_t)len, 0);
    assert(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *nodes = yyjson_obj_get(root, "nodes");

    assert(yyjson_get_uint(yyjson_obj_get(yyjson_arr_get(nodes, 0), "notify_wins")) == 2);
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_arr_get(nodes, 1), "notify_wins")) == 1);

    yyjson_doc_free(doc);
    stats_destroy(s);
    printf("  test_stats_record_notify_win: OK\n");
}

static void test_stats_record_notify_to_gbt(void)
{
    stats_t *s = stats_create(1);
    assert(s != NULL);

    stats_record_notify_to_gbt(s, 45000);

    config_t cfg;
    setup_config(&cfg, 1);

    stats_serialize_ctx_t ctx = { .stats = s, .cfg = &cfg, .proxy = NULL };
    char buf[BUF_SIZE];
    int len = stats_serialize_json(buf, BUF_SIZE, &ctx);
    assert(len > 0);

    yyjson_doc *doc = yyjson_read(buf, (size_t)len, 0);
    assert(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_get_uint(yyjson_obj_get(root, "last_notify_to_gbt_us")) == 45000);

    yyjson_doc_free(doc);
    stats_destroy(s);
    printf("  test_stats_record_notify_to_gbt: OK\n");
}

static void test_stats_record_request(void)
{
    stats_t *s = stats_create(1);
    assert(s != NULL);

    for (int i = 0; i < 100; i++)
        stats_record_request(s);

    config_t cfg;
    setup_config(&cfg, 1);

    stats_serialize_ctx_t ctx = { .stats = s, .cfg = &cfg, .proxy = NULL };
    char buf[BUF_SIZE];
    int len = stats_serialize_json(buf, BUF_SIZE, &ctx);
    assert(len > 0);

    yyjson_doc *doc = yyjson_read(buf, (size_t)len, 0);
    assert(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_get_uint(yyjson_obj_get(root, "total_rpc_requests")) == 100);

    yyjson_doc_free(doc);
    stats_destroy(s);
    printf("  test_stats_record_request: OK\n");
}

static void test_stats_serialize_json_buffer_too_small(void)
{
    stats_t *s = stats_create(1);
    assert(s != NULL);

    config_t cfg;
    setup_config(&cfg, 1);

    stats_serialize_ctx_t ctx = { .stats = s, .cfg = &cfg, .proxy = NULL };
    char buf[10];
    int len = stats_serialize_json(buf, 10, &ctx);
    assert(len == -1);

    stats_destroy(s);
    printf("  test_stats_serialize_json_buffer_too_small: OK\n");
}

static void test_stats_serialize_json_null_proxy(void)
{
    stats_t *s = stats_create(1);
    assert(s != NULL);

    config_t cfg;
    setup_config(&cfg, 1);

    stats_serialize_ctx_t ctx = { .stats = s, .cfg = &cfg, .proxy = NULL };
    char buf[BUF_SIZE];
    int len = stats_serialize_json(buf, BUF_SIZE, &ctx);
    assert(len > 0);

    yyjson_doc *doc = yyjson_read(buf, (size_t)len, 0);
    assert(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *nodes = yyjson_obj_get(root, "nodes");
    yyjson_val *n0 = yyjson_arr_get(nodes, 0);
    yyjson_val *v = yyjson_obj_get(n0, "state");
    assert(v != NULL && yyjson_is_str(v));
    assert(strcmp(yyjson_get_str(v), "unknown") == 0);

    yyjson_doc_free(doc);
    stats_destroy(s);
    printf("  test_stats_serialize_json_null_proxy: OK\n");
}

int main(void)
{
    printf("test_stats:\n");
    test_stats_create_destroy();
    test_stats_record_gbt();
    test_stats_record_race_win();
    test_stats_record_notify_win();
    test_stats_record_notify_to_gbt();
    test_stats_record_request();
    test_stats_serialize_json_buffer_too_small();
    test_stats_serialize_json_null_proxy();
    printf("All stats tests passed.\n");
    return 0;
}
