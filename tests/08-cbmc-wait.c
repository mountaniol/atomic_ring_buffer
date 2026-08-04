/* CBMC harness: rb_push_wait / rb_pull_wait / rb_wake, single-threaded,
 * arbitrary clock and futex results (nondet under __CPROVER__), bounded
 * exits via closed=1.  Build with -DRB_WAIT_SPIN=2 to keep the spin loop
 * small.  Works in both build variants. */
#include <assert.h>
#include "ring_buf.h"

static _Alignas(128) unsigned char buf[sizeof(ring_buf_t) + 64];

int main(void)
{
    ring_buf_t *d = (ring_buf_t *)buf;
    d->capacity = 7;                    /* one line (line build) */
    d->mask = 0;
#ifdef RB_INT_INDEXED
    d->capacity = 8;
    d->mask = 7;
#endif

    int64_t v;

    /* success paths, with a parked peer to drive the signal/xchg branch */
    atomic_store(&d->csleep, 1);
    assert(rb_push_wait(d, 42, 0) == RB_OK);
    atomic_store(&d->psleep, 1);
    assert(rb_pull_wait(d, &v, 1000) == RB_OK);
    assert(v == 42);

    /* closed ring: blocking paths must exit with RB_CLOSED */
    rb_wake(d);
    assert(rb_pull_wait(d, &v, 5) == RB_CLOSED);
    while (rb_push_wait(d, 1, 0) == RB_OK)     /* fill to full */
        ;
    assert(rb_push_wait(d, 2, 5) == RB_CLOSED);

    assert(rb_pull_wait(d, NULL, 0) == RB_PARAM_ERROR);
    assert(rb_push_wait(NULL, 1, 0) == RB_PARAM_ERROR);
    return 0;
}
