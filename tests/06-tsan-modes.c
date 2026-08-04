/* Two-thread exercise of every protocol edge, for ThreadSanitizer runs and
 * the ordering-mutation gate: int single, int burst, ptr, wait — in one
 * binary, sequentially.  No timing assertions; small default N.
 * Works in both build variants (line format and -DRB_INT_INDEXED).
 * Usage: 06-tsan-modes [N-messages]  (default 100000)
 * Exit: 0 on success; prints "ERRORS: n" as the last line. */
#define _DEFAULT_SOURCE          /* nanosleep under -std=c11 */
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
static _Atomic int stop;
static char base[4096];

static void *cons_int(void *arg)
{
    long total = (long)(intptr_t)arg;
    int64_t w, expect = 0;
    while (expect < total && !atomic_load_explicit(&stop,
                                                   memory_order_relaxed)) {
        if (rb_pull_int(rb, &w) != RB_OK)
            continue;
        if (w != expect) {
            CHECK(0, "int FIFO: got %ld want %ld", (long)w, (long)expect);
            atomic_store(&stop, 1);
            return NULL;
        }
        expect++;
    }
    return NULL;
}

static void *cons_burst(void *arg)
{
    long total = (long)(intptr_t)arg, got = 0;
    int64_t buf[256], expect = 0;
    while (got < total && !atomic_load_explicit(&stop,
                                                memory_order_relaxed)) {
        int k = rb_pull_int_burst(rb, buf, 256);
        for (int i = 0; i < k; i++, expect++)
            if (buf[i] != expect) {
                CHECK(0, "burst FIFO: got %ld want %ld",
                      (long)buf[i], (long)expect);
                atomic_store(&stop, 1);
                return NULL;
            }
        got += k;
    }
    return NULL;
}

static void *cons_ptr(void *arg)
{
    long total = (long)(intptr_t)arg;
    int64_t expect = 0;
    while (expect < total && !atomic_load_explicit(&stop,
                                                   memory_order_relaxed)) {
        void *data = NULL;
        size_t size = 0;
        if (rb_pull_ptr(rb, &data, &size) != RB_OK)
            continue;
        if (data != base + (expect % 4096) ||
            size != (size_t)(expect % 1000) + 1) {
            CHECK(0, "ptr FIFO: mismatch at %ld", (long)expect);
            atomic_store(&stop, 1);
            return NULL;
        }
        expect++;
    }
    return NULL;
}

static void *cons_wait(void *arg)
{
    long total = (long)(intptr_t)arg;
    int64_t v = -1, expect = 0;
    struct timespec nap = { 0, 100000 };    /* forces the producer to park */
    while (expect < total) {
        int rc = rb_pull_wait(rb, &v, 0);
        if (rc != RB_OK || v != expect) {
            CHECK(0, "wait FIFO: rc=%d got %ld want %ld", rc, (long)v,
                  (long)expect);
            atomic_store(&stop, 1);
            rb_wake(rb);
            return NULL;
        }
        expect++;
        if ((expect & 0x1FFF) == 0)
            nanosleep(&nap, NULL);
    }
    return NULL;
}

static void run_mode(const char *name, void *(*consumer)(void *),
                     long total, size_t ring, int mode)
{
    rb = (mode == 2) ? rb_alloc_init_ptr(ring, 1 << 22)
                     : rb_alloc_init(ring, 1 << 22);
    CHECK(rb != NULL, "%s: alloc(%zu)", name, ring);
    if (!rb)
        return;
    atomic_store(&stop, 0);
    pthread_t tc;
    CHECK(0 == pthread_create(&tc, NULL, consumer,
                              (void *)(intptr_t)total), "%s: create", name);
    struct timespec nap = { 0, 150000 };    /* forces the consumer to park */
    int64_t chunk[64];
    for (long i = 0; i < total &&
                     !atomic_load_explicit(&stop, memory_order_relaxed); ) {
        switch (mode) {
        case 0:                             /* int single */
            if (rb_push_int(rb, i) == RB_OK)
                i++;
            break;
        case 1: {                           /* int burst, 1..64 per call */
            long n = 1 + (i % 64);
            if (n > total - i)
                n = total - i;
            for (long k = 0; k < n; k++)
                chunk[k] = i + k;
            i += rb_push_int_burst(rb, chunk, (size_t)n);
            break;
        }
        case 2:                             /* ptr */
            if (rb_push_ptr(rb, base + (i % 4096),
                            (size_t)(i % 1000) + 1) == RB_OK)
                i++;
            break;
        case 3:                             /* wait */
            if (rb_push_wait(rb, i, 0) != RB_OK) {
                CHECK(0, "wait push failed at %ld", i);
                atomic_store(&stop, 1);
                rb_wake(rb);
                break;
            }
            i++;
            if ((i & 0x3FFF) == 0)
                nanosleep(&nap, NULL);
            break;
        }
    }
    pthread_join(tc, NULL);
    CHECK(!atomic_load(&stop), "%s: run failed", name);
    rb_destroy(rb);
    printf("%s done (%ld msgs)\n", name, total);
}

int main(int argc, char **argv)
{
    long n = (argc > 1) ? atol(argv[1]) : 0;
    if (n <= 0)
        n = 100000;

    run_mode("int-single", cons_int, n, 256, 0);
    run_mode("int-burst", cons_burst, n * 4, 256, 1);
    run_mode("ptr", cons_ptr, n, 256, 2);
    run_mode("wait", cons_wait, n / 2, 64, 3);

    printf("ERRORS: %d\n", errors);
    return errors ? 1 : 0;
}
