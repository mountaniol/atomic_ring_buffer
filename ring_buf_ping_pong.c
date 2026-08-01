#define _GNU_SOURCE  /* CPU_ZERO, CPU_SET, pthread_setaffinity_np */

/*
 * Ping-pong latency test: measures the time of a single message through the
 * ring buffer.  Thread A pushes a value into ring "to_pong" and spins for
 * the echo on ring "to_ping"; thread B mirrors it.  Each round trip is
 * timed individually; the one-way latency is RTT/2.
 *
 * Placement decides what is measured: distinct physical cores = the cache
 * coherence floor; HT siblings = shared-L1 handoff; one logical CPU =
 * context-switch latency (spin loops then yield, and fewer iterations).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <math.h>
#include "ring_buf.h"

#define RING_CELLS 64

static int cpu_a = 1, cpu_b = 2;
static long warmup = 10000, iters = 1000000;
static long batches = 500, batch_k = 2000;  /* batch phase: timer-free mean */
static int spin_yield = 0;  /* --same-core: busy-spin never progresses */

static ring_buf_t *to_pong, *to_ping;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void set_cpu(int num)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(num, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        perror("pthread_setaffinity_np");
}

static void set_fifo(void)
{
    struct sched_param p = { .sched_priority = 99 };

    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0)
        perror("pthread_setschedparam");
}

/* Returns the HT sibling of a CPU, or -1 if it has none */
static int sibling_of(int cpu)
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

static void *pong(__attribute__((unused)) void *arg)
{
    set_cpu(cpu_b);
    set_fifo();

    for (long i = 0; i < warmup + iters + batches * batch_k; i++) {
        int64_t v;

        while (rb_pull_int(to_pong, &v) != RB_OK)
            if (spin_yield)
                sched_yield();
        while (rb_push_int(to_ping, v) != RB_OK)
            if (spin_yield)
                sched_yield();
    }
    return NULL;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

    return (x > y) - (x < y);
}

static void usage(const char *prog)
{
    printf("Usage: %s <placement>\n"
           "  --cores        two distinct physical cores (coherence floor)\n"
           "  --siblings     HT siblings of one physical core (shared L1)\n"
           "  --same-core    one logical CPU (context-switch latency)\n"
           "  <cpu_a> <cpu_b> explicit logical CPUs\n"
           "  --help         this text\n", prog);
}

int main(int argc, char **argv)
{
    if (argc == 3) {
        cpu_a = atoi(argv[1]);
        cpu_b = atoi(argv[2]);
    } else if (argc == 2 && 0 == strcmp(argv[1], "--cores")) {
        int sib = sibling_of(cpu_a);
        for (cpu_b = cpu_a + 1; cpu_b == sib; cpu_b++)
            ;  /* first CPU after cpu_a that is not its sibling */
    } else if (argc == 2 && 0 == strcmp(argv[1], "--siblings")) {
        cpu_b = sibling_of(cpu_a);
        if (cpu_b < 0) {
            fprintf(stderr, "CPU %d has no HT sibling\n", cpu_a);
            return EXIT_FAILURE;
        }
    } else if (argc == 2 && 0 == strcmp(argv[1], "--same-core")) {
        cpu_b = cpu_a;
        spin_yield = 1;
        warmup = 100;   /* each round trip costs two context switches */
        iters = 20000;
        batches = 20;
        batch_k = 500;
    } else {
        usage(argv[0]);
        return (argc == 2 && 0 == strcmp(argv[1], "--help")) ? EXIT_SUCCESS
                                                             : EXIT_FAILURE;
    }

    to_pong = rb_alloc_init(RING_CELLS, 1 << 20);
    to_ping = rb_alloc_init(RING_CELLS, 1 << 20);
    uint32_t *rtt = malloc(iters * sizeof(*rtt));

    if (!to_pong || !to_ping || !rtt) {
        fprintf(stderr, "init failed\n");
        return EXIT_FAILURE;
    }

    pthread_t t;

    if (pthread_create(&t, NULL, pong, NULL) != 0) {
        perror("pthread_create");
        return EXIT_FAILURE;
    }
    set_cpu(cpu_a);
    set_fifo();

    /* Timer cost: each sample includes ~2 clock_gettime calls */
    uint64_t t0 = now_ns();
    for (int i = 0; i < 1000; i++)
        (void)now_ns();
    double timer_ns = (double)(now_ns() - t0) / 1000.0;

    for (long i = 0; i < warmup + iters; i++) {
        int64_t v;
        uint64_t start = now_ns();

        while (rb_push_int(to_pong, i) != RB_OK)
            if (spin_yield)
                sched_yield();
        while (rb_pull_int(to_ping, &v) != RB_OK)
            if (spin_yield)
                sched_yield();

        uint64_t stop = now_ns();

        if (v != i) {
            fprintf(stderr, "echo mismatch: got %lld, want %lld\n",
                    (long long)v, (long long)i);
            abort();
        }
        if (i >= warmup)
            rtt[i - warmup] = (uint32_t)(stop - start);
    }

    /* Batch phase: one timestamp pair per batch_k round trips - the clock
     * cost amortizes away, leaving an honest mean.  Round trips (not a
     * one-way stream) are batched because one-way messages overlap in
     * flight, which would measure throughput instead of latency. */
    double bsum = 0, bsum2 = 0, bmin = 1e18, bmax = 0;

    for (long b = 0; b < batches; b++) {
        uint64_t t0 = now_ns();

        for (long k = 0; k < batch_k; k++) {
            int64_t v;

            while (rb_push_int(to_pong, k) != RB_OK)
                if (spin_yield)
                    sched_yield();
            while (rb_pull_int(to_ping, &v) != RB_OK)
                if (spin_yield)
                    sched_yield();
            if (v != k) {
                fprintf(stderr, "batch echo mismatch\n");
                abort();
            }
        }

        double r = (double)(now_ns() - t0) / batch_k;

        bsum += r;
        bsum2 += r * r;
        if (r < bmin) bmin = r;
        if (r > bmax) bmax = r;
    }

    if (pthread_join(t, NULL) != 0)
        perror("pthread_join");

    qsort(rtt, iters, sizeof(*rtt), cmp_u32);

    double mean = 0.0, var = 0.0;
    for (long i = 0; i < iters; i++)
        mean += rtt[i];
    mean /= iters;
    for (long i = 0; i < iters; i++)
        var += (rtt[i] - mean) * (rtt[i] - mean);
    var /= iters;

    printf("CPUs %d/%d, %ld round trips (ring: %d cells)\n",
           cpu_a, cpu_b, iters, RING_CELLS);
    printf("RTT ns:  min %u  p50 %u  p99 %u  max %u  mean %.1f  stddev %.1f\n",
           rtt[0], rtt[iters / 2], rtt[iters / 100 * 99],
           rtt[iters - 1], mean, sqrt(var));
    printf("one-way: p50 %.1f ns, mean %.1f ns (timer cost ~%.0f ns/sample "
           "included)\n", rtt[iters / 2] / 2.0, mean / 2.0, timer_ns * 2);

    double bmean = bsum / batches;

    printf("\nBatch mode (%ld batches x %ld round trips, one timestamp pair "
           "per batch):\n", batches, batch_k);
    printf("RTT mean %.1f ns  (min %.1f  max %.1f  stddev %.1f across "
           "batches)\n", bmean, bmin, bmax,
           sqrt(bsum2 / batches - bmean * bmean));
    printf("one-way mean: %.1f ns - timer-free\n", bmean / 2.0);

    printf("\nWhat the numbers mean:\n"
           "  One round trip (RTT): thread A pushes a value into ring 1, thread B\n"
           "  pulls it and pushes it back into ring 2, A pulls the echo.\n"
           "  This is APPLICATION-OBSERVED time: cross-core transport plus message\n"
           "  detection (spin-loop exit) plus the push/pull calls themselves -\n"
           "  exactly what a real consumer experiences.  All times in nanoseconds.\n"
           "  min    - the fastest round trip observed\n"
           "  p50    - the median: half of the round trips were faster than this\n"
           "  p99    - 99%% of round trips were faster than this; everything above\n"
           "           is mostly OS noise (interrupts, scheduler ticks)\n"
           "  max    - the worst single round trip (almost always an interrupt hit)\n"
           "  stddev - spread of RTT around the mean\n"
           "  one-way = RTT/2: time of ONE message travelling from A to B.\n"
           "  Per-sample numbers include two clock readings (~%.0f ns) each; the\n"
           "  batch numbers do not - there the whole batch is timed with a single\n"
           "  timestamp pair, so the clock cost divides by %ld and vanishes.\n"
           "  Round trips (not a one-way stream) are batched because one-way\n"
           "  messages overlap in flight: that measures throughput, not latency.\n",
           timer_ns * 2, batch_k);

    free(rtt);
    rb_destroy(to_pong);
    rb_destroy(to_ping);
    return EXIT_SUCCESS;
}
