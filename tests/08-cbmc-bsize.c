/* CBMC harness: rb_batch_size fast path + sampling branch under an
 * arbitrary clock (rb_cycles is nondet under __CPROVER__).  The estimator
 * state is seeded directly (est[0]=63) so the next call takes the sampling
 * branch without unwinding 64 calls. */
#include <assert.h>
#include "ring_buf.h"

static _Alignas(128) unsigned char buf[sizeof(ring_buf_t) + 64];

int main(void)
{
    ring_buf_t *d = (ring_buf_t *)buf;
    d->capacity = 7;
    d->mask = 0;
#ifdef RB_INT_INDEXED
    d->capacity = 8;
    d->mask = 7;
#endif

    assert(rb_batch_size(NULL) == 1);

    assert(rb_batch_size(d) >= 1);          /* fast path */
    d->est[0] = 63;
    assert(rb_batch_size(d) >= 1);          /* first sampling pass */
    d->est[0] = 63;
    assert(rb_batch_size(d) >= 1);          /* warm-EWMA sampling pass */

    return 0;
}
