/* CBMC harness: allocation contracts + single push/pull FIFO round trips
 * over a statically placed tiny ring (this CBMC has no aligned_alloc model).
 * Two laps cover the wrap / line-handback paths.  Nondet payloads.
 * Works in both build variants (line format and -DRB_INT_INDEXED). */
#include <assert.h>
#include "ring_buf.h"

int64_t nondet_i64(void);

static _Alignas(128) unsigned char buf[sizeof(ring_buf_t) + 64];
static _Alignas(128) unsigned char pbuf[sizeof(ring_buf_t) + 8 * 16];

#ifdef RB_INT_INDEXED
#define CAP 8
#else
#define CAP 7
#endif

int main(void)
{
    /* allocation contracts: every reject path returns NULL before malloc */
    assert(rb_alloc_init(0, 1 << 20) == NULL);
    assert(rb_alloc_init(24, 1 << 20) == NULL);
    assert(rb_alloc_init(1 << 16, 100) == NULL);
    assert(rb_alloc_init((size_t)1 << 63, 1 << 20) == NULL);
    assert(rb_alloc_init_ptr(24, 1 << 20) == NULL);
    assert(rb_alloc_init_ptr(1 << 16, 100) == NULL);

    /* argument contracts */
    int64_t v;
    assert(rb_push_int(NULL, 1) == RB_PARAM_ERROR);
    assert(rb_pull_int(NULL, &v) == RB_PARAM_ERROR);

    /* int64 ring: two full fill/drain laps, FIFO + payload integrity */
    ring_buf_t *d = (ring_buf_t *)buf;
    d->capacity = CAP;
    d->mask = 0;
#ifdef RB_INT_INDEXED
    d->mask = 7;
#endif
    assert(rb_pull_int(d, NULL) == RB_PARAM_ERROR);
    assert(rb_pull_int(d, &v) == RB_EMPTY);

    int64_t in[2][CAP];
    for (int lap = 0; lap < 2; lap++) {
        for (int i = 0; i < CAP; i++) {
            in[lap][i] = nondet_i64();
            assert(rb_push_int(d, in[lap][i]) == RB_OK);
        }
        assert(rb_push_int(d, 0) == RB_FULL);
        for (int i = 0; i < CAP; i++) {
            assert(rb_pull_int(d, &v) == RB_OK);
            assert(v == in[lap][i]);
        }
        assert(rb_pull_int(d, &v) == RB_EMPTY);
    }

    /* ptr ring: contracts + one lap */
    ring_buf_t *p = (ring_buf_t *)pbuf;
    p->capacity = 8;
    p->mask = 7;
    void *data = NULL;
    size_t size = 0;
    assert(rb_push_ptr(NULL, &v, 1) == RB_PARAM_ERROR);
    assert(rb_push_ptr(p, &v, (size_t)INT32_MAX + 1) == RB_PARAM_ERROR);
    assert(rb_pull_ptr(p, NULL, &size) == RB_PARAM_ERROR);
    assert(rb_pull_ptr(p, &data, NULL) == RB_PARAM_ERROR);
    data = &v;
    assert(rb_pull_ptr(p, &data, &size) == RB_PARAM_ERROR);
    data = NULL;
    size = 1;
    assert(rb_pull_ptr(p, &data, &size) == RB_PARAM_ERROR);
    size = 0;
    assert(rb_pull_ptr(p, &data, &size) == RB_EMPTY);

    for (int i = 0; i < 8; i++)
        assert(rb_push_ptr(p, buf + i, (size_t)i + 1) == RB_OK);
    assert(rb_push_ptr(p, buf, 1) == RB_FULL);
    for (int i = 0; i < 8; i++) {
        data = NULL;
        size = 0;
        assert(rb_pull_ptr(p, &data, &size) == RB_OK);
        assert(data == buf + i && size == (size_t)i + 1);
    }
    return 0;
}
