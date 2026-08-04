#define _GNU_SOURCE  // Enables GNU extensions like CPU_ZERO, CPU_SET

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <locale.h>
#include <sched.h>         // For CPU affinity
#include <string.h>
#include <math.h>          // sqrt() for latency stddev

#include "ring_buf.h"

/* This variable is used in helper functions fo_push() / do_pull() "*/
const int loops_waiting = 10000;
long num_messages = 500 * 1000000L;  /* millions; overridden by --samples */
int use_wait = 0;                    /* --wait: rb_push_wait/rb_pull_wait */
size_t arr_size = 4096 * 2;

/* Latency sampling: every LAT_SAMPLE-th message is timestamped by the
 * producer into lat_sent_ns[] BEFORE the push; the consumer reads the slot
 * AFTER pulling that message, so the ring's own release/acquire publication
 * orders the accesses - no extra synchronization needed.  Slot count covers
 * far more in-flight samples than the ring can hold (8192/1024 = 8). */
#define LAT_SAMPLE 1024
#define LAT_SLOTS  64
uint64_t lat_sent_ns[LAT_SLOTS];


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

    /* Local copies: keep the hot loop free of global reloads (perf: the
     * ring_buf global was re-loaded + NULL-tested on every message) */
    const long n = num_messages;
    const int w = use_wait;
    ring_buf_t *const rb = ring_buf;
    uint64_t start_ns = get_time_ns(); // Start time

    /* Separate loops: the busy loop must stay as tiny as the original -
     * on HT siblings both threads share the uop cache and L1I */
    if (w) {
        for (int64_t i = 0; i < n; i++) {
            if (0 == (i & (LAT_SAMPLE - 1)))
                lat_sent_ns[(i / LAT_SAMPLE) & (LAT_SLOTS - 1)] = get_time_ns();
            rb_push_wait(rb, i, 0);
        }
    } else {
        for (int64_t i = 0; i < n; i++) {
            if (0 == (i & (LAT_SAMPLE - 1)))
                lat_sent_ns[(i / LAT_SAMPLE) & (LAT_SLOTS - 1)] = get_time_ns();
            do_push(rb, i);
        }
    }

    uint64_t end_ns = get_time_ns(); // End time

    double elapsed_sec = (end_ns - start_ns) / 1e9;
    double throughput = num_messages / elapsed_sec;
    printf("Producer finished in %.6f seconds, misses: %d\n", (end_ns - start_ns) / 1e9, miss_push);
    printf("Throughput: %'f messages/sec\n", throughput);
    printf("Per message: %.3f ns\n", (double)(end_ns - start_ns) / num_messages);

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

    double lat_sum = 0, lat_sum2 = 0, lat_min = 1e18, lat_max = 0;
    long lat_n = 0;

    /* Local copies: keep the hot loop free of global reloads (perf: the
     * ring_buf global was re-loaded + NULL-tested on every message) */
    const long n = num_messages;
    const int w = use_wait;
    ring_buf_t *const rb = ring_buf;
    uint64_t start_ns = get_time_ns(); // Start time

    /* Separate loops: keep the busy loop as tiny as the original */
    if (w) {
        for (long i = 0; i < n; i++) {
            int64_t idata = -1;

            rb_pull_wait(rb, &idata, 0);

            if (idata != i) {
                printf("Expected payload %ld but it is %ld\n", i, idata);
                abort();
            }

            if (0 == (i & (LAT_SAMPLE - 1))) {
                double d = (double)(get_time_ns() -
                                    lat_sent_ns[(i / LAT_SAMPLE) & (LAT_SLOTS - 1)]);
                lat_n++;
                lat_sum += d;
                lat_sum2 += d * d;
                if (d < lat_min) lat_min = d;
                if (d > lat_max) lat_max = d;
            }
        }
    } else {
        for (long i = 0; i < n; i++) {
            int64_t idata = -1;

            do_pull(rb, &idata);

            if (idata != i) {
                printf("Expected payload %ld but it is %ld\n", i, idata);
                abort();
            }

            if (0 == (i & (LAT_SAMPLE - 1))) {
                double d = (double)(get_time_ns() -
                                    lat_sent_ns[(i / LAT_SAMPLE) & (LAT_SLOTS - 1)]);
                lat_n++;
                lat_sum += d;
                lat_sum2 += d * d;
                if (d < lat_min) lat_min = d;
                if (d > lat_max) lat_max = d;
            }
        }
    }

    uint64_t end_ns = get_time_ns(); // End time
    double elapsed_sec = (end_ns - start_ns) / 1e9;
    double throughput = num_messages / elapsed_sec;

    printf("Consumer finished in %.6f seconds, missses: %d\n", elapsed_sec, miss_pull);
    printf("Throughput: %'f messages/sec\n", throughput);
    printf("Per message: %.3f ns\n", (double)(end_ns - start_ns) / num_messages);

    double lat_mean = lat_sum / lat_n;
    printf("Latency (every %dth msg, %ld samples): min %.0f ns, mean %.0f ns, "
           "max %.0f ns, stddev %.0f ns\n", LAT_SAMPLE, lat_n, lat_min,
           lat_mean, lat_max, sqrt(lat_sum2 / lat_n - lat_mean * lat_mean));
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
    int cpu_index;
    int filled = 0;

    /* Unfilled rows (offline cores) stay zero: the caller skips them via
     * its total_delta == 0 check */
    memset(stats, 0, sizeof(*stats) * (size_t)num_cpus);

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) break; // Stop at non-CPU lines
        if (line[3] < '0' || line[3] > '9') continue; // Skip aggregate "cpu " line

        /* Parse into a local row first: sscanf computes its argument
         * addresses before assigning cpu_index */
        cpu_stats_t s;
        if (sscanf(line, "cpu%d %ld %ld %ld %ld %ld %ld %ld %ld",
                   &cpu_index, &s.user, &s.nice, &s.system, &s.idle,
                   &s.iowait, &s.irq, &s.softirq, &s.steal) != 9)
            continue; // Malformed line
        if (cpu_index < 0 || cpu_index >= num_cpus)
            continue; // Core id outside the caller's array

        stats[cpu_index] = s; // "cpuN" ids are 0-based, as is stats[]
        filled++;
    }

    fclose(fp);
    return filled > 0 ? 0 : -1;
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

/**
 * @brief Returns the HT sibling of a CPU, or -1 if it has none
 * @param int cpu   Logical CPU number
 * @return int Sibling logical CPU, or -1
 */
int sibling_of(int cpu)
{
    char path[128];
    int a = -1, b = -1;
    FILE *fp;

    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
    fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        return -1;
    }
    if (fscanf(fp, "%d%*[-,]%d", &a, &b) < 2)  /* "2,8" or "2-3" or "2" */
        b = -1;
    fclose(fp);
    return (a == cpu) ? b : a;
}

void usage(const char *prog)
{
    printf("Usage: %s <placement> [options]\n"
           "  --cores      producer and consumer on two distinct physical cores\n"
           "  --siblings   producer and consumer on HT siblings of one physical core\n"
           "  --same-core  producer and consumer on one logical CPU\n"
           "  --wait       use blocking rb_push_wait/rb_pull_wait (futex)\n"
           "  --samples N[mM]  messages to run, in millions (default 500m)\n"
           "  --help       this text\n", prog);
}

int main(int argc, char **argv)
{
    const char *placement = NULL;

    for (int i = 1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (0 == strcmp(argv[i], "--wait")) {
            use_wait = 1;
        } else if (0 == strcmp(argv[i], "--samples")) {
            char *end;
            long v;
            if (++i >= argc) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            v = strtol(argv[i], &end, 10);
            if (v <= 0 ||
                (*end != '\0' && strcmp(end, "m") && strcmp(end, "M"))) {
                fprintf(stderr, "Bad --samples value: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
            num_messages = v * 1000000L;
        } else if (NULL == placement) {
            placement = argv[i];
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (NULL == placement) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* Read CPU core states; cpu_cons becomes the most idle CPU */
    find_two_least_busy_cores();

    if (0 == strcmp(placement, "--same-core")) {
        cpu_prod = cpu_cons;
    } else if (0 == strcmp(placement, "--siblings")) {
        cpu_prod = sibling_of(cpu_cons);
        if (cpu_prod < 0) {
            fprintf(stderr, "CPU %d has no HT sibling\n", cpu_cons);
            return EXIT_FAILURE;
        }
    } else if (0 == strcmp(placement, "--cores")) {
        /* The least-busy pair may be HT siblings: force distinct physical cores */
        int sib = sibling_of(cpu_cons);
        if (cpu_prod == cpu_cons || cpu_prod == sib) {
            int n = sysconf(_SC_NPROCESSORS_ONLN);
            for (int i = 0; i < n; i++) {
                if (i != cpu_cons && i != sib) {
                    cpu_prod = i;
                    break;
                }
            }
        }
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    printf("Placement %s: producer CPU %d, consumer CPU %d\n",
           placement, cpu_prod, cpu_cons);
    printf("Array size: %ld\n", arr_size);
    printf("Samples: %ldm%s\n", num_messages / 1000000L,
           use_wait ? ", mode: wait (futex)" : "");

    /* Just for nice printing */
    setlocale(LC_ALL, "");

    /* Init the Ring Buffer strcuture + array. We want "arr_size" members, but not more than 1Mb allocation */
    ring_buf = rb_alloc_init(arr_size, 1024*1024);

    /* Oops, could not allocate. Cry and die. */
    if (NULL == ring_buf) {
        fprintf(stderr, "Failed to initialize ring_buf.\n");
        return EXIT_FAILURE;
    }

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

