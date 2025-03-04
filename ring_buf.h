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

/* Enum defining ring buffer status codes */
enum {
    RB_OK = 0,          // Operation successful
    RB_FULL = -1,       // Buffer is full
    RB_EMPTY = -2,      // Buffer is empty
    RB_ERROR = -3,      // Generic error
    RB_PARAM_ERROR = -4, // Invalid parameter
    RB_MEMORY_FAIL = -5 // Memory allocation failure
};

/* Structure representing an record in the Ring Buffer */
struct ring_buf_cell_struct {
    int32_t size;  // Size of the data
    union {
        void *data;   // Pointer to the actual data
        int64_t idata; // Or integer
    };
} __attribute__((aligned(16)));

typedef struct ring_buf_cell_struct cell_t;

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Lock-free Ring Buffer.
 * @details The buffer controlling structure and the buffer itself should
 * be allocated toogether.
 * The structure is optimized to fit CPU cache line, so we do not split the control structure anf the memory
 * keeping values.
 * Also, it is more convinient to use in shared memory between processes
 */

typedef struct {
    uint64_t capacity;       // Buffer capacity (must be power of 2)
    uint64_t max_alloc_size; // Max allowed allocation size
    uint64_t head;           // Consumer read index
    uint64_t tail;           // Producer write index
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) // 64-bit
    // No padding needed; all fields naturally aligned
#else
    uint32_t _pad;           // Ensure proper alignment for `cells[]` on 32-bit
#endif
    cell_t cells[];          // Ring buffer data
} ring_buf_t;

/* Function prototypes */
ring_buf_t *rb_alloc_init(size_t num_cells, size_t max_alloc_size);
void rb_destroy(ring_buf_t *d);

int rb_push_ptr(ring_buf_t *d, void *data, size_t size);
int rb_pull_ptr(ring_buf_t *d, void **data, size_t *size);
int rb_push_int(ring_buf_t *d, int64_t idata);
int rb_pull_int(ring_buf_t *d, int64_t *idata);
#endif // DISRUPTOR_H
