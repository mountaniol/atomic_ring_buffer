/**
 * @file rb_naive.c
 * @brief Naive reference implementation of the Nictus Oculi API.
 *
 * One mutex, taken by BOTH sides for every single message.  No atomics, no
 * cache-line separation, no batching intelligence - the textbook ring buffer
 * everyone writes first.  It exports exactly the API of ring_buf.h with the
 * same status codes and the same preconditions, so any program built against
 * libnictus.a can be relinked against this file instead and measured.  That
 * is its only purpose: to say what the lock-free version actually buys.
 *
 * Do not ship it.  It is the baseline, not a fallback.
 *
 * Two deliberate differences from the real thing, both documented here so
 * that a comparison is read correctly:
 *   - capacity is exactly num_cells (the default line format gives 7/8 of it,
 *     because it spends one slot per line on the publication flag);
 *   - rb_batch_size() always answers 1: there is no adaptive law to consult.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ring_buf.h"

/* The public handle is opaque to callers - nothing outside ring_buf.c ever
 * dereferences a ring_buf_t - so the naive ring keeps its own layout and is
 * handed back as a ring_buf_t *.  The object's effective type is naive_t
 * throughout, so no aliasing question arises. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  not_full;   /**< producer waits here (rb_push_wait) */
    pthread_cond_t  not_empty;  /**< consumer waits here (rb_pull_wait) */
    uint64_t capacity;
    uint64_t mask;
    uint64_t head;              /**< free-running read index  */
    uint64_t tail;              /**< free-running write index */
    int      is_ptr;            /**< which view of the cells is live */
    int      closed;            /**< rb_wake() called; one-way */
    union {                     /* one storage, one view per instance */
        int64_t icells[0];
        cell_t  cells[0];
    };
} naive_t;

/* Lost wakeups are possible whenever the peer uses the polling API, which
 * never signals; re-check at this cap instead of sleeping forever.  Same
 * remedy and same value as the futex version. */
#define NAIVE_RECHECK_NS 1000000ull

#define USED(n)  ((n)->tail - (n)->head)

static uint64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static ring_buf_t *naive_alloc(size_t num_cells, size_t max_alloc_size,
                               int is_ptr)
{
    pthread_condattr_t attr;
    naive_t *n;
    size_t need;

    if (0 == num_cells || (num_cells & (num_cells - 1)) != 0)
        return NULL;                    /* power of two, as in the real API */

    need = sizeof(naive_t)
           + num_cells * (is_ptr ? sizeof(cell_t) : sizeof(int64_t));
    if (need > max_alloc_size)
        return NULL;

    n = calloc(1, need);
    if (!n)
        return NULL;

    if (pthread_mutex_init(&n->lock, NULL) != 0) {
        free(n);
        return NULL;
    }
    /* CLOCK_MONOTONIC: a wall-clock jump must not break a timeout */
    if (pthread_condattr_init(&attr) != 0
        || pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0
        || pthread_cond_init(&n->not_full, &attr) != 0
        || pthread_cond_init(&n->not_empty, &attr) != 0) {
        pthread_mutex_destroy(&n->lock);
        free(n);
        return NULL;
    }
    if (pthread_condattr_destroy(&attr) != 0) {
        /* nothing left to undo that matters; the ring is usable */
    }

    n->capacity = num_cells;
    n->mask = num_cells - 1;
    n->is_ptr = is_ptr;
    return (ring_buf_t *)n;
}

ring_buf_t *rb_alloc_init(size_t num_cells, size_t max_alloc_size)
{
    return naive_alloc(num_cells, max_alloc_size, 0);
}

ring_buf_t *rb_alloc_init_ptr(size_t num_cells, size_t max_alloc_size)
{
    return naive_alloc(num_cells, max_alloc_size, 1);
}

void rb_destroy(ring_buf_t *d)
{
    naive_t *n = (naive_t *)d;

    if (!n)
        return;
    pthread_cond_destroy(&n->not_empty);
    pthread_cond_destroy(&n->not_full);
    pthread_mutex_destroy(&n->lock);
    free(n);
}

/* ------------------------------------------------------------- int64 ring */

int rb_push_int(ring_buf_t *d, int64_t idata)
{
    naive_t *n = (naive_t *)d;
    int rc = RB_OK;

    if (!n)
        return RB_PARAM_ERROR;
    if (pthread_mutex_lock(&n->lock) != 0)
        return RB_ERROR;

    if (USED(n) == n->capacity)
        rc = RB_FULL;
    else
        n->icells[n->tail++ & n->mask] = idata;

    if (pthread_mutex_unlock(&n->lock) != 0)
        return RB_ERROR;
    return rc;
}

int rb_pull_int(ring_buf_t *d, int64_t *idata)
{
    naive_t *n = (naive_t *)d;
    int rc = RB_OK;

    if (!n || !idata)
        return RB_PARAM_ERROR;
    if (pthread_mutex_lock(&n->lock) != 0)
        return RB_ERROR;

    if (0 == USED(n))
        rc = RB_EMPTY;
    else
        *idata = n->icells[n->head++ & n->mask];

    if (pthread_mutex_unlock(&n->lock) != 0)
        return RB_ERROR;
    return rc;
}

/* Bursts take the lock ONCE for the whole batch - the only amortization a
 * mutex design has to offer, and the fair way to compare against bursts. */
int rb_push_int_burst(ring_buf_t *d, const int64_t *msgs, size_t num)
{
    naive_t *n = (naive_t *)d;
    size_t k, room;

    if (!n || !msgs)
        return RB_PARAM_ERROR;
    if (0 == num)
        return 0;
    if (pthread_mutex_lock(&n->lock) != 0)
        return RB_ERROR;

    room = n->capacity - USED(n);
    if (room > num)
        room = num;
    for (k = 0; k < room; k++)
        n->icells[n->tail++ & n->mask] = msgs[k];

    if (pthread_mutex_unlock(&n->lock) != 0)
        return RB_ERROR;
    return (int)room;
}

int rb_pull_int_burst(ring_buf_t *d, int64_t *msgs, size_t num)
{
    naive_t *n = (naive_t *)d;
    size_t k, have;

    if (!n || !msgs)
        return RB_PARAM_ERROR;
    if (0 == num)
        return 0;
    if (pthread_mutex_lock(&n->lock) != 0)
        return RB_ERROR;

    have = USED(n);
    if (have > num)
        have = num;
    for (k = 0; k < have; k++)
        msgs[k] = n->icells[n->head++ & n->mask];

    if (pthread_mutex_unlock(&n->lock) != 0)
        return RB_ERROR;
    return (int)have;
}

/* No estimator here: the naive ring has no idea how fast it is being called */
uint32_t rb_batch_size(ring_buf_t *d)
{
    (void)d;
    return 1;
}

/* --------------------------------------------------------- ptr+size ring */

int rb_push_ptr(ring_buf_t *d, void *data, size_t size)
{
    naive_t *n = (naive_t *)d;
    int rc = RB_OK;

    if (!n || size > INT32_MAX)
        return RB_PARAM_ERROR;
    if (pthread_mutex_lock(&n->lock) != 0)
        return RB_ERROR;

    if (USED(n) == n->capacity) {
        rc = RB_FULL;
    } else {
        cell_t *c = &n->cells[n->tail++ & n->mask];
        c->data = data;
        c->size = (int32_t)size;
    }

    if (pthread_mutex_unlock(&n->lock) != 0)
        return RB_ERROR;
    return rc;
}

int rb_pull_ptr(ring_buf_t *d, void **data, size_t *size)
{
    naive_t *n = (naive_t *)d;
    int rc = RB_OK;

    /* same precondition as the real one: the caller's variables must be
     * empty, which catches pulling into a slot that still owns a buffer */
    if (!n || !data || !size || *data || *size)
        return RB_PARAM_ERROR;
    if (pthread_mutex_lock(&n->lock) != 0)
        return RB_ERROR;

    if (0 == USED(n)) {
        rc = RB_EMPTY;
    } else {
        cell_t *c = &n->cells[n->head++ & n->mask];
        *data = c->data;
        *size = (size_t)c->size;
    }

    if (pthread_mutex_unlock(&n->lock) != 0)
        return RB_ERROR;
    return rc;
}

/* ------------------------------------------------------------ blocking API */

/* Sleep on `cv` until the deadline, the recheck cap, or a signal.  Called
 * with the lock held; returns with it held. */
static int naive_park(naive_t *n, pthread_cond_t *cv, uint64_t deadline)
{
    struct timespec ts;
    uint64_t wake, now = now_ns();

    if (n->closed)
        return RB_CLOSED;                /* closed dominates timeout */
    if (deadline && now >= deadline)
        return RB_TIMEOUT;

    wake = now + NAIVE_RECHECK_NS;
    if (deadline && deadline < wake)
        wake = deadline;
    ts.tv_sec = (time_t)(wake / 1000000000ull);
    ts.tv_nsec = (long)(wake % 1000000000ull);

    if (pthread_cond_timedwait(cv, &n->lock, &ts) == EINVAL)
        return RB_ERROR;
    if (n->closed)
        return RB_CLOSED;
    if (deadline && now_ns() >= deadline)
        return RB_TIMEOUT;               /* the USER limit, not the cap */
    return RB_OK;
}

int rb_push_wait(ring_buf_t *d, int64_t idata, uint64_t timeout_ns)
{
    naive_t *n = (naive_t *)d;
    uint64_t deadline;
    int rc;

    if (!n)
        return RB_PARAM_ERROR;
    deadline = timeout_ns ? now_ns() + timeout_ns : 0;

    if (pthread_mutex_lock(&n->lock) != 0)
        return RB_ERROR;
    for (;;) {
        if (n->closed) {
            rc = RB_CLOSED;
            break;
        }
        if (USED(n) < n->capacity) {
            n->icells[n->tail++ & n->mask] = idata;
            pthread_cond_signal(&n->not_empty);
            rc = RB_OK;
            break;
        }
        rc = naive_park(n, &n->not_full, deadline);
        if (rc != RB_OK)
            break;
    }
    if (pthread_mutex_unlock(&n->lock) != 0)
        return RB_ERROR;
    return rc;
}

int rb_pull_wait(ring_buf_t *d, int64_t *idata, uint64_t timeout_ns)
{
    naive_t *n = (naive_t *)d;
    uint64_t deadline;
    int rc;

    if (!n || !idata)
        return RB_PARAM_ERROR;
    deadline = timeout_ns ? now_ns() + timeout_ns : 0;

    if (pthread_mutex_lock(&n->lock) != 0)
        return RB_ERROR;
    for (;;) {
        if (USED(n) > 0) {               /* drain before honouring a close */
            *idata = n->icells[n->head++ & n->mask];
            pthread_cond_signal(&n->not_full);
            rc = RB_OK;
            break;
        }
        if (n->closed) {
            rc = RB_CLOSED;
            break;
        }
        rc = naive_park(n, &n->not_empty, deadline);
        if (rc != RB_OK)
            break;
    }
    if (pthread_mutex_unlock(&n->lock) != 0)
        return RB_ERROR;
    return rc;
}

void rb_wake(ring_buf_t *d)
{
    naive_t *n = (naive_t *)d;

    if (!n)
        return;
    if (pthread_mutex_lock(&n->lock) != 0)
        return;
    n->closed = 1;
    pthread_cond_broadcast(&n->not_empty);
    pthread_cond_broadcast(&n->not_full);
    if (pthread_mutex_unlock(&n->lock) != 0)
        return;
}
