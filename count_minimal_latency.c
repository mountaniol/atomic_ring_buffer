#define _GNU_SOURCE  /* CPU_ZERO, CPU_SET, pthread_setaffinity_np */

/*
 * count_minimal_latency: lower-bound estimate of one-way cross-core latency.
 *
 * One shared volatile int64 word and NO synchronization of any kind - this
 * is a deliberate, measurement-only data race (UB by C11; do not reuse this
 * pattern in production code).  Aligned 8-byte MOVs are single-copy atomic
 * on x86-64/ARM64 hardware, so a reader sees stale or skipped values, never
 * torn ones.
 *
 * Word layout: low 20 bits = counter, high 44 bits = CLOCK_MONOTONIC
 * nanoseconds mod 2^44 (0 = no stamp).  The producer hammers the word with
 * counters and embeds a stamp every STAMP_EVERY-th value; the consumer
 * polls, and for every stamped value it happens to catch computes
 * now() - stamp.  The clock-read cost is calibrated with a million calls
 * before the run; delta includes roughly one full clock read (tail of the
 * producer's + head of the consumer's), so "corrected" numbers subtract it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <math.h>
#include <unistd.h>

#define TOTAL       50000000ull  /* values the producer writes */
#define STAMP_EVERY 1000ull      /* every n-th value carries a timestamp */
#define STAMP_HOLD  64           /* repeat a stamped store: consecutive
                                  * same-address stores coalesce in the store
                                  * buffer, so an un-held stamp may never
                                  * become globally visible at all */
#define SEQ_BITS    20
#define SEQ_MASK    ((1ull << SEQ_BITS) - 1)
#define TS_MASK     ((1ull << 44) - 1)
#define FIN         UINT64_MAX   /* producer's end-of-run sentinel */
#define MAX_SAMPLES 65536
#define WINDOW_NS   1000000000ull /* deltas above 1 s = wrap garbage, drop */

static int cpu_a = 1, cpu_b = 2;

/* The single shared word, alone in its cache line */
static _Alignas(64) volatile uint64_t word;

static uint64_t stamps_sent;
static uint64_t seen_values, caught_stamps;
static int32_t delta_ns[MAX_SAMPLES];  /* signed: negative = clock skew */
static long deltas;

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

static void *producer(__attribute__((unused)) void *arg)
{
    set_cpu(cpu_a);

    for (uint64_t i = 1; i <= TOTAL; i++) {
        uint64_t w = i & SEQ_MASK;

        if (0 == i % STAMP_EVERY) {
            w |= (now_ns() & TS_MASK) << SEQ_BITS;
            stamps_sent++;
            /* Hold the stamp visible: without this the very next store may
             * coalesce over it before it ever leaves the store buffer.  The
             * consumer counts it once (first observation), so the hold does
             * not bias the measured delta. */
            for (int h = 0; h < STAMP_HOLD; h++)
                word = w;
        } else {
            word = w;  /* plain volatile store: one MOV, no synchronization */
        }
    }
    word = FIN;
    return NULL;
}

static void *consumer(__attribute__((unused)) void *arg)
{
    uint64_t last = 0;

    set_cpu(cpu_b);

    for (;;) {
        uint64_t w = word;  /* plain volatile load */

        if (FIN == w)
            break;
        if (w == last)
            continue;  /* nothing new in the word */
        last = w;
        seen_values++;

        uint64_t ts = w >> SEQ_BITS;

        if (ts) {  /* a stamped value we managed to catch */
            uint64_t d = ((now_ns() & TS_MASK) - ts) & TS_MASK;
            /* mod-2^44 result -> signed: a small negative delta means the
             * consumer's clock runs behind the producer's (cross-CPU skew) */
            int64_t sd = (d > TS_MASK / 2) ? (int64_t)d - (int64_t)(TS_MASK + 1)
                                           : (int64_t)d;

            caught_stamps++;
            if (sd > -1000 && sd < (int64_t)WINDOW_NS && deltas < MAX_SAMPLES)
                delta_ns[deltas++] = (int32_t)sd;
        }
    }
    return NULL;
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;

    return (x > y) - (x < y);
}

static void usage(const char *prog)
{
    printf("Usage: %s <placement>\n"
           "  --cores        two distinct physical cores (coherence floor)\n"
           "  --siblings     HT siblings of one physical core (shared L1)\n"
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
            ;
    } else if (argc == 2 && 0 == strcmp(argv[1], "--siblings")) {
        cpu_b = sibling_of(cpu_a);
        if (cpu_b < 0) {
            fprintf(stderr, "CPU %d has no HT sibling\n", cpu_a);
            return EXIT_FAILURE;
        }
    } else {
        usage(argv[0]);
        return (argc == 2 && 0 == strcmp(argv[1], "--help")) ? EXIT_SUCCESS
                                                             : EXIT_FAILURE;
    }

    /* Calibrate the clock-read cost: a million back-to-back calls */
    uint64_t t0 = now_ns();
    for (int i = 0; i < 1000000; i++)
        (void)now_ns();
    double timer_ns = (double)(now_ns() - t0) / 1e6;

    pthread_t prod, cons;

    if (pthread_create(&cons, NULL, consumer, NULL) != 0 ||
        pthread_create(&prod, NULL, producer, NULL) != 0) {
        perror("pthread_create");
        return EXIT_FAILURE;
    }
    if (pthread_join(prod, NULL) != 0 || pthread_join(cons, NULL) != 0)
        perror("pthread_join");

    if (0 == deltas) {
        fprintf(stderr, "no usable samples: %llu values observed, %llu stamps "
                "sent, %llu stamps caught (0 caught = stamps never became "
                "visible; caught but 0 usable = clock skew beyond limits)\n",
                (unsigned long long)seen_values,
                (unsigned long long)stamps_sent,
                (unsigned long long)caught_stamps);
        return EXIT_FAILURE;
    }

    qsort(delta_ns, deltas, sizeof(*delta_ns), cmp_i32);

    double mean = 0.0, var = 0.0;
    for (long i = 0; i < deltas; i++)
        mean += delta_ns[i];
    mean /= deltas;
    for (long i = 0; i < deltas; i++)
        var += (delta_ns[i] - mean) * (delta_ns[i] - mean);
    var /= deltas;

    printf("CPUs %d/%d: %llu writes, %llu observed by consumer, "
           "%llu stamps sent, %llu caught (%ld measured)\n",
           cpu_a, cpu_b, TOTAL, (unsigned long long)seen_values,
           (unsigned long long)stamps_sent,
           (unsigned long long)caught_stamps, deltas);
    printf("clock read cost: %.1f ns\n", timer_ns);
    printf("one-way ns (raw):       min %d  p50 %d  p99 %d  max %d  "
           "mean %.1f  stddev %.1f\n",
           delta_ns[0], delta_ns[deltas / 2], delta_ns[deltas / 100 * 99],
           delta_ns[deltas - 1], mean, sqrt(var));
    printf("one-way ns (corrected): min %.1f  p50 %.1f  mean %.1f  "
           "(raw minus one clock read)\n",
           delta_ns[0] - timer_ns, delta_ns[deltas / 2] - timer_ns,
           mean - timer_ns);

    printf("\nWhat the numbers mean:\n"
           "  A single volatile int64 word is shared with NO synchronization\n"
           "  (deliberate data race, measurement only).  The producer rewrites it\n"
           "  as fast as the cache coherence protocol allows; every %lluth value\n"
           "  embeds a truncated CLOCK_MONOTONIC timestamp.  The consumer polls\n"
           "  the word and, whenever it happens to catch a stamped value, computes\n"
           "  its own now() minus the stamp: the one-way delivery time of that\n"
           "  value.  Values are overwritten in place, so the consumer misses most\n"
           "  of them - that is expected (lossy mailbox semantics).\n"
           "  min      - the fastest delivery observed: the floor of this pair\n"
           "  p50/p99  - typical / tail delivery\n"
           "  corrected- raw minus the calibrated clock-read cost (~one read is\n"
           "             inside every sample: tail of the producer's call plus\n"
           "             head of the consumer's)\n", STAMP_EVERY);
    return EXIT_SUCCESS;
}
