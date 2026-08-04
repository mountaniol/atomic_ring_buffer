/* GenMC harness (RC11 model): the core SPSC protocol, 1 producer + 1
 * consumer pthread, tiny static ring, 3 messages.  GenMC explores every
 * consistent execution, so a too-weak memory_order on any protocol edge
 * shows up as an assertion failure or a stale read - the check CBMC (SC
 * between atomics) and x86 execution (TSO) structurally cannot do.
 *
 * The library source is included directly; the harness only touches the
 * non-blocking int64 API, so rdtsc/futex/syscall paths are never called.
 * Build variants: default and -DRB_INT_INDEXED. */
#include <assert.h>
#include <pthread.h>
#include "ring_buf.c"

#define N 3

static _Alignas(128) unsigned char rbuf[sizeof(ring_buf_t) + 64];
static ring_buf_t *d = (ring_buf_t *)rbuf;

static void *prod(void *arg)
{
    (void)arg;
    for (int64_t i = 0; i < N; i++)
        while (rb_push_int(d, i + 100) != RB_OK)
            ;
    return NULL;
}

static void *cons(void *arg)
{
    (void)arg;
    int64_t v;
    for (int64_t i = 0; i < N; i++) {
        while (rb_pull_int(d, &v) != RB_OK)
            ;
        assert(v == i + 100);           /* FIFO + payload integrity */
    }
    return NULL;
}

int main(void)
{
    d->capacity = 7;                    /* one line (line build) */
    d->mask = 0;
#ifdef RB_INT_INDEXED
    d->capacity = 8;
    d->mask = 7;
#endif

    pthread_t tp, tc;
    pthread_create(&tp, NULL, prod, NULL);
    pthread_create(&tc, NULL, cons, NULL);
    pthread_join(tp, NULL);
    pthread_join(tc, NULL);

    int64_t v;
    assert(rb_pull_int(d, &v) == RB_EMPTY);     /* ring drained */
    return 0;
}
