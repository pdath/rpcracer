/* test_event_loop.c — Unit tests for event_loop module */

#include "../src/event_loop.h"
#include "../src/log.h"
#include "../src/util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

/* ---- Test helpers ---- */

static int g_cb_called = 0;
static int g_cb_fd = -1;
static uint32_t g_cb_events = 0;

static void
test_event_cb(event_loop_t *loop, int fd, uint32_t events, void *data)
{
    (void)loop;
    (void)data;
    g_cb_called++;
    g_cb_fd = fd;
    g_cb_events = events;
}

static int g_timer_fired = 0;

static void
test_timer_cb(event_loop_t *loop, void *data)
{
    (void)data;
    g_timer_fired++;
    /* Stop after first fire for testing */
    if (g_timer_fired >= 3)
        event_loop_stop(loop);
}

static void
single_fire_timer_cb(event_loop_t *loop, void *data)
{
    (void)data;
    g_timer_fired++;
    event_loop_stop(loop);
}

/* ---- Tests ---- */

static void
test_create_destroy(void)
{
    printf("  test_create_destroy... ");

    event_loop_t *loop = event_loop_create();
    assert(loop != NULL);
    event_loop_destroy(loop);

    printf("PASS\n");
}

static void
test_add_del_fd(void)
{
    printf("  test_add_del_fd... ");

    event_loop_t *loop = event_loop_create();
    assert(loop != NULL);

    /* Create a pipe for testing */
    int pipefd[2];
    assert(pipe(pipefd) == 0);

    /* Add read end */
    int rc = event_loop_add_fd(loop, pipefd[0], EPOLLIN, test_event_cb, NULL);
    assert(rc == 0);

    /* Duplicate add should fail */
    rc = event_loop_add_fd(loop, pipefd[0], EPOLLIN, test_event_cb, NULL);
    assert(rc == -1);

    /* Delete */
    rc = event_loop_del_fd(loop, pipefd[0]);
    assert(rc == 0);

    /* Delete non-existent should fail */
    rc = event_loop_del_fd(loop, pipefd[0]);
    assert(rc == -1);

    close(pipefd[0]);
    close(pipefd[1]);
    event_loop_destroy(loop);

    printf("PASS\n");
}

static void
test_mod_fd(void)
{
    printf("  test_mod_fd... ");

    event_loop_t *loop = event_loop_create();
    assert(loop != NULL);

    int pipefd[2];
    assert(pipe(pipefd) == 0);

    int rc = event_loop_add_fd(loop, pipefd[0], EPOLLIN, test_event_cb, NULL);
    assert(rc == 0);

    /* Modify to EPOLLOUT */
    rc = event_loop_mod_fd(loop, pipefd[0], EPOLLOUT);
    assert(rc == 0);

    /* Modify non-existent should fail */
    rc = event_loop_mod_fd(loop, 9999, EPOLLIN);
    assert(rc == -1);

    close(pipefd[0]);
    close(pipefd[1]);
    event_loop_destroy(loop);

    printf("PASS\n");
}

static void
test_timer_basic(void)
{
    printf("  test_timer_basic... ");

    event_loop_t *loop = event_loop_create();
    assert(loop != NULL);

    g_timer_fired = 0;

    /* 10ms timer — should fire quickly */
    int tfd = event_loop_add_timer(loop, 10, single_fire_timer_cb, NULL);
    assert(tfd >= 0);

    /* Run loop — will stop after timer fires once */
    event_loop_run(loop);

    assert(g_timer_fired == 1);

    event_loop_destroy(loop);

    printf("PASS\n");
}

static void
test_timer_recurring(void)
{
    printf("  test_timer_recurring... ");

    event_loop_t *loop = event_loop_create();
    assert(loop != NULL);

    g_timer_fired = 0;

    /* 5ms recurring timer — stop after 3 fires */
    int tfd = event_loop_add_timer(loop, 5, test_timer_cb, NULL);
    assert(tfd >= 0);

    event_loop_run(loop);

    assert(g_timer_fired >= 3);

    event_loop_destroy(loop);

    printf("PASS\n");
}

static void
test_fd_event_dispatch(void)
{
    printf("  test_fd_event_dispatch... ");

    event_loop_t *loop = event_loop_create();
    assert(loop != NULL);

    int pipefd[2];
    assert(pipe(pipefd) == 0);

    g_cb_called = 0;
    g_cb_fd = -1;

    /* Register read end for EPOLLIN */
    int rc = event_loop_add_fd(loop, pipefd[0], EPOLLIN, test_event_cb, NULL);
    assert(rc == 0);

    /* Also add a timer to stop the loop after a short delay */
    g_timer_fired = 0;
    event_loop_add_timer(loop, 50, single_fire_timer_cb, NULL);

    /* Write to pipe to trigger EPOLLIN */
    char msg[] = "hello";
    ssize_t w = write(pipefd[1], msg, sizeof(msg));
    assert(w == (ssize_t)sizeof(msg));

    /* Run loop — timer will stop it */
    event_loop_run(loop);

    /* The pipe read callback should have been called */
    assert(g_cb_called >= 1);
    assert(g_cb_fd == pipefd[0]);
    assert((g_cb_events & EPOLLIN) != 0);

    close(pipefd[0]);
    close(pipefd[1]);
    event_loop_destroy(loop);

    printf("PASS\n");
}

static void
test_stall_detection_normal(void)
{
    printf("  test_stall_detection_normal... ");

    /* Verify stall detection can be enabled without crashing
     * (we can't easily test the exit(1) path in a unit test) */
    event_loop_t *loop = event_loop_create();
    assert(loop != NULL);

    /* Enable stall detection with a generous threshold (won't trigger) */
    int rc = event_loop_enable_stall_detection(loop, 5000);
    assert(rc == 0);

    /* Add a timer to stop the loop quickly */
    g_timer_fired = 0;
    event_loop_add_timer(loop, 20, single_fire_timer_cb, NULL);

    /* Run briefly — stall detection should NOT trigger */
    event_loop_run(loop);

    event_loop_destroy(loop);

    printf("PASS\n");
}

static void
test_invalid_args(void)
{
    printf("  test_invalid_args... ");

    event_loop_t *loop = event_loop_create();
    assert(loop != NULL);

    /* NULL callback */
    assert(event_loop_add_fd(loop, 0, EPOLLIN, NULL, NULL) == -1);

    /* Negative fd */
    assert(event_loop_add_fd(loop, -1, EPOLLIN, test_event_cb, NULL) == -1);

    /* NULL loop */
    assert(event_loop_add_fd(NULL, 0, EPOLLIN, test_event_cb, NULL) == -1);

    /* Timer with 0 ms */
    assert(event_loop_add_timer(loop, 0, single_fire_timer_cb, NULL) == -1);

    /* Timer with NULL callback */
    assert(event_loop_add_timer(loop, 100, NULL, NULL) == -1);

    /* Stall detection with 0 threshold */
    assert(event_loop_enable_stall_detection(loop, 0) == -1);

    /* Stop and destroy with NULL */
    event_loop_stop(NULL);
    event_loop_destroy(NULL);

    event_loop_destroy(loop);

    printf("PASS\n");
}

static int g_user_val = 42;
static int *g_received_data = NULL;

static void
timer_with_data_cb(event_loop_t *l, void *data)
{
    g_received_data = (int *)data;
    event_loop_stop(l);
}

static void
test_user_data_passthrough(void)
{
    printf("  test_user_data_passthrough... ");

    event_loop_t *loop = event_loop_create();
    assert(loop != NULL);

    g_received_data = NULL;
    int tfd = event_loop_add_timer(loop, 10, timer_with_data_cb, &g_user_val);
    assert(tfd >= 0);

    event_loop_run(loop);

    assert(g_received_data == &g_user_val);
    assert(*g_received_data == 42);

    event_loop_destroy(loop);

    printf("PASS\n");
}

/* ---- Main ---- */

int
main(void)
{
    log_init(LOG_DEBUG);

    printf("test_event_loop:\n");
    test_create_destroy();
    test_add_del_fd();
    test_mod_fd();
    test_timer_basic();
    test_timer_recurring();
    test_fd_event_dispatch();
    test_stall_detection_normal();
    test_invalid_args();
    test_user_data_passthrough();
    printf("All event_loop tests passed.\n");

    return 0;
}
