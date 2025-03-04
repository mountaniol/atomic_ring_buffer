#ifndef DISRUPTOR_H
#define DISRUPTOR_H

#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200112L  // Enables POSIX API, including posix_memalign

#include <stdatomic.h>  // Required for atomic operations
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>      // Required for sysconf()
#include <stdint.h>      // Required for sysconf()

/**
 * @enum
 * @brief This enum define status values the Ring Buffer function return
 */
enum {
    RB_OK = 0,              /**< Operation successful */
    RB_FULL = -1,           /**< Buffer is full */
    RB_EMPTY = -2,          /**< Buffer is empty */
    RB_ERROR = -3,          /**< Generic error */
    RB_PARAM_ERROR = -4,    /**< Invalid parameter */
    RB_MEMORY_FAIL = -5     /**< Memory allocation failure */
};

/**
 * @struct
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Strcucture representing one record in the Ring Buffer
 * @details 
 */
struct ring_buf_cell_struct {
    int32_t size;  // Size of the data
    union {
        void *data;   // Pointer to the actual data
        int64_t idata; // Or integer
    };
} __attribute__((aligned(16)));

typedef struct ring_buf_cell_struct cell_t;

/**
 * @struct
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Lock-free Ring Buffer.
 * @details The Ring Buffer structure and the buffer itself should be allocated together. The structure is
 *          optimized to fit the CPU cache line, so we do not split the control structure and memory-keeping
 *          values. Also, it is more convenient to use shared memory between processes.
 */
typedef struct {
    uint64_t capacity;       /**< Buffer capacity (must be power of 2) */
    uint64_t max_alloc_size; /**< Max allowed allocation size */
    uint64_t head;           /**< Consumer read index */
    uint64_t tail;           /**< Producer write index */
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) // 64-bit
    // No padding needed; all fields naturally aligned
#else
    uint32_t _pad;           /**< Ensure proper alignment for `cells[]` on 32-bit */
#endif
    cell_t cells[];          /**< Ring buffer data */
} ring_buf_t;

/* Function prototypes */


/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Allocate and init the Ring Buffer structure
 * @param size_t num_cells     How many records should be in the Ring Buffer
 * @param size_t max_alloc_size Maximum allowed memory to allocate
 * @return rb_t* Allocated and inited Ring Buffer structure; NULL on error
 * @details 
 */
ring_buf_t *rb_alloc_init(size_t num_cells, size_t max_alloc_size);


/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief release the Ring Buffer structure
 * @param ring_buf_t* d     Pointer to the Ring Buffer to free
 */
void rb_destroy(ring_buf_t *d);

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Save a pointer and the buffer size in the Ring Buffer
 * @param ring_buf_t* d     Ring Buffer structure
 * @param void* data  Pointer to a buffer to save
 * @param size_t size  Size of the saved buffer
 * @return int RB_OK (0) if saved, RB_FULL if the Ring Buffer is full, RB_PARAM_ERROR if one of input poiters
 *         is invalid.
 * @details 
 */
int rb_push_ptr(ring_buf_t *d, void *data, size_t size);

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Pull next buffer from the Ring Buffer
 * @param ring_buf_t* d     Pointer to the Ring Buffer structure
 * @param void** data  Double pointer; the poiter to a buffer will be copyed into 
 * @param size_t* size  Size of returned buffer
 * @return int RB_OK on success, RB_PARAM_ERROR if one of input poiters is invalid; RB_EMPTY if the Ring
 *         Buffer is empty
 * @details 
 */
int rb_pull_ptr(ring_buf_t *d, void **data, size_t *size);

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Push an integer value to Ring Buffer
 * @param ring_buf_t* d     Pointer to the Ring Buffer structure
 * @param int64_t idata Integer value to save into the Ring Buffer
 * @return int RB_OK if the integer value saved, RB_PARAM_ERROR if structure poiter is invalid; RB_FULL if the
 *         ring buffer is full
 * @details 
 */
__attribute__((hot))
int rb_push_int(ring_buf_t *d, int64_t idata);

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Extract an integer value from the Ring Buffer
 * @param ring_buf_t* d     Pointer to the Ring Buffer structure
 * @param int64_t* idata Pointer to integer, the value will be copied into
 * @return int RB_OK if a value extracted; RB_PARAM_ERROR if one of pointers is invalid; RB_EMPTY is the Ring
 *         Buffer is empty
 * @details 
 */
__attribute__((hot))
int rb_pull_int(ring_buf_t *d, int64_t *idata);
#endif // DISRUPTOR_H
