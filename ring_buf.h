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
    RB_MEMORY_FAIL = -5     /**< Memory allocation failure */
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
    uint64_t est[4];                 /**< rb_batch_size() estimator state */
    uint8_t  _pad1[RB_SEC - 6 * sizeof(uint64_t)];

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
    uint8_t  _pad2[RB_SEC - 2 * sizeof(uint64_t)];

    unsigned char data[];     /**< Cells @384; layout depends on ring mode */
} ring_buf_t;

_Static_assert(offsetof(ring_buf_t, tail) == 128, "producer section @128");
_Static_assert(offsetof(ring_buf_t, head) == 256, "consumer section @256");
_Static_assert(offsetof(ring_buf_t, data) == 384, "data section @384");
_Static_assert(sizeof(_Atomic uint64_t) == sizeof(uint64_t), "shm ABI");
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

#endif // DISRUPTOR_H
