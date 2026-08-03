/*
 * v3_ab_bench — open-loop A/B benchmark of the REAL library API:
 *   single  v2 path: rb_push_int / rb_pull_int, one message per call
 *   adapt   v3 path: rb_batch_size() cap + rb_push_int_burst (take-what's-
 *           there, never waits for a batch to fill) / rb_pull_int_burst
 *   b<K>    v3 path with a fixed cap K instead of the adaptive law
 *
 * Producer paces arrivals at a given rate; every message carries its
 * SCHEDULED arrival TSC (coordinated-omission-safe).  Latency is one-way,
 * cross-core TSC-offset corrected.  On overload messages are dropped and
 * counted, never blocked on - drops mark instability.
 *
 * Usage: v3_ab_bench <cpu_p> <cpu_c> <single|adapt|b8|b32|b128> \
 *                    <rate_Mmsg_s> <measure_ms> [warmup_ms]
 * CSV: policy,rate_M,thr_M,drop_pct,chunk,p50,p90,p99,p999,max,mean,samples
 */
#define _GNU_SOURCE
#include <sched.h>
#include <string.h>
#include <x86intrin.h>
#include <time.h>
#include "ring_buf.h"

#define RING_CELLS 8192                 /* same as ring_buf_test: cache-fit */
#define CHUNK_MAX  512
#define HBUCKETS   3840                 /* log-linear, 1ns below 256ns */
#define STOPV  (-1LL)
#define RESETV (-2LL)

enum { P_SINGLE, P_ADAPT, P_FIXED };

static ring_buf_t *rb;
static int policy, fixedB;
static double rate, tsc_hz, ns_per_cyc;
static uint64_t t_warm_end, t_end;      /* cycle deltas, rebased by producer */
static _Atomic uint64_t cal_flag;
static _Atomic uint64_t cal_tsc;
static int64_t tsc_off;
static uint64_t samp_stride;

static uint64_t produced, drops, push_calls;
static uint64_t consumed_n, hist[HBUCKETS];
static uint64_t c_meas_begin, c_meas_end, c_snap, c_meas_cnt;
static double lat_sum;

static inline unsigned hbucket(uint64_t ns)
{
    if (ns < 256)
        return (unsigned)ns;
    unsigned msb = 63 - __builtin_clzll(ns);
    unsigned idx = 256 + (msb - 8) * 64 + ((unsigned)(ns >> (msb - 6)) & 63);
    return idx < HBUCKETS ? idx : HBUCKETS - 1;
}

static double hvalue(unsigned idx)
{
    if (idx < 256)
        return idx;
    unsigned msb = 8 + (idx - 256) / 64, sub = (idx - 256) % 64;
    return (double)(1ULL << msb) * (1.0 + (sub + 0.5) / 64.0);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void measure_tsc_hz(void)
{
    double s0 = now_s();
    uint64_t c0 = __rdtsc();
    struct timespec req = { 0, 200 * 1000 * 1000 };
    nanosleep(&req, NULL);
    tsc_hz = (__rdtsc() - c0) / (now_s() - s0);
}

static void pin_to(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set)) {
        perror("setaffinity");
        exit(1);
    }
}

#define CAL_ITERS 20000
static void cal_producer(void)          /* monotonic-phase handshake */
{
    int64_t best_rtt = INT64_MAX, best_off = 0;
    for (uint64_t i = 0; i < CAL_ITERS; i++) {
        uint64_t t0 = __rdtsc();
        atomic_store_explicit(&cal_flag, 2 * i + 1, memory_order_release);
        while (atomic_load_explicit(&cal_flag, memory_order_acquire) !=
               2 * i + 2)
            _mm_pause();
        uint64_t t1 = __rdtsc();
        uint64_t tc = atomic_load_explicit(&cal_tsc, memory_order_acquire);
        int64_t rtt = (int64_t)(t1 - t0);
        if (rtt < best_rtt) {
            best_rtt = rtt;
            best_off = (int64_t)tc - (int64_t)(t0 + rtt / 2);
        }
    }
    tsc_off = best_off;
}

static void cal_consumer(void)
{
    for (uint64_t i = 0; i < CAL_ITERS; i++) {
        while (atomic_load_explicit(&cal_flag, memory_order_acquire) !=
               2 * i + 1)
            _mm_pause();
        atomic_store_explicit(&cal_tsc, __rdtsc(), memory_order_release);
        atomic_store_explicit(&cal_flag, 2 * i + 2, memory_order_release);
    }
}

static void push_sentinel(int64_t v)    /* blocking: sentinels must land */
{
    while (rb_push_int(rb, v) != RB_OK)
        _mm_pause();
}

static void *producer(void *arg)
{
    pin_to((int)(intptr_t)arg);
    cal_producer();

    uint64_t t0 = __rdtsc() + (uint64_t)(tsc_hz * 0.005);
    t_end = t0 + t_end;
    t_warm_end = t0 + t_warm_end;

    typedef unsigned __int128 u128;
    u128 step_fp = (u128)(tsc_hz / rate * 4294967296.0);
    int G = (int)(rate * 60e-9);
    if (G < 1) G = 1;
    if (G > 96) G = 96;
    u128 gstep_fp = step_fp * (unsigned)G;
    u128 grp_fp = step_fp * (unsigned)(G - 1);

    int64_t chunk[CHUNK_MAX];
    uint32_t cn = 0, B = (policy == P_FIXED) ? (uint32_t)fixedB : 1;
    int sent_reset = 0;
    uint64_t pend_ts = 0;               /* stamp of oldest unpublished msg */
    double cyc_per_msg = tsc_hz / rate;

    for (;;) {
        uint64_t now = __rdtsc();
        if (now >= t_end)
            break;
        int emitted = 0, full = 0;
        while (t0 + (uint64_t)(grp_fp >> 32) <= now) {  /* all due groups */
            if (full) {                 /* overload: O(1) schedule skip */
                uint64_t gcyc = (uint64_t)(gstep_fp >> 32);
                uint64_t k = (now - (t0 + (uint64_t)(grp_fp >> 32)))
                             / (gcyc | 1) + 1;
                drops += k * (uint64_t)G;
                produced += k * (uint64_t)G;
                grp_fp += (u128)k * gstep_fp;
                emitted = 1;
                break;
            }
            u128 s_fp = grp_fp - step_fp * (unsigned)(G - 1);
            for (int g = 0; g < G; g++, s_fp += step_fp) {
                int64_t stamp = (int64_t)(t0 + (uint64_t)(s_fp >> 32));
                produced++;
                if (P_SINGLE == policy) {
                    push_calls++;
                    if (rb_push_int(rb, stamp) != RB_OK) {
                        drops++;
                        full = 1;
                    }
                    continue;
                }
                if (P_ADAPT == policy)
                    B = rb_batch_size(rb);
                if (0 == cn)
                    pend_ts = (uint64_t)stamp;
                chunk[cn++] = stamp;
                if (cn >= B || CHUNK_MAX == cn) {
                    push_calls++;
                    int p = rb_push_int_burst(rb, chunk, cn);
                    if ((uint32_t)p < cn) {
                        drops += cn - (uint32_t)p;
                        full = 1;
                    }
                    cn = 0;
                }
            }
            grp_fp += gstep_fp;
            emitted = 1;
        }
        if (cn && !emitted &&
            now - pend_ts > (uint64_t)(4.0 * B * cyc_per_msg)) {
            /* deadline T_max = 4*B*d: flush a stale partial batch */
            push_calls++;
            int p = rb_push_int_burst(rb, chunk, cn);
            drops += cn - (uint32_t)p;
            cn = 0;
        }
        if (!sent_reset && now >= t_warm_end) {
            if (cn) {
                int p = rb_push_int_burst(rb, chunk, cn);
                drops += cn - (uint32_t)p;
                cn = 0;
            }
            push_sentinel(RESETV);
            sent_reset = 1;
            t_warm_end = t_end;
        }
    }
    if (cn) {
        int p = rb_push_int_burst(rb, chunk, cn);
        drops += cn - (uint32_t)p;
    }
    push_sentinel(STOPV);
    return NULL;
}

static void *consumer(void *arg)
{
    pin_to((int)(intptr_t)arg);
    cal_consumer();

    int64_t buf[CHUNK_MAX], prev = INT64_MIN;
    uint64_t cnt = 1, S = samp_stride;

    for (;;) {
        int k;
        if (P_SINGLE == policy) {
            if (rb_pull_int(rb, buf) != RB_OK) {
                _mm_pause();
                continue;
            }
            k = 1;
        } else {
            k = rb_pull_int_burst(rb, buf, CHUNK_MAX);
            if (k <= 0) {
                _mm_pause();
                continue;
            }
        }
        uint64_t now = 0;               /* rdtsc only if pass has a sample */
        if (cnt <= (uint64_t)k)
            now = __rdtsc();
        for (int i = 0; i < k; i++) {
            int64_t v = buf[i];
            if (v < prev) {             /* sentinels are negative */
                if (STOPV == v) {
                    c_meas_end = __rdtsc();
                    c_meas_cnt = consumed_n - c_snap;
                    return NULL;
                }
                if (RESETV == v) {
                    memset(hist, 0, sizeof(hist));
                    lat_sum = 0;
                    c_snap = consumed_n;
                    c_meas_begin = __rdtsc();
                    continue;
                }
                fprintf(stderr, "ORDER VIOLATION\n");
                abort();
            }
            prev = v;
            consumed_n++;
            if (--cnt == 0) {
                cnt = S;
                int64_t lat_c = (int64_t)now - tsc_off - v;
                if (lat_c < 0)
                    lat_c = 0;
                uint64_t ns = (uint64_t)((double)lat_c * ns_per_cyc);
                hist[hbucket(ns)]++;
                lat_sum += (double)ns;
            }
        }
    }
}

static uint64_t pct(double q)
{
    uint64_t total = 0, acc = 0;
    for (int i = 0; i < HBUCKETS; i++)
        total += hist[i];
    if (!total)
        return 0;
    uint64_t target = (uint64_t)(q * (double)total);
    if (target >= total)
        target = total - 1;
    for (int i = 0; i < HBUCKETS; i++) {
        acc += hist[i];
        if (acc > target)
            return (uint64_t)hvalue(i);
    }
    return (uint64_t)hvalue(HBUCKETS - 1);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <cpu_p> <cpu_c> "
                "<single|adapt|b<K>> <rate_M> <measure_ms> [warmup_ms]\n",
                argv[0]);
        return 1;
    }
    int cpu_p = atoi(argv[1]), cpu_c = atoi(argv[2]);
    const char *pol = argv[3];
    if (!strcmp(pol, "single")) {
        policy = P_SINGLE;
    } else if (!strcmp(pol, "adapt")) {
        policy = P_ADAPT;
    } else if ('b' == pol[0] && (fixedB = atoi(pol + 1)) > 0) {
        policy = P_FIXED;
    } else {
        fprintf(stderr, "bad policy\n");
        return 1;
    }
    rate = atof(argv[4]) * 1e6;
    double meas_ms = atof(argv[5]);
    double warm_ms = (argc > 6) ? atof(argv[6]) : 150.0;
    if (rate <= 0) {
        fprintf(stderr, "bad rate\n");
        return 1;
    }

    measure_tsc_hz();
    ns_per_cyc = 1e9 / tsc_hz;
    samp_stride = 1;                    /* odd stride, <=32M samples/s */
    while (rate / (double)samp_stride > 32e6)
        samp_stride = samp_stride * 2 + 1;

    rb = rb_alloc_init(RING_CELLS, 1 << 24);
    if (!rb)
        return 1;
    t_warm_end = (uint64_t)(warm_ms * 1e-3 * tsc_hz);
    t_end = (uint64_t)((warm_ms + meas_ms) * 1e-3 * tsc_hz);

    pthread_t tp, tc;
    if (pthread_create(&tc, NULL, consumer, (void *)(intptr_t)cpu_c) ||
        pthread_create(&tp, NULL, producer, (void *)(intptr_t)cpu_p)) {
        perror("pthread_create");
        return 1;
    }
    if (pthread_join(tp, NULL) || pthread_join(tc, NULL)) {
        perror("pthread_join");
        return 1;
    }

    double meas_s = c_meas_end > c_meas_begin ?
                    (double)(c_meas_end - c_meas_begin) / tsc_hz : 0;
    double thr = meas_s > 0 ? c_meas_cnt / meas_s / 1e6 : 0;
    double chunk = push_calls ?
                   (double)(produced - drops) / (double)push_calls : 0;
    double drop_pct = produced ? 100.0 * (double)drops / (double)produced : 0;
    uint64_t samples = 0;
    for (int i = 0; i < HBUCKETS; i++)
        samples += hist[i];
    double mean = samples ? lat_sum / (double)samples : 0;

    printf("%s,%.0f,%.2f,%.3f,%.1f,%lu,%lu,%lu,%lu,%lu,%.1f,%lu\n",
           pol, rate / 1e6, thr, drop_pct, chunk,
           pct(0.50), pct(0.90), pct(0.99), pct(0.999), pct(1.0), mean,
           samples);
    rb_destroy(rb);
    return 0;
}
