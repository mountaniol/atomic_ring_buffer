/* CBMC harness: burst push/pull with a CONCRETE n (-DN_CONST=0..8) over a
 * statically placed one-line ring.  Symbolic n makes the two-piece memcpy
 * intractable (SAT OOM at the 4G cap) and mid-line cursor seeding trips
 * CBMC's byte-extract imprecision on the line struct, so: n is covered
 * exhaustively by separate runs, the line variant starts from the empty
 * state, and the indexed variant additionally seeds tail=head=6 (a valid
 * empty state) so n>=3 exercises the wrapping second memcpy.  Mid-line
 * burst resume is covered by the functional tests and the AFL corpus
 * (100% branch coverage), not by this harness.
 * Works in both build variants (line format and -DRB_INT_INDEXED). */
#include <assert.h>
#include "ring_buf.h"

#ifndef N_CONST
#define N_CONST 4
#endif

int64_t nondet_i64(void);

/* Verification stand-in for memcpy (the script builds ring_buf.c with
 * -Dmemcpy=rb_cbmc_memcpy): CBMC's byte-blasted builtin memcpy is both
 * imprecise through the data[] reinterpretation cast (spurious FIFO
 * counterexamples) and the SAT-size driver (OOM at the 4G cap).  All
 * ring memcpys move whole int64 cells, so a word-wise loop is
 * byte-identical semantics with a precise, small encoding. */
void *rb_cbmc_memcpy(void *dst, const void *src, size_t nbytes)
{
    int64_t *d = dst;
    const int64_t *s = src;
    for (size_t i = 0; i < nbytes / 8; i++)
        d[i] = s[i];
    return dst;
}

static _Alignas(128) unsigned char buf[sizeof(ring_buf_t) + 64];

#ifdef RB_INT_INDEXED
#define CAP 8
#else
#define CAP 7
#endif

int main(void)
{
    ring_buf_t *d = (ring_buf_t *)buf;
    d->capacity = CAP;
    d->mask = 0;
#ifdef RB_INT_INDEXED
    d->mask = 7;
    /* valid empty state mid-ring: burst with n>=3 wraps at index 8 */
    atomic_store(&d->tail, 6);
    d->head_shadow = 6;
    atomic_store(&d->head, 6);
    d->tail_shadow = 6;
#endif

    int64_t in[N_CONST + 1], out[N_CONST + 1];

    /* argument contracts */
    assert(rb_push_int_burst(NULL, in, 1) == RB_PARAM_ERROR);
    assert(rb_push_int_burst(d, NULL, 1) == RB_PARAM_ERROR);
    assert(rb_pull_int_burst(NULL, out, 1) == RB_PARAM_ERROR);
    assert(rb_pull_int_burst(d, NULL, 1) == RB_PARAM_ERROR);

    /* one burst round: exact clamp, FIFO, payload integrity */
    for (int i = 0; i < N_CONST; i++)
        in[i] = nondet_i64();
    int pushed = rb_push_int_burst(d, in, N_CONST);
    assert(pushed == (N_CONST <= CAP ? N_CONST : CAP));
    int pulled = rb_pull_int_burst(d, out, N_CONST ? N_CONST : 1);
    assert(pulled == pushed);
    for (int i = 0; i < pulled; i++)
        assert(out[i] == in[i]);

    /* ring is empty again */
    assert(rb_pull_int_burst(d, out, 1) == 0);
    return 0;
}
