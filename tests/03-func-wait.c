/* Functional test of rb_push_wait / rb_pull_wait / rb_wake:
 * FIFO with forced parking on both sides, pull timeout on empty, push
 * timeout on full, rb_wake (RB_CLOSED) semantics, wake-latency probe.
 * Works in both build variants (line format and -DRB_INT_INDEXED).
 * Usage: 03-func-wait [N-messages] [quick]
 *   quick: skip the wake-latency probe and the upper timing bounds (for
 *   valgrind / sanitizer runs where wall-clock behaviour is distorted)
 * Exit: 0 on success; prints "ERRORS: n" as the last line. */
#define _DEFAULT_SOURCE          /* nanosleep, clock_gettime under -std=c11 */
#include <string.h>
#include <time.h>
#include "ring_buf.h"

static int errors;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        errors++; \
    } \
} while (0)

static ring_buf_t *rb;
static _Atomic long consumed_total;
static _Atomic int t_fail;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* consumer with periodic naps: forces the producer to park on FULL */
static void *cons_slowfast(void *arg)
{
    long total = (long)(intptr_t)arg;
    int64_t v = -1, expect = 0;
    struct timespec nap = { 0, 200000 };
    while (expect < total) {
        int rc = rb_pull_wait(rb, &v, 0);
        if (rc != RB_OK || v != expect) {
            CHECK(0, "cons: rc=%d got %ld want %ld", rc, (long)v,
                  (long)expect);
            atomic_store(&t_fail, 1);
            rb_wake(rb);            /* unblock the producer */
            return NULL;
        }
        expect++;
        atomic_store(&consumed_total, expect);
        if ((expect & 0x3FFF) == 0)
            nanosleep(&nap, NULL);
    }
    return NULL;
}

/* producer with periodic naps: forces the consumer to park on EMPTY */
static void *prod_bursty(void *arg)
{
    long total = (long)(intptr_t)arg;
    struct timespec nap = { 0, 300000 };
    for (long i = 0; i < total; i++) {
        int rc = rb_push_wait(rb, i, 0);
        if (rc != RB_OK) {
            CHECK(0, "prod: rc=%d at %ld", rc, i);
            atomic_store(&t_fail, 1);
            rb_wake(rb);            /* unblock the consumer */
            return NULL;
        }
        if ((i & 0xFFFF) == 0)
            nanosleep(&nap, NULL);
        if (atomic_load_explicit(&t_fail, memory_order_relaxed))
            return NULL;
    }
    return NULL;
}

static void *cons_forever(void *arg)
{
    (void)arg;
    int64_t v;
    return (void *)(intptr_t)rb_pull_wait(rb, &v, 0);
}

enum { N_PROBE = 100 };
static _Atomic uint64_t t_push;
static uint64_t lat[N_PROBE];

static void *probe_body(void *arg)
{
    (void)arg;
    int64_t v;
    for (int i = 0; i < N_PROBE; i++) {
        if (rb_pull_wait(rb, &v, 0) != RB_OK) {
            atomic_store(&t_fail, 1);
            return NULL;
        }
        lat[i] = now_ns() - atomic_load(&t_push);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    long total = (argc > 1) ? atol(argv[1]) : 0;
    if (total <= 0)
        total = 2000000;
    int quick = (argc > 2) && 0 == strncmp(argv[2], "quick", 5);
    int64_t v;

    /* contracts */
    CHECK(rb_push_wait(NULL, 1, 0) == RB_PARAM_ERROR, "push_wait(NULL)");
    CHECK(rb_pull_wait(NULL, &v, 0) == RB_PARAM_ERROR, "pull_wait(NULL)");

    /* 1. FIFO with forced parking on both sides (tiny ring) */
    rb = rb_alloc_init(64, 1 << 20);
    CHECK(rb != NULL, "alloc(64)");
    if (!rb)
        goto out;
    CHECK(rb_pull_wait(rb, NULL, 0) == RB_PARAM_ERROR, "pull_wait NULL out");
    pthread_t tc, tp;
    CHECK(0 == pthread_create(&tc, NULL, cons_slowfast,
                              (void *)(intptr_t)total), "create cons");
    CHECK(0 == pthread_create(&tp, NULL, prod_bursty,
                              (void *)(intptr_t)total), "create prod");
    pthread_join(tp, NULL);
    pthread_join(tc, NULL);
    CHECK(!atomic_load(&t_fail), "parking FIFO failed");
    CHECK(atomic_load(&consumed_total) == total, "consumed %ld of %ld",
          atomic_load(&consumed_total), total);
    rb_destroy(rb);
    printf("parking FIFO done (%ld msgs, ring 64)\n", total);

    /* 2. pull timeout on empty, push timeout on full */
    rb = rb_alloc_init(64, 1 << 20);
    CHECK(rb != NULL, "alloc(64)");
    if (!rb)
        goto out;
    uint64_t t0 = now_ns();
    int rc = rb_pull_wait(rb, &v, 50 * 1000 * 1000ull);
    uint64_t dt = now_ns() - t0;
    CHECK(RB_TIMEOUT == rc, "pull timeout rc=%d", rc);
    CHECK(dt > 45ull * 1000 * 1000, "pull timeout too early: %.1fms",
          (double)dt / 1e6);
    if (!quick)
        CHECK(dt < 500ull * 1000 * 1000, "pull timeout too late: %.1fms",
              (double)dt / 1e6);
    while (rb_push_int(rb, 0) == RB_OK)     /* fill the ring */
        ;
    t0 = now_ns();
    rc = rb_push_wait(rb, 1, 20 * 1000 * 1000ull);
    dt = now_ns() - t0;
    CHECK(RB_TIMEOUT == rc, "push timeout rc=%d", rc);
    CHECK(dt > 15ull * 1000 * 1000, "push timeout too early: %.1fms",
          (double)dt / 1e6);
    printf("timeouts done\n");

    /* 3. rb_wake: a sleeping consumer returns RB_CLOSED; later calls too */
    while (rb_pull_int(rb, &v) == RB_OK)    /* drain so the consumer parks */
        ;
    pthread_t tw;
    CHECK(0 == pthread_create(&tw, NULL, cons_forever, NULL), "create");
    struct timespec nap = { 0, 50 * 1000 * 1000 };
    nanosleep(&nap, NULL);                  /* let it park */
    rb_wake(rb);
    void *ret;
    pthread_join(tw, &ret);
    CHECK(RB_CLOSED == (int)(intptr_t)ret, "woken consumer rc=%d",
          (int)(intptr_t)ret);
    CHECK(RB_CLOSED == rb_pull_wait(rb, &v, 1000), "closed pull");
    while (rb_push_int(rb, 0) == RB_OK)     /* fill, then blocking push */
        ;
    CHECK(RB_CLOSED == rb_push_wait(rb, 1, 1000), "closed push");
    rb_destroy(rb);
    printf("rb_wake done\n");

    /* 4. wake-latency probe: consumer parks between 300us-spaced pushes */
    if (!quick) {
        rb = rb_alloc_init(1024, 1 << 22);
        CHECK(rb != NULL, "alloc(1024)");
        if (!rb)
            goto out;
        pthread_t tl;
        CHECK(0 == pthread_create(&tl, NULL, probe_body, NULL), "create");
        struct timespec gap = { 0, 300000 };
        for (int i = 0; i < N_PROBE; i++) {
            nanosleep(&gap, NULL);
            atomic_store(&t_push, now_ns());
            CHECK(RB_OK == rb_push_wait(rb, i, 0), "probe push %d", i);
        }
        pthread_join(tl, NULL);
        CHECK(!atomic_load(&t_fail), "probe consumer failed");
        rb_destroy(rb);
        for (int i = 0; i < N_PROBE; i++)
            for (int j = i + 1; j < N_PROBE; j++)
                if (lat[j] < lat[i]) {
                    uint64_t t = lat[i];
                    lat[i] = lat[j];
                    lat[j] = t;
                }
        printf("wake latency (parked): p50=%.1fus p90=%.1fus max=%.1fus\n",
               (double)lat[N_PROBE / 2] / 1e3,
               (double)lat[N_PROBE * 9 / 10] / 1e3,
               (double)lat[N_PROBE - 1] / 1e3);
    }

out:
    printf("ERRORS: %d\n", errors);
    return errors ? 1 : 0;
}
