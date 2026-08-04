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

#define _DEFAULT_SOURCE          /* syscall() beside the header's POSIX */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <linux/futex.h>
#include <sys/syscall.h>
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

/* Burst: same protocol, ONE tail publication for the whole batch - the
 * per-publication cost T_idx amortizes over n messages */
int rb_push_int_burst(ring_buf_t *d, const int64_t *msgs, size_t n)
{
    if (__builtin_expect(!d || !msgs, 0))
        return RB_PARAM_ERROR;
    if (0 == n)
        return 0;
    if (n > INT32_MAX)
        n = INT32_MAX;

    uint64_t tail = atomic_load_explicit(&d->tail, memory_order_relaxed);
    uint64_t free_n = d->capacity - (tail - d->head_shadow);

    if (__builtin_expect(0 == free_n, 0)) {
        d->head_shadow = atomic_load_explicit(&d->head, memory_order_acquire);
        free_n = d->capacity - (tail - d->head_shadow);
        if (0 == free_n)
            return 0;
    }
    if (n > free_n)
        n = free_n;

    /* Copy in up to two pieces: up to the end of the array, then the rest
     * from index 0 (second memcpy is 0 bytes when nothing wraps) */
    uint64_t idx = tail & d->mask;
    size_t first = d->capacity - idx;
    if (first > n)
        first = n;
    memcpy(&INT_CELLS(d)[idx], msgs, first * sizeof(int64_t));
    memcpy(INT_CELLS(d), msgs + first, (n - first) * sizeof(int64_t));

    /* One publication for the whole batch */
    atomic_store_explicit(&d->tail, tail + n, memory_order_release);
    return (int)n;
}

int rb_pull_int_burst(ring_buf_t *d, int64_t *msgs, size_t n)
{
    if (__builtin_expect(!d || !msgs, 0))
        return RB_PARAM_ERROR;
    if (0 == n)
        return 0;
    if (n > INT32_MAX)
        n = INT32_MAX;

    uint64_t head = atomic_load_explicit(&d->head, memory_order_relaxed);
    uint64_t avail = d->tail_shadow - head;

    if (__builtin_expect(0 == avail, 0)) {
        d->tail_shadow = atomic_load_explicit(&d->tail, memory_order_acquire);
        avail = d->tail_shadow - head;
        if (0 == avail)
            return 0;
    }
    if (n > avail)
        n = avail;

    /* Copy out in up to two pieces (wrap), then free all slots at once */
    uint64_t idx = head & d->mask;
    size_t first = d->capacity - idx;
    if (first > n)
        first = n;
    memcpy(msgs, &INT_CELLS(d)[idx], first * sizeof(int64_t));
    memcpy(msgs + first, INT_CELLS(d), (n - first) * sizeof(int64_t));

    atomic_store_explicit(&d->head, head + n, memory_order_release);
    return (int)n;
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

/* Burst: same protocol, one seq publication per 64B line instead of per
 * message; a batch never waits for a line to fill */
int rb_push_int_burst(ring_buf_t *d, const int64_t *msgs, size_t n)
{
    if (__builtin_expect(!d || !msgs, 0))
        return RB_PARAM_ERROR;
    if (n > INT32_MAX)
        n = INT32_MAX;

    size_t done = 0;
    while (done < n) {
        uint64_t off = d->p.off;
        rb_line_t *ln = &LINES(d)[d->p.line];

        if (0 == off &&
            atomic_load_explicit(&ln->seq, memory_order_acquire) != 0)
            break;                          /* full: next line not drained */

        /* Fill as much of this line as the batch has, publish it once */
        size_t k = RB_LINE_MSGS - off;
        if (k > n - done)
            k = n - done;
        memcpy(&ln->msg[off], msgs + done, k * sizeof(int64_t));

        atomic_store_explicit(&ln->seq, off + k, memory_order_release);

        done += k;
        off += k;
        if (RB_LINE_MSGS == off) {          /* line filled: go to the next */
            off = 0;
            d->p.line = (d->p.line + 1) & d->mask;
        }
        d->p.off = off;
    }
    return (int)done;
}

int rb_pull_int_burst(ring_buf_t *d, int64_t *msgs, size_t n)
{
    if (__builtin_expect(!d || !msgs, 0))
        return RB_PARAM_ERROR;
    if (n > INT32_MAX)
        n = INT32_MAX;

    size_t done = 0;
    while (done < n) {
        uint64_t off = d->c.off;
        rb_line_t *ln = &LINES(d)[d->c.line];
        uint64_t seq = atomic_load_explicit(&ln->seq, memory_order_acquire);

        if (seq <= off)
            break;                          /* empty: nothing published */

        /* Take everything already published in this line */
        size_t k = seq - off;
        if (k > n - done)
            k = n - done;
        memcpy(msgs + done, &ln->msg[off], k * sizeof(int64_t));

        done += k;
        off += k;
        if (RB_LINE_MSGS == off) {          /* line drained: hand it back */
            atomic_store_explicit(&ln->seq, 0, memory_order_release);
            off = 0;
            d->c.line = (d->c.line + 1) & d->mask;
        }
        d->c.off = off;
    }
    return (int)done;
}

#endif /* RB_INT_INDEXED */

/*
 * Producer-side adaptive batch size: B = T_idx / (rho* . d^ - a), the
 * utilization-targeted law (rho* = 0.85).  d^ = EWMA (alpha = 1/8) of the
 * producer's own per-message cost, sampled once per 64 calls (~0.15 ns/msg
 * amortized).  Degenerates to B = 1 below ~15M msg/s.  Constants are
 * cycles*16 fixed point, calibrated on i7-10850H (T_idx = 60 ns, a =
 * 1.37 ns/msg at 2.7 GHz TSC); override per machine if needed.
 */
/* Per-batch overhead differs by regime: the line format amortizes inside
 * the data line (60 ns effective); the indexed format in the near-empty
 * (lockstep) regime pays serialized control+data line hops per publication
 * (~420 ns effective, measured in the 2026-08 open-loop study). */
#ifndef RB_BATCH_TIDX_C16
#ifdef RB_INT_INDEXED
#define RB_BATCH_TIDX_C16 (1130 * 16)
#else
#define RB_BATCH_TIDX_C16 (162 * 16)
#endif
#endif
#ifndef RB_BATCH_A_C16
#define RB_BATCH_A_C16 59
#endif

static inline uint64_t rb_cycles(void)
{
#if defined(__CPROVER__)
    extern uint64_t nondet_u64(void);
    return nondet_u64();                /* model checker: arbitrary clock */
#elif defined(__x86_64__)
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
#error "rb_batch_size: no cycle counter for this architecture"
#endif
}

/* Sampling pass, 1 call in 64.  noinline keeps rb_batch_size() itself tiny
 * so LTO inlines the fast path into the caller's loop without dragging
 * this cold code into its uop-cache footprint. */
static __attribute__((noinline)) uint32_t rb_batch_calc(ring_buf_t *d)
{
    uint64_t *e = d->est;

    /* Measure how fast the producer really runs */
    uint64_t t = rb_cycles();
    uint64_t prev = e[1];
    e[1] = t;
    /* No previous sample yet (prev 0), clock stepped back, or stalled
     * (coarse ARM timer): only record t, keep the last B */
    if (t <= prev || 0 == prev)
        return e[3] ? (uint32_t)e[3] : 1;

    uint64_t dc = t - prev;             /* cycles spent on 64 messages */
    /* Smooth it: new = old +- |sample - old|/8; branchy form is wrap-free
     * for ANY inputs (result always lands between old and sample) */
    if (0 == e[2])
        e[2] = dc;                      /* seed with the first real sample */
    else if (dc >= e[2])
        e[2] += (dc - e[2]) >> 3;
    else
        e[2] -= (e[2] - dc) >> 3;

    uint64_t d16 = e[2] / 4;            /* d^ = cycles*16 per one message */
    if (d16 >> 32) {                    /* clock jumped: absurdly slow; also
                                           keeps the scaling below wrap-free */
        e[3] = 1;
        return 1;
    }
    uint64_t rho_d = (d16 * 109) >> 7;  /* d^ scaled to 85% utilization */
    uint64_t b;

    if (rho_d <= RB_BATCH_A_C16) {
        /* Producer faster than the ring can ever be: use the max batch */
        b = d->capacity / 4;
    } else {
        /* The law: smallest B whose amortized cost keeps up with d^ */
        b = RB_BATCH_TIDX_C16 / (rho_d - RB_BATCH_A_C16) + 1;
        if (b > 8)
            b = (b + 7) & ~(uint64_t)7; /* round up to whole 64B lines */
        if (b > d->capacity / 4)
            b = d->capacity / 4;        /* one batch <= 1/4 of the ring */
    }
    if (0 == b)
        b = 1;
    e[3] = b;
    return (uint32_t)b;
}

uint32_t rb_batch_size(ring_buf_t *d)
{
    if (__builtin_expect(!d, 0))
        return 1;

    uint64_t *e = d->est;   /* [0] call cnt, [1] t_last, [2] ewma, [3] B */

    /* 63 of 64 calls: just return the last computed B (1 until warmed up) */
    if (__builtin_expect((++e[0] & 63) != 0, 1))
        return e[3] ? (uint32_t)e[3] : 1;

    return rb_batch_calc(d);
}

/*
 * Optional blocking, built on Linux futex.
 *
 * Each direction has a "door" - a 32-bit counter the sleeper waits on -
 * and a "sleep flag" that tells the other side someone is sleeping.
 *
 * To sleep: read the door value, raise my flag, try the operation once
 * more, and only then FUTEX_WAIT on the door.  The kernel compares the
 * door against my value atomically, so a wakeup sent in between is not
 * lost - the wait just returns immediately.
 *
 * To wake: after a successful push/pull, one cheap read of the peer's
 * flag; if set - clear it, bump the door, FUTEX_WAKE.  That flag read is
 * deliberately unfenced: in the worst (nanosecond-window) case a wakeup
 * is missed, and the sleeper still recovers because every sleep is capped
 * at RB_WAIT_RECHECK_NS and re-checks the ring afterwards.
 *
 * No FUTEX_PRIVATE_FLAG: the ring may live in shared memory.
 */
#ifndef RB_WAIT_SPIN
#define RB_WAIT_SPIN 256   /* pause-iterations before parking (~2-9 us) */
#endif
#ifndef RB_WAIT_RECHECK_NS
#define RB_WAIT_RECHECK_NS 1000000   /* park cap: heals lost wakeups */
#endif

/* One polite spin-wait step (frees the core for the HT sibling) */
static inline void rb_pause(void)
{
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

/* Raw futex syscall - glibc has no wrapper for it */
static int rb_futex(_Atomic uint32_t *w, int op, uint32_t val,
                    const struct timespec *ts)
{
#if defined(__CPROVER__)
    extern int nondet_int(void);
    (void)w; (void)op; (void)val; (void)ts;
    return nondet_int();
#else
    return (int)syscall(SYS_futex, w, op, val, ts, NULL, 0);
#endif
}

/* Monotonic wall clock in nanoseconds (for timeouts) */
static uint64_t rb_now_ns(void)
{
#if defined(__CPROVER__)
    extern uint64_t nondet_u64(void);
    return nondet_u64();                /* model checker: arbitrary clock */
#else
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("rb: clock_gettime");
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* Wake the peer if its sleep flag is up.  Costs one cheap read when
 * nobody sleeps; the xchg lets only one caller issue the syscall. */
static void rb_signal(_Atomic uint32_t *flag, _Atomic uint32_t *door)
{
    if (atomic_load_explicit(flag, memory_order_relaxed) &&
        atomic_exchange_explicit(flag, 0, memory_order_seq_cst)) {
        atomic_fetch_add_explicit(door, 1, memory_order_seq_cst);
        if (rb_futex(door, FUTEX_WAKE, (uint32_t)INT32_MAX, NULL) == -1)
            perror("rb: futex wake");
    }
}

/* One sleep on the door.  Returns RB_OK when the caller should try the
 * ring again (woken, spurious, or the internal re-check cap expired);
 * RB_TIMEOUT / RB_CLOSED end the wait for good. */
static int rb_park(ring_buf_t *d, _Atomic uint32_t *door, uint32_t g,
                   uint64_t deadline)
{
    struct timespec ts;
    uint64_t left = RB_WAIT_RECHECK_NS; /* never sleep longer than the cap */

    if (atomic_load_explicit(&d->closed, memory_order_acquire))
        return RB_CLOSED;               /* closed dominates timeout */
    if (deadline) {                     /* trim the sleep to the user limit */
        uint64_t now = rb_now_ns();
        if (now >= deadline)
            return RB_TIMEOUT;
        if (deadline - now < left)
            left = deadline - now;
    }
    ts.tv_sec = (time_t)(left / 1000000000ull);
    ts.tv_nsec = (long)(left % 1000000000ull);

    /* Sleeps only while the door still equals g (kernel checks atomically) */
    int rc = rb_futex(door, FUTEX_WAIT, g, &ts);

    if (atomic_load_explicit(&d->closed, memory_order_acquire))
        return RB_CLOSED;
    if (-1 == rc && ETIMEDOUT == errno && deadline && rb_now_ns() >= deadline)
        return RB_TIMEOUT;              /* the USER limit expired, not the cap */
    return RB_OK;
}

int rb_push_wait(ring_buf_t *d, int64_t idata, uint64_t timeout_ns)
{
    if (__builtin_expect(!d, 0))
        return RB_PARAM_ERROR;

    /* Fast path: ring has room - push and wake the consumer if it sleeps */
    int rc = rb_push_int(d, idata);
    if (__builtin_expect(RB_OK == rc, 1)) {
        rb_signal(&d->csleep, &d->cdoor);
        return RB_OK;
    }
    if (RB_FULL != rc)
        return rc;

    uint64_t deadline = timeout_ns ? rb_now_ns() + timeout_ns : 0;
    for (;;) {
        /* Full: spin a little first - much cheaper than a sleep/wake trip */
        for (int i = 0; i < RB_WAIT_SPIN; i++) {
            rb_pause();
            if (rb_push_int(d, idata) == RB_OK) {
                rb_signal(&d->csleep, &d->cdoor);
                return RB_OK;
            }
        }

        /* Going to sleep: remember the door, raise my flag, then try once
         * more - so either the consumer sees the flag, or I see its
         * progress; both at once cannot be missed */
        uint32_t g = atomic_load_explicit(&d->pdoor, memory_order_relaxed);
        atomic_store_explicit(&d->psleep, 1, memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        if (rb_push_int(d, idata) == RB_OK) {
            atomic_store_explicit(&d->psleep, 0, memory_order_relaxed);
            rb_signal(&d->csleep, &d->cdoor);
            return RB_OK;
        }

        rc = rb_park(d, &d->pdoor, g, deadline);
        atomic_store_explicit(&d->psleep, 0, memory_order_relaxed);
        if (RB_OK != rc) {
            /* Timed out or closed: one last try, then give up */
            if (rb_push_int(d, idata) == RB_OK) {
                rb_signal(&d->csleep, &d->cdoor);
                return RB_OK;
            }
            return rc;
        }
    }
}

/* Mirror of rb_push_wait: sleeps while empty, wakes a full-blocked producer */
int rb_pull_wait(ring_buf_t *d, int64_t *idata, uint64_t timeout_ns)
{
    if (__builtin_expect(!d || !idata, 0))
        return RB_PARAM_ERROR;

    /* Fast path: data ready - pull and wake the producer if it sleeps */
    int rc = rb_pull_int(d, idata);
    if (__builtin_expect(RB_OK == rc, 1)) {
        rb_signal(&d->psleep, &d->pdoor);
        return RB_OK;
    }
    if (RB_EMPTY != rc)
        return rc;

    uint64_t deadline = timeout_ns ? rb_now_ns() + timeout_ns : 0;
    for (;;) {
        /* Empty: spin a little first */
        for (int i = 0; i < RB_WAIT_SPIN; i++) {
            rb_pause();
            if (rb_pull_int(d, idata) == RB_OK) {
                rb_signal(&d->psleep, &d->pdoor);
                return RB_OK;
            }
        }

        /* Going to sleep: door value, flag up, one more try (see push) */
        uint32_t g = atomic_load_explicit(&d->cdoor, memory_order_relaxed);
        atomic_store_explicit(&d->csleep, 1, memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        if (rb_pull_int(d, idata) == RB_OK) {
            atomic_store_explicit(&d->csleep, 0, memory_order_relaxed);
            rb_signal(&d->psleep, &d->pdoor);
            return RB_OK;
        }

        rc = rb_park(d, &d->cdoor, g, deadline);
        atomic_store_explicit(&d->csleep, 0, memory_order_relaxed);
        if (RB_OK != rc) {
            /* Timed out or closed: one last try, then give up */
            if (rb_pull_int(d, idata) == RB_OK) {
                rb_signal(&d->psleep, &d->pdoor);
                return RB_OK;
            }
            return rc;
        }
    }
}

/* Shutdown: mark the ring closed and kick both doors so every sleeper
 * (and every future _wait call that would block) returns RB_CLOSED */
void rb_wake(ring_buf_t *d)
{
    if (!d)
        return;
    atomic_store_explicit(&d->closed, 1, memory_order_seq_cst);
    atomic_fetch_add_explicit(&d->cdoor, 1, memory_order_seq_cst);
    atomic_fetch_add_explicit(&d->pdoor, 1, memory_order_seq_cst);
    if (rb_futex(&d->cdoor, FUTEX_WAKE, (uint32_t)INT32_MAX, NULL) == -1)
        perror("rb: futex wake");
    if (rb_futex(&d->pdoor, FUTEX_WAKE, (uint32_t)INT32_MAX, NULL) == -1)
        perror("rb: futex wake");
}
