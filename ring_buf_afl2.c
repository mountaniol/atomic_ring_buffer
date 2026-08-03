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

static afl_input_t in, raw;  /* raw = pre-clamp copy: entropy for contracts */
static ring_buf_t *rb;
static uint64_t pushed, pulled;

#define MAX_ALLOC ((size_t)1 << 20)

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

/* Mirror of the library's allocation-size formula, for exact expectations */
static size_t expected_total(uint32_t pow2, uint32_t mode)
{
    size_t cells = (size_t)1 << pow2;
#ifdef RB_INT_INDEXED
    size_t bytes = mode ? cells * sizeof(cell_t) : cells * sizeof(int64_t);
#else
    size_t bytes = mode ? cells * sizeof(cell_t)
                        : (cells / 8 ? cells / 8 : 1) * 64;
#endif
    return (384 + bytes + 127) & ~(size_t)127;
}

#define CONTRACT(cond) do { \
    if (!(cond)) { fprintf(stderr, "contract failed: %s\n", #cond); abort(); } \
} while (0)

/* Exercise every argument-validation path with fuzz-varied values; a wrong
 * return code is a bug and aborts.  Called once per iteration. */
static void contract_phase(void)
{
    int64_t v;
    void *p;
    size_t sz;

    /* NULL-ring contracts */
    CONTRACT(rb_push_int(NULL, 1) == RB_PARAM_ERROR);
    CONTRACT(rb_pull_int(NULL, &v) == RB_PARAM_ERROR);
    CONTRACT(rb_push_ptr(NULL, &v, 1) == RB_PARAM_ERROR);
    p = NULL;
    sz = 0;
    CONTRACT(rb_pull_ptr(NULL, &p, &sz) == RB_PARAM_ERROR);

    /* Not-a-power-of-two and zero cells are rejected by both constructors */
    size_t np = raw.phase_len % 65536;
    if (np < 3)
        np = 3;
    while (0 == (np & (np - 1)))
        np += 3;
    CONTRACT(rb_alloc_init(np, MAX_ALLOC) == NULL);
    CONTRACT(rb_alloc_init_ptr(np, MAX_ALLOC) == NULL);
    CONTRACT(rb_alloc_init(0, MAX_ALLOC) == NULL);
    CONTRACT(rb_alloc_init_ptr(0, MAX_ALLOC) == NULL);

    /* Size-multiplication overflow guard */
    uint32_t bigp = 58 + raw.spin_limit % 6;  /* 2^58 .. 2^63 cells */
    CONTRACT(rb_alloc_init((size_t)1 << bigp, SIZE_MAX) == NULL);
    CONTRACT(rb_alloc_init_ptr((size_t)1 << bigp, SIZE_MAX) == NULL);

    /* max_alloc_size boundary: limit = size-1 rejects, limit = size accepts */
    uint32_t bp = raw.num_messages % 10 + 1;
    uint32_t bmode = raw.start_order >> 1 & 1;
    size_t need = expected_total(bp, bmode);
    ring_buf_t *b;

    b = bmode ? rb_alloc_init_ptr((size_t)1 << bp, need - 1)
              : rb_alloc_init((size_t)1 << bp, need - 1);
    CONTRACT(NULL == b);
    b = bmode ? rb_alloc_init_ptr((size_t)1 << bp, need)
              : rb_alloc_init((size_t)1 << bp, need);
    CONTRACT(NULL != b);
    rb_destroy(b);
}

/* Contracts that need a live ring of the session's mode */
static void contract_phase_rb(void)
{
    int64_t v;
    void *p;
    size_t sz;

    if (in.mode) {
        /* oversized size is rejected before anything is written */
        CONTRACT(rb_push_ptr(rb, &v, (size_t)INT32_MAX + 1 + raw.cells_pow2)
                 == RB_PARAM_ERROR);
        /* NULL and dirty out-parameters, each || branch separately */
        sz = 0;
        CONTRACT(rb_pull_ptr(rb, NULL, &sz) == RB_PARAM_ERROR);
        p = NULL;
        CONTRACT(rb_pull_ptr(rb, &p, NULL) == RB_PARAM_ERROR);
        p = &v;
        sz = 0;
        CONTRACT(rb_pull_ptr(rb, &p, &sz) == RB_PARAM_ERROR);
        p = NULL;
        sz = 1 + raw.spin_limit % 100;
        CONTRACT(rb_pull_ptr(rb, &p, &sz) == RB_PARAM_ERROR);
    } else {
        CONTRACT(rb_pull_int(rb, NULL) == RB_PARAM_ERROR);
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
        memcpy(&raw, buf, sizeof(raw));
        in = raw;

        in.num_messages = in.num_messages % 2048;      /* 0 is a scenario too */
        in.cells_pow2 = in.cells_pow2 % 16 + 1;        /* 2 .. 65536 cells */
        in.spin_limit = in.spin_limit % 1024 + 1;
        in.start_order &= 1;
        in.mode &= 1;
        in.phase_len = in.phase_len % 512 + 1;
        for (int i = 0; i < 4; i++) {
            in.p_delay[i] &= 31;
            in.c_delay[i] &= 31;
        }

        contract_phase();

        /* Large ptr rings exceed MAX_ALLOC: rejection is the expected result */
        int expect_null = expected_total(in.cells_pow2, in.mode) > MAX_ALLOC;

        rb = in.mode ? rb_alloc_init_ptr((size_t)1 << in.cells_pow2, MAX_ALLOC)
                     : rb_alloc_init((size_t)1 << in.cells_pow2, MAX_ALLOC);
        if ((NULL == rb) != expect_null) {
            fprintf(stderr, "alloc expectation broken, pow2 %u mode %u\n",
                    in.cells_pow2, in.mode);
            abort();
        }
        if (NULL == rb)
            continue;  /* rejection scenario: no session to run */

        contract_phase_rb();
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
