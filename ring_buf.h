#ifndef DISRUPTOR_H
#define DISRUPTOR_H

#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200112L  /* posix_memalign, posix_madvise */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

/** Status codes returned by the Ring Buffer functions */
enum {
    RB_OK = 0,              /**< Operation successful */
    RB_FULL = -1,           /**< Buffer is full */
    RB_EMPTY = -2,          /**< Buffer is empty */
    RB_ERROR = -3,          /**< Generic error */
    RB_PARAM_ERROR = -4,    /**< Invalid parameter */
    RB_MEMORY_FAIL = -5,    /**< Memory allocation failure */
    RB_TIMEOUT = -6,        /**< rb_*_wait: timeout expired */
    RB_CLOSED = -7          /**< rb_*_wait: ring was closed by rb_wake() */
};

/** One record of a ptr+size ring: 16 bytes, 4 per cache line */
struct ring_buf_cell_struct {
    int32_t size;           /**< Size of the data */
    union {
        void *data;         /**< Pointer to the actual data */
        int64_t idata;      /**< Or integer payload */
    };
} __attribute__((aligned(16)));

typedef struct ring_buf_cell_struct cell_t;

/** One 64B line of the default int64 ring: seq doubles as the publication
 *  flag (0 = free, 1..7 = published count).  INTERNAL layout - exposed here
 *  only as a typed view of the data section; never touch it directly. */
#define RB_LINE_MSGS 7  /* int64 messages per 64B line; 8th slot is the flag */

typedef struct {
    _Atomic uint64_t seq;         /**< 0 = line free; 1..7 = valid messages */
    int64_t msg[RB_LINE_MSGS];    /**< Payload slots */
} rb_line_t;

/*
 * Lock-free SPSC (single producer / single consumer) Ring Buffer.
 *
 * Control state is split into 128-byte sections (two cache lines: Intel's
 * adjacent-line prefetcher pairs 64B lines, so the effective false-sharing
 * unit is 128B).  Producer and consumer never write into the same line.
 *
 * One buffer instance serves ONE mode, chosen at creation:
 *   rb_alloc_init()     - int64 ring, used with rb_push_int()/rb_pull_int()
 *   rb_alloc_init_ptr() - ptr+size ring, used with rb_push_ptr()/rb_pull_ptr()
 *
 * The whole object is a single contiguous block with no internal pointers,
 * so it can be placed in shared memory and mapped at different addresses;
 * place it at a 128-byte-aligned address (mmap'd pages qualify).  Note that
 * a void* payload transported by the ptr ring is only meaningful within one
 * address space.
 */
#define RB_SEC 128

typedef struct {
    /* section 0 @0: read-only after init; stays Shared in both cores' caches */
    uint64_t capacity;        /**< Usable slots */
    uint64_t mask;            /**< Cell (or line) index mask */
    uint64_t max_alloc_size;  /**< Max allowed allocation size */
    uint8_t  _pad0[RB_SEC - 3 * sizeof(uint64_t)];

    /* section 1 @128: written by the producer ONLY */
    union {
        struct {
            _Atomic uint64_t tail;   /**< Free-running write index */
            uint64_t head_shadow;    /**< Producer-private copy of head */
        };
        struct {                     /* default (line-format) int64 ring */
            uint64_t line;           /**< Current write line */
            uint64_t off;            /**< Offset in line, 0..6 */
        } p;
    };
    /* est[] lives in the SECOND 64B line of the producer section: it is
     * written on every rb_batch_size() call, and the first line (tail) is
     * acquire-polled by the consumer - sharing it would ping-pong per call */
    uint8_t  _pad1a[64 - 2 * sizeof(uint64_t)];
    uint64_t est[4];                 /**< rb_batch_size() estimator state */
    uint8_t  _pad1b[64 - 4 * sizeof(uint64_t)];

    /* section 2 @256: written by the consumer ONLY */
    union {
        struct {
            _Atomic uint64_t head;   /**< Free-running read index */
            uint64_t tail_shadow;    /**< Consumer-private copy of tail */
        };
        struct {
            uint64_t line;           /**< Current read line */
            uint64_t off;            /**< Offset in line, 0..6 */
        } c;
    };
    uint8_t  _pad2a[64 - 2 * sizeof(uint64_t)];

    /* Parking state (rb_*_wait/rb_wake) on its own quiet line: touched only
     * around sleep/wake events, by BOTH sides (RMW protocol) - deliberate
     * exception to the section ownership rule, never on a hot-path line.
     * Doors are futex words: 32-bit, 4-byte aligned. */
    _Atomic uint32_t csleep;         /**< consumer parked flag */
    _Atomic uint32_t cdoor;          /**< consumer wake generation (futex) */
    _Atomic uint32_t psleep;         /**< producer parked flag */
    _Atomic uint32_t pdoor;          /**< producer wake generation (futex) */
    _Atomic uint32_t closed;         /**< rb_wake() called; one-way */
    uint8_t  _pad2b[64 - 5 * sizeof(uint32_t)];

    /* Cells @384.  Typed views of one storage, chosen by ring mode at
     * creation; every byte is only ever accessed through the one view its
     * instance was created with (union punning, defined in GNU C - no
     * casts, no strict-aliasing questions).  [0] is the GNU zero-length
     * array, the union-compatible spelling of a flexible array member. */
    union {
        int64_t   icells[0];      /**< int64 ring, -DRB_INT_INDEXED build */
        rb_line_t lines[0];       /**< int64 ring, default (line format) */
        cell_t    cells[0];       /**< ptr+size ring */
        unsigned char data[0];    /**< byte view (offset anchor, shm) */
    };
} ring_buf_t;

_Static_assert(offsetof(ring_buf_t, tail) == 128, "producer section @128");
_Static_assert(offsetof(ring_buf_t, est) == 192, "estimator on own line");
_Static_assert(offsetof(ring_buf_t, head) == 256, "consumer section @256");
_Static_assert(offsetof(ring_buf_t, csleep) == 320, "parking on own line");
_Static_assert(offsetof(ring_buf_t, data) == 384, "data section @384");
_Static_assert(sizeof(rb_line_t) == 64, "one cache line per rb_line_t");
_Static_assert(sizeof(_Atomic uint64_t) == sizeof(uint64_t), "shm ABI");
_Static_assert(sizeof(_Atomic uint32_t) == 4, "futex word ABI");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
               "shm needs address-free lock-free 64-bit atomics");

/* Function prototypes */

/**
 * @brief Allocate and init an int64 Ring Buffer (for rb_push_int/rb_pull_int)
 * @param size_t num_cells      Requested capacity; must be a power of 2.
 *                              Default build: usable capacity is 7/8 of it,
 *                              minimum one 7-message line (with
 *                              -DRB_INT_INDEXED: exactly num_cells).
 * @param size_t max_alloc_size Maximum allowed memory to allocate
 * @return ring_buf_t* Ring Buffer, or NULL on error
 */
ring_buf_t *rb_alloc_init(size_t num_cells, size_t max_alloc_size);

/**
 * @brief Allocate and init a ptr+size Ring Buffer (for rb_push_ptr/rb_pull_ptr)
 * @param size_t num_cells      How many records; must be a power of 2
 * @param size_t max_alloc_size Maximum allowed memory to allocate
 * @return ring_buf_t* Ring Buffer, or NULL on error
 */
ring_buf_t *rb_alloc_init_ptr(size_t num_cells, size_t max_alloc_size);

/**
 * @brief Release the Ring Buffer
 * @param ring_buf_t* d Ring Buffer to free
 */
void rb_destroy(ring_buf_t *d);

/**
 * @brief Save a pointer and the buffer size in the Ring Buffer
 * @param ring_buf_t* d    Ring Buffer (created with rb_alloc_init_ptr)
 * @param void* data       Pointer to save
 * @param size_t size      Size of the saved buffer; must be <= INT32_MAX
 * @return int RB_OK, RB_FULL if full, RB_PARAM_ERROR on invalid input
 */
int rb_push_ptr(ring_buf_t *d, void *data, size_t size);

/**
 * @brief Pull the next buffer from the Ring Buffer
 * @param ring_buf_t* d    Ring Buffer (created with rb_alloc_init_ptr)
 * @param void** data      Out: saved pointer; *data must be NULL on entry
 * @param size_t* size     Out: saved size; *size must be 0 on entry
 * @return int RB_OK, RB_EMPTY if empty, RB_PARAM_ERROR on invalid input
 */
int rb_pull_ptr(ring_buf_t *d, void **data, size_t *size);

/**
 * @brief Push an integer value into the Ring Buffer
 * @param ring_buf_t* d    Ring Buffer (created with rb_alloc_init)
 * @param int64_t idata    Value to save
 * @return int RB_OK, RB_FULL if full, RB_PARAM_ERROR on invalid input
 */
__attribute__((hot))
int rb_push_int(ring_buf_t *d, int64_t idata);

/**
 * @brief Extract an integer value from the Ring Buffer
 * @param ring_buf_t* d    Ring Buffer (created with rb_alloc_init)
 * @param int64_t* idata   Out: extracted value
 * @return int RB_OK, RB_EMPTY if empty, RB_PARAM_ERROR on invalid input
 */
__attribute__((hot))
int rb_pull_int(ring_buf_t *d, int64_t *idata);

/**
 * @brief Push up to n values as one batch; never waits, never defers
 * Default build publishes once per 64B line, -DRB_INT_INDEXED once per call.
 * @param ring_buf_t* d      Ring Buffer (created with rb_alloc_init)
 * @param const int64_t* msgs Values to push
 * @param size_t n           How many
 * @return int Number pushed (0 if full), RB_PARAM_ERROR on invalid input
 */
int rb_push_int_burst(ring_buf_t *d, const int64_t *msgs, size_t n);

/**
 * @brief Pull up to n already-published values; never waits
 * @param ring_buf_t* d    Ring Buffer (created with rb_alloc_init)
 * @param int64_t* msgs    Out: pulled values
 * @param size_t n         Capacity of msgs
 * @return int Number pulled (0 if empty), RB_PARAM_ERROR on invalid input
 */
int rb_pull_int_burst(ring_buf_t *d, int64_t *msgs, size_t n);

/**
 * @brief Producer-side adaptive batch size from the producer's own rate
 * Call once per message from the producer thread; collect that many messages
 * before rb_push_int_burst().  Degenerates to 1 at low rates (publish
 * immediately); never accumulate beyond what the source already has.
 * @param ring_buf_t* d    Ring Buffer (created with rb_alloc_init)
 * @return uint32_t Recommended batch size, >= 1
 */
uint32_t rb_batch_size(ring_buf_t *d);

/*
 * Optional blocking (Linux futex).  Contract: a side may sleep ONLY if the
 * opposite side also uses the _wait functions - they carry the wakeups; the
 * non-blocking API stays wakeup-free and untouched.  Spins ~RB_WAIT_SPIN
 * pause-iterations before parking (wake-from-sleep costs ~3-40 us depending
 * on C-states; see study notes).
 */

/**
 * @brief Push one value; sleep while the ring is full
 * @param ring_buf_t* d      Ring Buffer (created with rb_alloc_init)
 * @param int64_t idata      Value to save
 * @param uint64_t timeout_ns Max wait; 0 = wait forever
 * @return int RB_OK, RB_TIMEOUT, RB_CLOSED, RB_PARAM_ERROR
 */
int rb_push_wait(ring_buf_t *d, int64_t idata, uint64_t timeout_ns);

/**
 * @brief Pull one value; sleep while the ring is empty
 * @param ring_buf_t* d      Ring Buffer (created with rb_alloc_init)
 * @param int64_t* idata     Out: extracted value
 * @param uint64_t timeout_ns Max wait; 0 = wait forever
 * @return int RB_OK, RB_TIMEOUT, RB_CLOSED, RB_PARAM_ERROR
 */
int rb_pull_wait(ring_buf_t *d, int64_t *idata, uint64_t timeout_ns);

/**
 * @brief Close the ring for waiters: both sleeping sides wake and return
 *        RB_CLOSED, as do all future _wait calls that would block.
 *        Data already in the ring stays readable.  One-way.
 * @param ring_buf_t* d      Ring Buffer
 */
void rb_wake(ring_buf_t *d);

#endif // DISRUPTOR_H
