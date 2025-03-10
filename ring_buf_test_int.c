#define _GNU_SOURCE  // Enables GNU extensions like CPU_ZERO, CPU_SET

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <locale.h>
#include <sched.h>         // For CPU affinity
#include <string.h>

#include "ring_buf.h"

/* This variable is used in helper functions fo_push() / do_pull() "*/
const int loops_waiting = 10000;
#define NUM_MESSAGES 500000000
size_t arr_size = 4096 * 2;


/* What processor should it run? Notem these values will be replaced by find_two_least_busy_cores() */
int cpu_prod = 0;
int cpu_cons = 1;

/* Used to calculate number of "hard" misses, when the sched_yield() was called */
int miss_push = 0;
int miss_pull = 0;

/* The Ring Buffer structure, shared between threads. */
ring_buf_t *ring_buf = NULL;  // Shared ring buffer buffer

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Get current time in microseconds
 * @return uint64_t Current time in microseconds
 * @details Used to calculate the test result
 */
inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1e9 + ts.tv_nsec;
}

__attribute__((hot))
/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Push integer into the Ring Buffer, try to do it multiple time
 * @param ring_buf_t* ring_buf the Ring Buffer structure poiter
 * @param int64_t idata   An Integer value to save into the Ring Buffer
 * @return int RB_FULL if could not push (the Ring Buffer is full), RB_OK if the integer saved into the Ring
 *          Buffer
 */
inline int do_push(ring_buf_t *ring_buf, int64_t idata)
{
    int rc = RB_FULL;

    for (int i = 0; i < loops_waiting; i++) {
        rc = rb_push_int(ring_buf, idata);
        if (RB_OK == rc) {
            return RB_OK;
        }
    }

    int cnt = 0;
    do {
        cnt++;
        sched_yield();
        rc = rb_push_int(ring_buf, idata);
        if (rc != RB_OK) {
            miss_push++;
        }
    } while (rc != RB_OK);

    return rc;
}


__attribute__((hot))
/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Pull integer value from the Ring Buffer
 * @param ring_buf_t* ring_buf Pointer to the Ring Buffer strcuture
 * @param int64_t* idata   Return value extracted from the Ring Buffer
 * @return int RB_EMPTY if the Ring buffer is empty, RB_OK if an integer value extracted
 */
inline int do_pull(ring_buf_t *ring_buf, int64_t *idata)
{
    int rc = RB_EMPTY;

    for (int i = 0; i < loops_waiting; i++) {
        rc = rb_pull_int(ring_buf, idata);
        if (RB_OK == rc) {
            return RB_OK;
        }
    }

    int cnt = 0;
    do {
        cnt++;
        rc = rb_pull_int(ring_buf, idata);
        if (rc != RB_OK) {
            miss_pull++;
        }
        sched_yield();
    } while (rc != RB_OK);

    return rc;
}

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Moves the caller thread to asked CPU
 * @param int num   
 * @details 
 */
void set_my_cpu(int num)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(num, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        perror("pthread_setaffinity_np () failed");
    }
}

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Set thread scheduling algorithm (if you use it, you must run with sudo)
 * @details 
 */
void set_my_prio(void)
{
    int rc;
    struct sched_param fifo_param;

    // Set priority according to function parameter
    fifo_param.sched_priority = 99;

    // Set the scheduling policy & priority for the calling thread
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &fifo_param);
    //rc = pthread_setschedparam(pthread_self(), SCHED_RR, &fifo_param);
    //rc = pthread_setschedparam(pthread_self(), SCHED_DEADLINE, &fifo_param);
    if (rc != 0) {
        perror("pthread_setschedparam");
    }
}

/* Producer Thread: Sends NUM_MESSAGES messages */
/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Producer thread: writes integers to the Ring Buffer
 * @param void* arg   Ignored
 * @return void* Ignored
 * @details 
 */
void *producer(__attribute__((unused))void *arg)
{
    set_my_cpu(cpu_prod);
    set_my_prio();
    uint64_t start_ns = get_time_ns(); // Start time


    for (int64_t i = 0; i < NUM_MESSAGES; i++) {
        do_push(ring_buf, i);
    }

    uint64_t end_ns = get_time_ns(); // End time

    double elapsed_sec = (end_ns - start_ns) / 1e9;
    double throughput = NUM_MESSAGES / elapsed_sec;
    printf("Producer finished in %.6f seconds, misses: %d\n", (end_ns - start_ns) / 1e9, miss_push);
    printf("Throughput: %'f messages/sec\n", throughput);

    return NULL;
}

/* Consumer Thread: Receives NUM_MESSAGES messages */
/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Reads integers from the Ring Buffer
 * @param void* arg   Ignored
 * @return void* Ignored
 * @details 
 */
void *consumer(__attribute__((unused))void *arg)
{
    set_my_cpu(cpu_cons);
    set_my_prio();

    uint64_t start_ns = get_time_ns(); // Start time

    for (long i = 0; i < NUM_MESSAGES; i++) {
        int64_t idata;

        do_pull(ring_buf, &idata);

        if (idata != i) {
            printf("Expected payload %ld but it is %ld\n", i, idata);
            abort();
        }
    }

    uint64_t end_ns = get_time_ns(); // End time
    double elapsed_sec = (end_ns - start_ns) / 1e9;
    double throughput = NUM_MESSAGES / elapsed_sec;

    printf("Consumer finished in %.6f seconds, missses: %d\n", elapsed_sec, miss_pull);
    printf("Throughput: %'f messages/sec\n", throughput);
    return NULL;
}

typedef struct {
    long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_stats_t;

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Helper: Computes total CPU time from stats
 * @param cpu_stats_t* s     Stat of a single CPU core
 * @return long Total time of CPU
 * @details Used to find the least busy CPU core
 */
long total_time(cpu_stats_t *s)
{
    return s->user + s->nice + s->system + s->idle +
           s->iowait + s->irq + s->softirq + s->steal;
}

/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Read stats from the /proc/stat
 * @param cpu_stats_t* stats   Pointer to stst structure, it is an output of the function
 * @param int num_cpus CPU core number to read
 * @return int 0 on success, < 0 on an error
 * @details 
 */
int get_cpu_stats(cpu_stats_t *stats, int num_cpus)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        perror("Error opening /proc/stat");
        return -1;
    }

    char line[256];
    int cpu_index = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) break; // Stop at non-CPU lines
        if (cpu_index == 0) continue; // Skip total CPU stats (first line)

        sscanf(line, "cpu%d %ld %ld %ld %ld %ld %ld %ld %ld",
               &cpu_index,
               &stats[cpu_index - 1].user, &stats[cpu_index - 1].nice,
               &stats[cpu_index - 1].system, &stats[cpu_index - 1].idle,
               &stats[cpu_index - 1].iowait, &stats[cpu_index - 1].irq,
               &stats[cpu_index - 1].softirq, &stats[cpu_index - 1].steal);

        if (cpu_index >= num_cpus) break;
    }

    fclose(fp);
    return 0;
}

/* Finds the two least busy cores */
/**
 * @author Sebastian Mountaniol (04/03/2025)
 * @brief Find two the least busy cores.
 * @details The least busy cored assigned to gloabla variables cpu_prod and cpu_cons
 */
void find_two_least_busy_cores(void)
{
    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_stats_t stats_before[num_cpus], stats_after[num_cpus];

    if (get_cpu_stats(stats_before, num_cpus) < 0) return;
    usleep(100000);  // Sleep for 100ms to measure CPU activity
    if (get_cpu_stats(stats_after, num_cpus) < 0) return;

    int min_core1 = -1, min_core2 = -1;
    double min_idle1 = -1.0, min_idle2 = -1.0;

    for (int i = 0; i < num_cpus; i++) {
        long total_before = total_time(&stats_before[i]);
        long total_after = total_time(&stats_after[i]);
        long idle_before = stats_before[i].idle;
        long idle_after = stats_after[i].idle;

        long total_delta = total_after - total_before;
        long idle_delta = idle_after - idle_before;

        if (total_delta == 0) continue;  // Prevent division by zero
        double idle_ratio = (double)idle_delta / total_delta; // Idle percentage

        // Find the two highest idle percentage cores
        if (idle_ratio > min_idle1) {
            min_idle2 = min_idle1;
            min_core2 = min_core1;
            min_idle1 = idle_ratio;
            min_core1 = i;
        } else if (idle_ratio > min_idle2) {
            min_idle2 = idle_ratio;
            min_core2 = i;
        }
    }

    // Assign to global variables
    cpu_cons = min_core1;
    cpu_prod = min_core2;
}

int main(void)
{
    printf("Array size: %ld\n", arr_size);
    
    /* Just for nice printing */    
    setlocale(LC_ALL, "");

    /* Init the Ring Buffer strcuture + array. We want "arr_size" members, but not more than 1Mb allocation */
    ring_buf = rb_alloc_init(arr_size, 1024*1024);

    /* Oops, could not allocate. Cry and die. */
    if (NULL == ring_buf) {
        fprintf(stderr, "Failed to initialize ring_buf.\n");
        return EXIT_FAILURE;
    }

    /* Read CPU core states, find two least busy to run the testing threads on them */
    find_two_least_busy_cores();

    pthread_t prod_thread, cons_thread;

    /* Start producer and consumer threads */
    pthread_create(&prod_thread, NULL, producer, NULL);
    pthread_create(&cons_thread, NULL, consumer, NULL);

    /* Wait for both threads to complete */
    pthread_join(prod_thread, NULL);
    pthread_join(cons_thread, NULL);

    /* Release the Ring Buffer */
    rb_destroy(ring_buf);
    return EXIT_SUCCESS;
}

