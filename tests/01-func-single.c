/* Functional test of the single-message API: argument contracts, FIFO and
 * full/empty edges across many wraps (int64 + ptr rings, single thread),
 * then two-thread FIFO stress for both rings.
 * Works in both build variants (line format and -DRB_INT_INDEXED).
 * Usage: 01-func-single [N-messages]  (default 3000000)
 * Exit: 0 on success; prints "ERRORS: n" as the last line. */
#include <string.h>
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

/* --- section A: argument contracts ------------------------------------- */
static void contracts(void)
{
    CHECK(rb_alloc_init(0, 1 << 20) == NULL, "size 0 accepted");
    CHECK(rb_alloc_init(24, 1 << 20) == NULL, "non-power-of-2 accepted");
    CHECK(rb_alloc_init(1 << 16, 100) == NULL, "alloc limit ignored");
    CHECK(rb_alloc_init((size_t)1 << 63, 1 << 20) == NULL,
          "size overflow accepted");
    CHECK(rb_alloc_init_ptr(24, 1 << 20) == NULL, "ptr non-power-of-2");
    CHECK(rb_alloc_init_ptr(1 << 16, 100) == NULL, "ptr alloc limit");

    int64_t v;
    CHECK(rb_push_int(NULL, 1) == RB_PARAM_ERROR, "push_int(NULL)");
    CHECK(rb_pull_int(NULL, &v) == RB_PARAM_ERROR, "pull_int(NULL)");

    ring_buf_t *d = rb_alloc_init(8, 1 << 20);
    CHECK(d != NULL, "alloc(8)");
    if (!d)
        return;
    CHECK(rb_pull_int(d, NULL) == RB_PARAM_ERROR, "pull_int(d, NULL)");
    CHECK(rb_pull_int(d, &v) == RB_EMPTY, "pull from empty");
    rb_destroy(d);

    ring_buf_t *p = rb_alloc_init_ptr(8, 1 << 20);
    CHECK(p != NULL, "alloc_ptr(8)");
    if (!p)
        return;
    void *data = NULL;
    size_t size = 0;
    CHECK(rb_push_ptr(NULL, &v, 1) == RB_PARAM_ERROR, "push_ptr(NULL)");
    CHECK(rb_push_ptr(p, &v, (size_t)INT32_MAX + 1) == RB_PARAM_ERROR,
          "push_ptr size > INT32_MAX");
    CHECK(rb_pull_ptr(p, NULL, &size) == RB_PARAM_ERROR, "pull_ptr NULL data");
    CHECK(rb_pull_ptr(p, &data, NULL) == RB_PARAM_ERROR, "pull_ptr NULL size");
    data = &v;
    CHECK(rb_pull_ptr(p, &data, &size) == RB_PARAM_ERROR,
          "pull_ptr *data != NULL");
    data = NULL;
    size = 1;
    CHECK(rb_pull_ptr(p, &data, &size) == RB_PARAM_ERROR,
          "pull_ptr *size != 0");
    size = 0;
    CHECK(rb_pull_ptr(p, &data, &size) == RB_EMPTY, "pull_ptr from empty");
    rb_destroy(p);
    printf("contracts done\n");
}

/* --- section B: int64 fill/drain laps across wraps ---------------------- */
static void int_laps(void)
{
    ring_buf_t *d = rb_alloc_init(8, 1 << 20);
    CHECK(d != NULL, "alloc(8)");
    if (!d)
        return;
    uint64_t cap = d->capacity;
    int64_t v = 0, w, expect = 0;

    for (int lap = 0; lap < 2000; lap++) {
        uint64_t n = 0;
        while (rb_push_int(d, v) == RB_OK) {
            v++;
            n++;
        }
        CHECK(n == cap, "lap %d: filled %lu of %lu", lap,
              (unsigned long)n, (unsigned long)cap);
        CHECK(rb_push_int(d, v) == RB_FULL, "lap %d: not FULL", lap);
        while (rb_pull_int(d, &w) == RB_OK) {
            CHECK(w == expect, "lap %d: got %ld want %ld", lap,
                  (long)w, (long)expect);
            expect++;
            if (errors > 20)
                goto out;
        }
        CHECK(rb_pull_int(d, &w) == RB_EMPTY, "lap %d: not EMPTY", lap);
    }
    CHECK(v == expect, "pushed %ld pulled %ld", (long)v, (long)expect);
out:
    rb_destroy(d);
    printf("int laps done (capacity %lu)\n", (unsigned long)cap);
}

/* --- section C: ptr ring fill/drain laps -------------------------------- */
static char base[4096];

static void ptr_laps(void)
{
    ring_buf_t *d = rb_alloc_init_ptr(8, 1 << 20);
    CHECK(d != NULL, "alloc_ptr(8)");
    if (!d)
        return;
    int64_t v = 0, expect = 0;

    for (int lap = 0; lap < 2000; lap++) {
        uint64_t n = 0;
        while (rb_push_ptr(d, base + (v % 4096), (size_t)(v % 1000) + 1)
               == RB_OK) {
            v++;
            n++;
        }
        CHECK(n == d->capacity, "lap %d: filled %lu", lap, (unsigned long)n);
        void *data;
        size_t size;
        for (;;) {
            data = NULL;
            size = 0;
            if (rb_pull_ptr(d, &data, &size) != RB_OK)
                break;
            CHECK(data == base + (expect % 4096) &&
                  size == (size_t)(expect % 1000) + 1,
                  "lap %d: ptr/size mismatch at %ld", lap, (long)expect);
            expect++;
            if (errors > 20)
                goto out;
        }
    }
    CHECK(v == expect, "ptr pushed %ld pulled %ld", (long)v, (long)expect);
out:
    rb_destroy(d);
    printf("ptr laps done\n");
}

/* --- sections D/E: two-thread FIFO stress ------------------------------- */
static ring_buf_t *rb;
static _Atomic int stop;

static void *int_cons(void *arg)
{
    long total = (long)(intptr_t)arg;
    int64_t w, expect = 0;
    while (expect < total && !atomic_load_explicit(&stop,
                                                   memory_order_relaxed)) {
        if (rb_pull_int(rb, &w) != RB_OK)
            continue;
        if (w != expect) {
            CHECK(0, "2-thread int: got %ld want %ld", (long)w, (long)expect);
            atomic_store(&stop, 1);
            return NULL;
        }
        expect++;
    }
    return NULL;
}

static void *ptr_cons(void *arg)
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
            CHECK(0, "2-thread ptr: mismatch at %ld", (long)expect);
            atomic_store(&stop, 1);
            return NULL;
        }
        expect++;
    }
    return NULL;
}

static void two_thread(long total, int ptr_mode)
{
    rb = ptr_mode ? rb_alloc_init_ptr(1024, 1 << 22)
                  : rb_alloc_init(1024, 1 << 22);
    CHECK(rb != NULL, "alloc(1024)");
    if (!rb)
        return;
    atomic_store(&stop, 0);
    pthread_t tc;
    CHECK(0 == pthread_create(&tc, NULL, ptr_mode ? ptr_cons : int_cons,
                              (void *)(intptr_t)total), "pthread_create");
    for (long i = 0; i < total &&
                     !atomic_load_explicit(&stop, memory_order_relaxed); ) {
        int rc = ptr_mode
            ? rb_push_ptr(rb, base + (i % 4096), (size_t)(i % 1000) + 1)
            : rb_push_int(rb, i);
        if (rc == RB_OK)
            i++;
    }
    pthread_join(tc, NULL);
    CHECK(!atomic_load(&stop), "2-thread %s run failed",
          ptr_mode ? "ptr" : "int");
    rb_destroy(rb);
    printf("2-thread %s done (%ld msgs)\n", ptr_mode ? "ptr" : "int", total);
}

int main(int argc, char **argv)
{
    long n = (argc > 1) ? atol(argv[1]) : 0;
    if (n <= 0)
        n = 3000000;

    contracts();
    int_laps();
    ptr_laps();
    two_thread(n, 0);
    two_thread(n / 4, 1);

    printf("ERRORS: %d\n", errors);
    return errors ? 1 : 0;
}
