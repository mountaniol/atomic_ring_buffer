#define _GNU_SOURCE  /* sched_yield */

/*
 * AFL++ persistent-mode harness for the v2 SPSC ring buffer.
 *
 * Speed design: the process persists across testcases (__AFL_LOOP, shared
 * memory), and the producer/consumer WORKER THREADS persist too - they are
 * created once and park on a spin barrier between sessions, so an iteration
 * costs no pthread_create/join.  Per-iteration work is capped so a testcase
 * runs in tens of microseconds.
 *
 * Each testcase derives one scenario:
 *   - ring size 2..4096 cells: tiny rings stress line wrap and full/empty
 *   - int64 or ptr+size mode (one ring instance = one mode)
 *   - per-phase busy delays on BOTH sides: the fuzzer controls the speed
 *     ratio of producer vs consumer over time, driving the ring through
 *     near-empty lockstep, near-full backpressure and every transition
 *   - spin budget before sched_yield; which side starts staggered
 *
 * Invariants (any violation -> abort -> AFL crash):
 *   - strict FIFO order and payload integrity (value/pointer/size)
 *   - push returns only RB_OK/RB_FULL, pull only RB_OK/RB_EMPTY
 *   - pushed == pulled == num_messages, ring empty after the session
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <sched.h>
#include <unistd.h>
#include "ring_buf.h"

#ifndef __AFL_FUZZ_TESTCASE_LEN
/* Plain-gcc fallback: one testcase from stdin (smoke testing without AFL) */
static ssize_t fuzz_len;
static unsigned char fuzz_buf[4096];
#define __AFL_FUZZ_TESTCASE_LEN fuzz_len
#define __AFL_FUZZ_TESTCASE_BUF fuzz_buf
#define __AFL_FUZZ_INIT() void rb_afl_dummy(void)
#define __AFL_INIT() (void)0
#define __AFL_LOOP(x) ((fuzz_len = read(0, fuzz_buf, sizeof(fuzz_buf))) > 0)
#endif

__AFL_FUZZ_INIT();

/* Scenario parameters, clamped from raw fuzz input */
typedef struct {
    uint32_t num_messages;  /* 1 .. 2048 */
    uint32_t cells_pow2;    /* ring size = 2^(1..12) */
    uint32_t spin_limit;    /* pushes/pulls tried before sched_yield, 1..1024 */
    uint32_t start_order;   /* which side starts with an extra initial delay */
    uint32_t mode;          /* 0: int64 ring, 1: ptr+size ring */
    uint32_t phase_len;     /* messages per speed phase, 1 .. 512 */
    uint8_t  p_delay[4];    /* producer busy-delay per phase, 0..31 spins */
    uint8_t  c_delay[4];    /* consumer busy-delay per phase, 0..31 spins */
} afl_input_t;

static afl_input_t in;
static ring_buf_t *rb;
static uint64_t pushed, pulled;

/* Session control: main release-bumps go_seq to start both workers; each
 * worker release-increments done_cnt when its role is finished. */
static _Atomic uint32_t go_seq, done_cnt;
static volatile int quit;

/* Payload generators for the ptr mode: value must survive the ring intact */
static void *mk_ptr(uint32_t i)  { return (void *)(uintptr_t)(0xA5000000u + i); }
static size_t mk_size(uint32_t i) { return (size_t)(i & 0x7FFFu); }

static void delay_loop(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++)
        ;
}

static void fz_push_int(int64_t v)
{
    for (uint32_t s = 0; s < in.spin_limit; s++) {
        int rc = rb_push_int(rb, v);
        if (RB_OK == rc)
            return;
        if (RB_FULL != rc) {
            fprintf(stderr, "push_int rc %d\n", rc);
            abort();
        }
    }
    for (;;) {
        sched_yield();
        int rc = rb_push_int(rb, v);
        if (RB_OK == rc)
            return;
        if (RB_FULL != rc) {
            fprintf(stderr, "push_int rc %d\n", rc);
            abort();
        }
    }
}

static void fz_pull_int(int64_t *v)
{
    for (uint32_t s = 0; s < in.spin_limit; s++) {
        int rc = rb_pull_int(rb, v);
        if (RB_OK == rc)
            return;
        if (RB_EMPTY != rc) {
            fprintf(stderr, "pull_int rc %d\n", rc);
            abort();
        }
    }
    for (;;) {
        sched_yield();
        int rc = rb_pull_int(rb, v);
        if (RB_OK == rc)
            return;
        if (RB_EMPTY != rc) {
            fprintf(stderr, "pull_int rc %d\n", rc);
            abort();
        }
    }
}

static void fz_push_ptr(void *p, size_t sz)
{
    for (;;) {
        int rc = rb_push_ptr(rb, p, sz);
        if (RB_OK == rc)
            return;
        if (RB_FULL != rc) {
            fprintf(stderr, "push_ptr rc %d\n", rc);
            abort();
        }
        sched_yield();
    }
}

static void fz_pull_ptr(void **p, size_t *sz)
{
    for (;;) {
        *p = NULL;
        *sz = 0;
        int rc = rb_pull_ptr(rb, p, sz);
        if (RB_OK == rc)
            return;
        if (RB_EMPTY != rc) {
            fprintf(stderr, "pull_ptr rc %d\n", rc);
            abort();
        }
        sched_yield();
    }
}

static void produce(void)
{
    if (0 == in.start_order)
        delay_loop(1024);  /* staggered start: consumer goes first */

    for (uint32_t i = 0; i < in.num_messages; i++) {
        delay_loop(in.p_delay[(i / in.phase_len) & 3]);
        if (in.mode)
            fz_push_ptr(mk_ptr(i), mk_size(i));
        else
            fz_push_int((int64_t)i);
        pushed++;
    }
}

static void consume(void)
{
    if (1 == in.start_order)
        delay_loop(1024);  /* staggered start: producer goes first */

    for (uint32_t i = 0; i < in.num_messages; i++) {
        delay_loop(in.c_delay[(i / in.phase_len) & 3]);
        if (in.mode) {
            void *p;
            size_t sz;

            fz_pull_ptr(&p, &sz);
            if (p != mk_ptr(i) || sz != mk_size(i)) {
                fprintf(stderr, "ptr FIFO broken at %u\n", i);
                abort();
            }
        } else {
            int64_t v;

            fz_pull_int(&v);
            if (v != (int64_t)i) {
                fprintf(stderr, "int FIFO broken at %u: got %lld\n",
                        i, (long long)v);
                abort();
            }
        }
        pulled++;
    }
}

/* Persistent worker: parks on the spin barrier between sessions */
static void *worker_fn(void *arg)
{
    int role = (int)(intptr_t)arg;  /* 0 = producer, 1 = consumer */
    uint32_t last = 0;

    for (;;) {
        while (atomic_load_explicit(&go_seq, memory_order_acquire) == last)
            sched_yield();
        last++;

        if (quit)
            break;

        if (0 == role)
            produce();
        else
            consume();

        atomic_fetch_add_explicit(&done_cnt, 1, memory_order_release);
    }
    return NULL;
}

int main(void)
{
    pthread_t prod, cons;

#ifdef __AFL_HAVE_MANUAL_CONTROL
    __AFL_INIT();
#endif

    /* Workers MUST be created after __AFL_INIT(): the forkserver's fork()
     * clones only the calling thread, so threads made earlier would not
     * exist in the fuzzing child. */
    if (pthread_create(&prod, NULL, worker_fn, (void *)0) != 0 ||
        pthread_create(&cons, NULL, worker_fn, (void *)1) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        abort();
    }

    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;

    while (__AFL_LOOP(10000)) {
        if ((size_t)__AFL_FUZZ_TESTCASE_LEN < sizeof(afl_input_t))
            continue;
        memcpy(&in, buf, sizeof(in));

        in.num_messages = in.num_messages % 2048 + 1;
        in.cells_pow2 = in.cells_pow2 % 12 + 1;
        in.spin_limit = in.spin_limit % 1024 + 1;
        in.start_order &= 1;
        in.mode &= 1;
        in.phase_len = in.phase_len % 512 + 1;
        for (int i = 0; i < 4; i++) {
            in.p_delay[i] &= 31;
            in.c_delay[i] &= 31;
        }

        rb = in.mode ? rb_alloc_init_ptr((size_t)1 << in.cells_pow2, 1 << 20)
                     : rb_alloc_init((size_t)1 << in.cells_pow2, 1 << 20);
        if (NULL == rb) {
            fprintf(stderr, "alloc failed, pow2 %u\n", in.cells_pow2);
            abort();
        }
        pushed = pulled = 0;

        /* Run the session on the persistent workers */
        atomic_store_explicit(&done_cnt, 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&go_seq, 1, memory_order_release);
        while (atomic_load_explicit(&done_cnt, memory_order_acquire) != 2)
            sched_yield();

        if (pushed != in.num_messages || pulled != in.num_messages) {
            fprintf(stderr, "pushed %llu pulled %llu want %u\n",
                    (unsigned long long)pushed, (unsigned long long)pulled,
                    in.num_messages);
            abort();
        }

        /* The session consumed everything: the ring must be empty */
        if (in.mode) {
            void *p = NULL;
            size_t sz = 0;

            if (rb_pull_ptr(rb, &p, &sz) != RB_EMPTY)
                abort();
        } else {
            int64_t v;

            if (rb_pull_int(rb, &v) != RB_EMPTY)
                abort();
        }

        rb_destroy(rb);
    }

    quit = 1;
    atomic_fetch_add_explicit(&go_seq, 1, memory_order_release);
    if (pthread_join(prod, NULL) != 0 || pthread_join(cons, NULL) != 0)
        perror("pthread_join");
    return EXIT_SUCCESS;
}
