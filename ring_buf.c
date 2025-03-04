#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif

/**
 * THis file implements atomic Ring Buffer, based on LMAX Disruptor algorythm.
 */

#define _POSIX_C_SOURCE 200112L  // Enables POSIX API, including posix_memalign

#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "ring_buf.h"

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Allocate and init the Ring Buffer structure
 * @param size_t num_cells     How many records should be in the Ring Buffer
 * @param size_t max_alloc_size Maximum allowed memory to allocate
 * @return rb_t* Allocated and inited Ring Buffer structure; NULL on error
 * @details 
 */
ring_buf_t *rb_alloc_init(size_t num_cells, size_t max_alloc_size)
{
    size_t total_memory;
    ring_buf_t *d = NULL;

    if ((num_cells & (num_cells - 1)) != 0) {
        printf("Number of cells must be power of 2\n");
        return NULL;  // Capacity must be power of 2
    }

    total_memory = num_cells * sizeof(cell_t) + sizeof(ring_buf_t);

    if (total_memory > max_alloc_size) {
        return NULL;  // Prevent excessive memory usage
    }

    d = aligned_alloc(64, total_memory);
    if (NULL == d) {
        perror("Can not allocate aligned memory: ");
        return NULL;
    }

    /* Clean memory, and also push Kernel to connect physica memory to virtual */
    memset(d, 1, total_memory);
    memset(d, 0, total_memory);

    d->capacity = num_cells;
    atomic_store(&d->max_alloc_size, max_alloc_size);
    atomic_init(&d->head, 0);
    atomic_init(&d->tail, 0);

    posix_madvise(d, total_memory, POSIX_MADV_SEQUENTIAL);
    posix_madvise(d, total_memory, POSIX_MADV_WILLNEED);

    return d;
}

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Deinint and remove the Ring Buffer structure
 * @param ring_buf_t* d     
 * @details 
 */
void rb_destroy(ring_buf_t *d)
{
    free(d);
}

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Save a pointer and the buffer size in the Ring Buffer
 * @param ring_buf_t* d     Ring Buffer structure
 * @param void* data  Pointer to a buffer to save
 * @param size_t size  Size of the saved buffer
 * @return int OK (0) if saved, an error otherwise
 * @details 
 */
int rb_push(ring_buf_t *d, void *data, size_t size)
{
    if (!d) return RB_PARAM_ERROR;

    uint64_t tail = atomic_load_explicit(&d->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&d->head, memory_order_acquire);

    if (((tail + 1) & (d->capacity - 1)) == (head & (d->capacity - 1))) {
        return RB_FULL; // Buffer is full
    }

    size_t index = tail & (d->capacity - 1);
    d->cells[index].data = data;
    d->cells[index].size = size;

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&d->tail, tail + 1, memory_order_release);

    return RB_OK;
}

int rb_pull(ring_buf_t *d, void **data, size_t *size)
{
    if (!d || !data || *data || *size) return RB_PARAM_ERROR;

    uint64_t head = atomic_load_explicit(&d->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&d->tail, memory_order_acquire);

    if (head == tail) return RB_EMPTY; // Buffer is empty

    size_t index = head & (d->capacity - 1);

    *data = d->cells[index].data;
    *size = d->cells[index].size;
    

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&d->head, head + 1, memory_order_release);

    return RB_OK;
}

__attribute__((hot))
int rb_push_int(ring_buf_t *d, int64_t idata)
{
    if (!d) return RB_PARAM_ERROR;

    uint64_t tail = atomic_load_explicit(&d->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&d->head, memory_order_acquire);

    if (((tail + 1) & (d->capacity - 1)) == (head & (d->capacity - 1))) {
        return RB_FULL; // Buffer is full
    }

    size_t index = tail & (d->capacity - 1);
    d->cells[index].idata = idata;

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&d->tail, tail + 1, memory_order_release);

    return RB_OK;
}

__attribute__((hot))
int rb_pull_int(ring_buf_t *d, int64_t *idata)
{
    if (!d || !idata) return RB_PARAM_ERROR;

    uint64_t head = atomic_load_explicit(&d->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&d->tail, memory_order_acquire);

    if (head == tail) return RB_EMPTY; // Buffer is empty

    size_t index = head & (d->capacity - 1);

    *idata = d->cells[index].idata;

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&d->head, head + 1, memory_order_release);

    return RB_OK;
}

