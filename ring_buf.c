/*
 * Lock-free SPSC Ring Buffer.
 *
 * Synchronization uses exactly one release-store / acquire-load pair per
 * direction; no fences, no read-modify-write, no seq_cst anywhere.  On
 * x86-TSO every atomic below compiles to a plain MOV; on ARM64 to LDAR/STLR.
 *
 * Default int64 ring ("line" format): each 64B line is {seq; 7 x int64} and
 * seq doubles as the publication flag.  The cache-line transfer that delivers
 * the flag delivers the payload too - one cross-core hop instead of two, so
 * both the single-message latency and the measured streaming throughput are
 * the best of the two variants.
 *
 * With -DRB_INT_INDEXED the int64 ring uses 8-byte cells, 8 per cache line;
 * the peer index is re-read only when the ring appears full/empty (private
 * shadow, Rigtorp scheme).  Highest per-message ceiling when the application
 * itself maintains a backlog, at twice the single-message latency.
 *
 * The ptr+size ring always uses the indexed scheme with 16-byte cells.
 */

#include <string.h>
#include <stdlib.h>
#include "ring_buf.h"

/* Views of the data[] section, per ring mode */
#define INT_CELLS(d) ((int64_t *)(void *)(d)->data)
#define PTR_CELLS(d) ((cell_t *)(void *)(d)->data)

#ifndef RB_INT_INDEXED
#define RB_LINE_MSGS 7  /* int64 messages per 64B line; 8th slot is the flag */

typedef struct {
    _Atomic uint64_t seq;         /* 0 = line free; 1..7 = valid messages */
    int64_t msg[RB_LINE_MSGS];
} rb_line_t;

_Static_assert(sizeof(rb_line_t) == 64, "one cache line per rb_line_t");
#define LINES(d) ((rb_line_t *)(void *)(d)->data)
#endif

/**
 * @brief Allocate a zeroed, 128B-aligned, prefaulted buffer
 * @param size_t count          Number of data elements
 * @param size_t elem_size      Size of one element
 * @param size_t max_alloc_size Allocation limit
 * @return ring_buf_t* Buffer with max_alloc_size set, or NULL on error
 */
static ring_buf_t *rb_init_common(size_t count, size_t elem_size,
                                  size_t max_alloc_size)
{
    size_t total;
    ring_buf_t *d;

    /* Reject before multiplying: a wrapped product would defeat the limit */
    if (count > (SIZE_MAX - sizeof(ring_buf_t) - 127) / elem_size)
        return NULL;

    total = sizeof(ring_buf_t) + count * elem_size;
    total = (total + 127) & ~(size_t)127;  /* aligned_alloc needs a multiple */
    if (total > max_alloc_size)
        return NULL;  /* Prevent excessive memory usage */

    d = aligned_alloc(128, total);
    if (NULL == d) {
        perror("Can not allocate aligned memory: ");
        return NULL;
    }

    /* Zero init: initializes every _Atomic field (all-zero bytes are valid
     * on the supported targets) and faults every page in up front */
    memset(d, 0, total);

    d->max_alloc_size = max_alloc_size;
    return d;
}

ring_buf_t *rb_alloc_init(size_t num_cells, size_t max_alloc_size)
{
    ring_buf_t *d;

    if (0 == num_cells || (num_cells & (num_cells - 1)) != 0) {
        fprintf(stderr, "Number of cells must be power of 2\n");
        return NULL;
    }

#ifndef RB_INT_INDEXED
    size_t lines = num_cells / 8;  /* same memory footprint as 8B cells */

    if (0 == lines)
        lines = 1;  /* tiny rings still get one full line */

    d = rb_init_common(lines, sizeof(rb_line_t), max_alloc_size);
    if (NULL == d)
        return NULL;

    d->capacity = lines * RB_LINE_MSGS;
    d->mask = lines - 1;
#else
    d = rb_init_common(num_cells, sizeof(int64_t), max_alloc_size);
    if (NULL == d)
        return NULL;

    d->capacity = num_cells;
    d->mask = num_cells - 1;
#endif
    return d;
}

ring_buf_t *rb_alloc_init_ptr(size_t num_cells, size_t max_alloc_size)
{
    ring_buf_t *d;

    if (0 == num_cells || (num_cells & (num_cells - 1)) != 0) {
        fprintf(stderr, "Number of cells must be power of 2\n");
        return NULL;
    }

    d = rb_init_common(num_cells, sizeof(cell_t), max_alloc_size);
    if (NULL == d)
        return NULL;

    d->capacity = num_cells;
    d->mask = num_cells - 1;
    return d;
}

void rb_destroy(ring_buf_t *d)
{
    free(d);
}

/*
 * Indexed protocol (ptr ring always; int64 ring in the default build).
 *
 * Free-running indices: tail-head = occupancy, all `capacity` slots usable.
 * The full/empty test runs against a PRIVATE shadow of the peer index, so
 * the fast path touches no shared line.  The peer index is monotonic, so a
 * stale shadow can only overestimate occupancy (spurious full/empty) - it is
 * refreshed once per episode, never per message.  Safety of the plain cell
 * access: the acquire on the shadow refresh pairs with the peer's release,
 * ordering the peer's cell access before ours on slot recycling.
 */

int rb_push_ptr(ring_buf_t *d, void *data, size_t size)
{
    /* size is stored in an int32_t cell field: larger values would be
     * silently corrupted (truncate + sign-extend on pull) */
    if (__builtin_expect(!d || size > INT32_MAX, 0))
        return RB_PARAM_ERROR;

    uint64_t tail = atomic_load_explicit(&d->tail, memory_order_relaxed);

    if (__builtin_expect(tail - d->head_shadow == d->capacity, 0)) {
        d->head_shadow = atomic_load_explicit(&d->head, memory_order_acquire);
        if (tail - d->head_shadow == d->capacity)
            return RB_FULL;
    }

    cell_t *c = &PTR_CELLS(d)[tail & d->mask];

    c->data = data;   /* plain stores: the slot is producer-owned */
    c->size = (int32_t)size;

    /* Publication: orders the cell stores before the new tail value */
    atomic_store_explicit(&d->tail, tail + 1, memory_order_release);
    return RB_OK;
}

int rb_pull_ptr(ring_buf_t *d, void **data, size_t *size)
{
    if (__builtin_expect(!d || !data || !size || *data || *size, 0))
        return RB_PARAM_ERROR;

    uint64_t head = atomic_load_explicit(&d->head, memory_order_relaxed);

    if (__builtin_expect(head == d->tail_shadow, 0)) {
        d->tail_shadow = atomic_load_explicit(&d->tail, memory_order_acquire);
        if (head == d->tail_shadow)
            return RB_EMPTY;
    }

    cell_t *c = &PTR_CELLS(d)[head & d->mask];

    *data = c->data;  /* plain loads: ordered by the acquire above */
    *size = (size_t)c->size;

    /* Hand the slot back: orders our reads before the producer's reuse */
    atomic_store_explicit(&d->head, head + 1, memory_order_release);
    return RB_OK;
}

#ifdef RB_INT_INDEXED

__attribute__((hot))
int rb_push_int(ring_buf_t *d, int64_t idata)
{
    if (__builtin_expect(!d, 0))
        return RB_PARAM_ERROR;

    /* Own single-writer index: relaxed suffices, no inter-thread edge here */
    uint64_t tail = atomic_load_explicit(&d->tail, memory_order_relaxed);

    if (__builtin_expect(tail - d->head_shadow == d->capacity, 0)) {
        /* One cross-core read per fill episode.  Acquire pairs with the
         * consumer's release of head: its reads of the recycled cell
         * happen-before our overwrite. */
        d->head_shadow = atomic_load_explicit(&d->head, memory_order_acquire);
        if (tail - d->head_shadow == d->capacity)
            return RB_FULL;
    }

    INT_CELLS(d)[tail & d->mask] = idata;  /* plain store: slot is ours */

    atomic_store_explicit(&d->tail, tail + 1, memory_order_release);
    return RB_OK;
}

__attribute__((hot))
int rb_pull_int(ring_buf_t *d, int64_t *idata)
{
    if (__builtin_expect(!d || !idata, 0))
        return RB_PARAM_ERROR;

    uint64_t head = atomic_load_explicit(&d->head, memory_order_relaxed);

    if (__builtin_expect(head == d->tail_shadow, 0)) {
        /* Acquire pairs with the producer's release of tail: the cell read
         * below sees the fully written payload.  Empty-spin retries hit the
         * local cache until the producer's store invalidates the line. */
        d->tail_shadow = atomic_load_explicit(&d->tail, memory_order_acquire);
        if (head == d->tail_shadow)
            return RB_EMPTY;
    }

    *idata = INT_CELLS(d)[head & d->mask];  /* plain load */

    atomic_store_explicit(&d->head, head + 1, memory_order_release);
    return RB_OK;
}

#else  /* default: flag rides in the data line - single-hop latency */

__attribute__((hot))
int rb_push_int(ring_buf_t *d, int64_t idata)
{
    if (__builtin_expect(!d, 0))
        return RB_PARAM_ERROR;

    uint64_t off = d->p.off;              /* producer-private, plain */
    rb_line_t *ln = &LINES(d)[d->p.line];

    /* Full check only on line entry (1 of 7 ops), probing the line we must
     * own anyway to write.  Acquire pairs with the consumer's release-clear:
     * its old-lap reads happen-before our overwrite. */
    if (0 == off &&
        atomic_load_explicit(&ln->seq, memory_order_acquire) != 0)
        return RB_FULL;

    ln->msg[off] = idata;                 /* plain store */

    /* Per-message publication; flag and payload share the cache line */
    atomic_store_explicit(&ln->seq, off + 1, memory_order_release);

    if (++off == RB_LINE_MSGS) {
        off = 0;
        d->p.line = (d->p.line + 1) & d->mask;
    }
    d->p.off = off;
    return RB_OK;
}

__attribute__((hot))
int rb_pull_int(ring_buf_t *d, int64_t *idata)
{
    if (__builtin_expect(!d || !idata, 0))
        return RB_PARAM_ERROR;

    uint64_t off = d->c.off;              /* consumer-private, plain */
    rb_line_t *ln = &LINES(d)[d->c.line];

    /* Acquire pairs with the producer's release publish.  The transfer that
     * delivers seq delivers msg[] too: the empty-spin polls the very line
     * the data arrives in - the single-hop latency floor. */
    if (atomic_load_explicit(&ln->seq, memory_order_acquire) <= off)
        return RB_EMPTY;

    *idata = ln->msg[off];                /* plain load, ordered by acquire */

    if (++off == RB_LINE_MSGS) {
        /* Hand the drained line back (1 of 7 ops): release orders our reads
         * before the producer's next-lap writes */
        atomic_store_explicit(&ln->seq, 0, memory_order_release);
        off = 0;
        d->c.line = (d->c.line + 1) & d->mask;
    }
    d->c.off = off;
    return RB_OK;
}

#endif /* RB_INT_INDEXED */
