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

int num_cpus = 0;
int cpu_prod = 1;
int cpu_cons = 7;

ring_buf_t *ring_buf;  // Shared ring buffer buffer
volatile int producer_done = 0; // Flag to indicate producer completion

void find_two_least_busy_cores(void);

/* Get current time in nanoseconds */
inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1e9 + ts.tv_nsec;
}

int misses_push = 0;
int misses_pull = 0;

__attribute__((hot))
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
            misses_push++;
        }
    } while (rc != RB_OK);

    return rc;
}


__attribute__((hot))
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
            misses_pull++;
        }
        sched_yield();
    } while (rc != RB_OK);

    return rc;
}

void set_my_cpu(int num)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(num, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        perror("pthread_setaffinity_np () failed");
    }
}

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
    printf("Producer finished in %.6f seconds, misses: %d\n", (end_ns - start_ns) / 1e9, misses_push);
    printf("Throughput: %'f messages/sec\n", throughput);

    producer_done = 1; // Signal consumer to finish
    return NULL;
}

/* Consumer Thread: Receives NUM_MESSAGES messages */
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

    printf("Consumer finished in %.6f seconds, missses: %d\n", elapsed_sec, misses_pull);
    printf("Throughput: %'f messages/sec\n", throughput);
    return NULL;
}

typedef struct {
    long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_stats_t;

/* Computes total CPU time from stats */
long total_time(cpu_stats_t *s)
{
    return s->user + s->nice + s->system + s->idle +
           s->iowait + s->irq + s->softirq + s->steal;
}

/* Reads CPU stats from /proc/stat */
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
void find_two_least_busy_cores(void)
{
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
    
    setlocale(LC_ALL, ""); /* use user selected locale */
    ring_buf = rb_alloc_init(arr_size, 1024*1024);
    if (NULL == ring_buf) {
        fprintf(stderr, "Failed to initialize ring_buf.\n");
        return EXIT_FAILURE;
    }

    num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    find_two_least_busy_cores();

    pthread_t prod_thread, cons_thread;

    // Start producer and consumer threads
    pthread_create(&prod_thread, NULL, producer, NULL);
    pthread_create(&cons_thread, NULL, consumer, NULL);

    // Wait for both threads to complete
    pthread_join(prod_thread, NULL);
    pthread_join(cons_thread, NULL);

    rb_destroy(ring_buf);
    return EXIT_SUCCESS;
}

