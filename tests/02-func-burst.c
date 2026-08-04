/* Functional test of rb_push_int_burst / rb_pull_int_burst / rb_batch_size:
 * contracts, partial push into a full ring, drain, mixed single+burst across
 * wraps, batch-size law sanity, two-thread burst FIFO stress.
 * Works in both build variants (line format and -DRB_INT_INDEXED).
 * Usage: 02-func-burst [N-messages] [quick]
 *   quick: skip the rb_batch_size timing-sensitive section (for sanitizer /
 *   valgrind runs where per-call cost is inflated)
 * Exit: 0 on success; prints "ERRORS: n" as the last line. */
#define _DEFAULT_SOURCE          /* nanosleep under -std=c11 */
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
static _Atomic int stop;

static void single_thread(void)
{
    rb = rb_alloc_init(64, 1 << 20);
    CHECK(rb != NULL, "alloc(64)");
    if (!rb)
        return;
    int64_t in[128], out[128];
    for (int i = 0; i < 128; i++)
        in[i] = i;

    /* contracts */
    CHECK(rb_push_int_burst(NULL, in, 1) == RB_PARAM_ERROR, "push NULL ring");
    CHECK(rb_push_int_burst(rb, NULL, 1) == RB_PARAM_ERROR, "push NULL msgs");
    CHECK(rb_pull_int_burst(NULL, out, 1) == RB_PARAM_ERROR, "pull NULL ring");
    CHECK(rb_pull_int_burst(rb, NULL, 1) == RB_PARAM_ERROR, "pull NULL msgs");
    CHECK(rb_push_int_burst(rb, in, 0) == 0, "push n=0");
    CHECK(rb_pull_int_burst(rb, out, 0) == 0, "pull n=0");

    /* oversized push is partial, then drain everything */
    int n = rb_push_int_burst(rb, in, 128);
    CHECK(n > 0 && n <= 64, "oversized push returned %d", n);
    int m = rb_pull_int_burst(rb, out, 128);
    CHECK(m == n, "drained %d of %d", m, n);
    for (int i = 0; i < m; i++)
        CHECK(out[i] == i, "FIFO at %d: %ld", i, (long)out[i]);
    CHECK(rb_pull_int_burst(rb, out, 16) == 0, "pull from empty");

    /* mixed single+burst across many wraps */
    int64_t v = 0, w, expect = 0;
    for (int r = 0; r < 5000 && errors < 20; r++) {
        int64_t chunk[13];
        for (int i = 0; i < 13; i++)
            chunk[i] = v + i;
        int p = rb_push_int_burst(rb, chunk, 13);
        CHECK(p >= 0, "burst push rc %d", p);
        v += p;
        if (RB_OK == rb_push_int(rb, v))
            v++;
        int q = rb_pull_int_burst(rb, out, 7);
        for (int i = 0; i < q; i++, expect++)
            CHECK(out[i] == expect, "mixed FIFO at %ld", (long)expect);
        while (RB_OK == rb_pull_int(rb, &w)) {
            CHECK(w == expect, "mixed single FIFO at %ld", (long)expect);
            expect++;
        }
    }
    CHECK(v == expect, "mixed: pushed %ld pulled %ld", (long)v, (long)expect);
    rb_destroy(rb);
    printf("single-thread burst done\n");
}

static void batch_size_law(void)
{
    rb = rb_alloc_init(8192, 1 << 24);
    CHECK(rb != NULL, "alloc(8192)");
    if (!rb)
        return;
    CHECK(rb_batch_size(NULL) == 1, "batch_size(NULL)");

    /* slow producer (~2us per msg): B must stay 1 */
    struct timespec ts = { 0, 2000 };
    uint32_t b_slow = 1;
    for (int i = 0; i < 512; i++) {
        b_slow = rb_batch_size(rb);
        nanosleep(&ts, NULL);
    }
    CHECK(b_slow == 1, "slow producer B=%u", b_slow);

    /* tight loop: B must grow */
    uint32_t b_fast = 1;
    for (int i = 0; i < 100000; i++)
        b_fast = rb_batch_size(rb);
    CHECK(b_fast > 8, "saturated B=%u", b_fast);
    printf("batch size law: slow B=%u, saturated B=%u\n", b_slow, b_fast);
    rb_destroy(rb);
}

static void *cons(void *arg)
{
    long total = (long)(intptr_t)arg, got = 0;
    int64_t buf[512], expect = 0;
    while (got < total && !atomic_load_explicit(&stop,
                                                memory_order_relaxed)) {
        int k = rb_pull_int_burst(rb, buf, 512);
        if (k < 0) {
            CHECK(0, "2-thread pull rc %d", k);
            atomic_store(&stop, 1);
            return NULL;
        }
        for (int i = 0; i < k; i++, expect++)
            if (buf[i] != expect) {
                CHECK(0, "2-thread FIFO: got %ld want %ld",
                      (long)buf[i], (long)expect);
                atomic_store(&stop, 1);
                return NULL;
            }
        got += k;
    }
    return NULL;
}

static void two_thread(long total)
{
    rb = rb_alloc_init(8192, 1 << 24);
    CHECK(rb != NULL, "alloc(8192)");
    if (!rb)
        return;
    atomic_store(&stop, 0);
    pthread_t tc;
    CHECK(0 == pthread_create(&tc, NULL, cons, (void *)(intptr_t)total),
          "pthread_create");
    int64_t batch[256];
    long sent = 0;
    while (sent < total && !atomic_load_explicit(&stop,
                                                 memory_order_relaxed)) {
        uint32_t b = rb_batch_size(rb);
        if (b > 256)
            b = 256;
        if ((long)b > total - sent)
            b = (uint32_t)(total - sent);
        for (uint32_t i = 0; i < b; i++)
            batch[i] = sent + i;
        int p = rb_push_int_burst(rb, batch, b);
        if (p < 0) {
            CHECK(0, "2-thread push rc %d", p);
            break;
        }
        sent += p;
    }
    pthread_join(tc, NULL);
    CHECK(!atomic_load(&stop), "2-thread burst run failed");
    rb_destroy(rb);
    printf("2-thread burst done (%ld msgs)\n", total);
}

int main(int argc, char **argv)
{
    long n = (argc > 1) ? atol(argv[1]) : 0;
    if (n <= 0)
        n = 10000000;
    int quick = (argc > 2) && 0 == strncmp(argv[2], "quick", 5);

    single_thread();
    if (!quick)
        batch_size_law();
    two_thread(n);

    printf("ERRORS: %d\n", errors);
    return errors ? 1 : 0;
}
