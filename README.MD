# Nictus Oculi

A lock-free ring buffer for C, in the spirit of the LMAX Disruptor: one producer
thread hands messages to one consumer thread through preallocated memory, with no
locks, no allocation on the hot path and — on the polling API — no system calls at
all. The whole library is one translation unit (`ring_buf.c` + `ring_buf.h`) and
ships as the static archive `libnictus.a`.

Three numbers, because they answer three different questions. All of them measured
on one laptop CPU (Intel Core i7-10850H), two threads, real-time priority.

**Throughput, batched.** Pushing in bursts of 256 through `rb_push_int_burst`, the
indexed build moves **1.17 billion messages per second** between two physical
cores — **0.85 ns of CPU per message**.

**Throughput, one message per call.** With no batching at all — a plain
`rb_push_int` per message, which is how most code will use it — the same machine
does **387 M msg/s** between two physical cores (**2.6 ns per message**) and
**535 M msg/s** between two hyperthreads (**1.9 ns per message**). The default
line format does 323 M and 381 M msg/s in the same two places.

So batching is worth about **3×**, and it is never applied behind your back: you
call the burst API or you do not. What it costs is worst-case latency, because a
message has to wait for its own batch to fill — the *Observed performance* section
below prices that exactly.

**Latency, one message alone in flight.** **14.6 ns** between two hyperthreads,
**56 ns** between two physical cores. That is what a message costs when there is
no queue ahead of it, and it is within a small factor of the hardware floor — see
*How close is this to the hardware limit?* for the measurement and the citation.

---

## The one rule: single producer, single consumer

**Exactly one thread may push and exactly one (other) thread may pull.**

Two producers, or two consumers, corrupt the buffer. This is not policed at
runtime — the checks would cost more than the whole data path. If you need many
producers, give each one its own ring; that is both faster and simpler than
making one ring multi-writer.

Two further contracts, equally unpoliced:

- **One instance, one payload mode.** A ring created with `rb_alloc_init()`
  carries `int64_t` values and may only be used with `rb_push_int`/`rb_pull_int`
  and their burst forms. A ring created with `rb_alloc_init_ptr()` carries
  pointer+size records and may only be used with `rb_push_ptr`/`rb_pull_ptr`.
  Mixing the two on one instance is undefined.
- **`rb_pull_ptr()` demands clean output variables.** `*data` must be `NULL` and
  `*size` must be `0` on entry, otherwise it returns `RB_PARAM_ERROR`. This
  catches the classic bug of pulling into a variable that still owns a buffer.

---

## Quick start

```sh
make                                          # builds libnictus.a and the test programs

sudo ./ring_buf_test.out --cores              # throughput + FIFO order, two cores
sudo ./ring_buf_test.out --siblings           # same, on one core's two hyperthreads
sudo ./ring_buf_test.out --same-core          # same, both threads on one logical CPU
sudo ./ring_buf_test.out --cores --wait       # blocking mode: futex, no spinning
sudo ./ring_buf_test.out --cores --auto-batch # the library picks the burst size

sudo ./ring_buf_ping_pong.out --cores         # one message in flight = true latency
sudo ./ring_buf_ping_pong.out --siblings      # the same between hyperthreads
```

Both benchmarks ask for `SCHED_FIFO` priority 99 so the OS cannot preempt the two
threads mid-measurement. Without root they print `pthread_setschedparam:
Operation not permitted` and carry on at ordinary priority: the results stay
correct, but they are noisier and the tail grows. Every throughput and latency
figure in the tables below was measured as root; the two probes in *How close is
this to the hardware limit?* were not, and say so where they appear.

```c
#include "ring_buf.h"

/* 8192 requested cells, refuse to allocate more than 1 MB */
ring_buf_t *rb = rb_alloc_init(8192, 1024 * 1024);
if (!rb)
    return -1;

/* producer thread */
if (rb_push_int(rb, value) != RB_OK) {
    /* RB_FULL: the consumer is behind - retry, drop, or back off */
}

/* consumer thread */
int64_t v;
if (rb_pull_int(rb, &v) == RB_OK) {
    /* got one */
}

rb_destroy(rb);
```

`num_cells` must be a power of two. In the default build the usable capacity is
7/8 of it (8192 → 7168), because every 64-byte line carries seven messages plus
a publication flag; see *Two int64 layouts* below.

---

## How it is built inside

- **One contiguous allocation.** The control state and the data area come from a
  single `aligned_alloc(128, …)` block. There is nothing to free but the block,
  and nothing to walk on the hot path.
- **Three 128-byte control sections**: read-only constants, producer-owned state,
  consumer-owned state — then the data. The producer and the consumer never write
  the same cache line, and the 128-byte stride also defeats Intel's adjacent-line
  prefetcher, which would otherwise drag the neighbour's line along.
- **No internal pointers.** Every reference inside the object is an offset, so the
  buffer can be placed in shared memory and mapped at a *different address* in
  each process. Two processes can share one ring with no extra machinery.
- **One release/acquire pair per direction on the data path. No fences, no
  read-modify-write, no sequentially-consistent operations there.** A side reads
  its own fields relaxed; the payload is ordered by the single publication edge.
  On x86-64 the six data-path entry points compile to plain `MOV` instructions —
  no `lock`, no `mfence`, no `xchg`; on AArch64 they are `ldar`/`stlr` with no
  `dmb`. Both facts are enforced by a test (see *Correctness* below), not merely
  believed. The blocking layer is the deliberate exception: parking and waking
  threads uses fences and `seq_cst` read-modify-write operations, because that
  path is cold and correctness there is worth more than a few cycles.

---

## Two int64 layouts

The int64 ring exists in two shapes, chosen at compile time. They share the API
exactly; only the internal layout differs.

|                  | **line** (default)                             | **indexed** (`-DRB_INT_INDEXED`) |
|------------------|------------------------------------------------|----------------------------------|
| Cell             | 64-byte line: one flag + 7 messages            | 8-byte cell                      |
| Publication      | one release store publishes the whole line     | free-running head/tail indices   |
| Usable capacity  | 7/8 of the requested size                      | all of it                        |
| Flag and payload | travel in **one** cache-line transfer          | two separate transfers           |
| Best at          | low latency, small messages, unbatched traffic | high throughput when batching    |

The line format wins whenever messages arrive one at a time: flag and data ride in
the same cache-line transfer, so a message costs one coherence round trip instead
of two. The indexed format pays more per publication, which is exactly why
batching lifts it further — with bursts of 256 it reaches 1.17 G msg/s where the
line format tops out at 0.77 G. Without batching the order reverses on the
default placement: 387 M against 323 M msg/s.

Build the second variant with:

```sh
make CFLAGS='-Wall -Wextra -std=c11 -O3 -march=native -flto -funroll-loops -fomit-frame-pointer -DRB_INT_INDEXED'
```

---

## The API, in four levels

Every function returns a status from the `RB_*` enum in `ring_buf.h`: `RB_OK` is
zero and every error is negative (`RB_FULL`, `RB_EMPTY`, `RB_ERROR`,
`RB_PARAM_ERROR`, `RB_MEMORY_FAIL`, `RB_TIMEOUT`, `RB_CLOSED`).

**1. One message at a time.** The simplest and the lowest-latency path.

```c
int  rb_push_int(ring_buf_t *d, int64_t v);          /* RB_OK / RB_FULL  */
int  rb_pull_int(ring_buf_t *d, int64_t *v);         /* RB_OK / RB_EMPTY */
```

**2. Bursts.** Amortize the publication cost over many messages. Both calls
return *how many* messages they actually moved — never an error for a partial
transfer — so a short result simply means the ring filled up or ran dry.

```c
int n = rb_push_int_burst(rb, batch, 64);   /* 0..64 pushed */
int k = rb_pull_int_burst(rb, out,   64);   /* 0..64 pulled */
```

**3. Adaptive batching.** The library measures how fast the producer is calling it
and returns the burst size that keeps the pipeline near its efficiency knee — big
enough to amortize, small enough not to sit on messages.

```c
uint32_t b = rb_batch_size(rb);             /* then push b messages */
```

This is the setting to reach for when you want throughput *and* short queues. In
the matrix below, on two physical cores it keeps 89% of the peak rate with a tail
1.8× shorter; on hyperthread siblings it gives up 36% of the throughput and cuts
the tail by **86×** — 111 ns instead of 9.6 µs.

**4. Blocking.** When burning a core on polling is the wrong trade, the waiting
form parks the thread on a futex and is woken by the other side. This is the only
part of the library that makes a system call, and it makes none while messages are
flowing.

```c
rb_push_wait(rb, v, 0);              /* 0 = wait forever          */
rb_pull_wait(rb, &v, 1000000);       /* or a timeout in ns        */
rb_wake(rb);                         /* release both sides: RB_CLOSED */
```

**Pointer payloads.** The same shapes exist for pointer+size records:

```c
ring_buf_t *rb = rb_alloc_init_ptr(4096, 1024 * 1024);
rb_push_ptr(rb, buf, len);

void  *p = NULL;                     /* must be NULL */
size_t n = 0;                        /* must be 0    */
rb_pull_ptr(rb, &p, &n);
```

---

## Observed performance

Measured on an Intel Core i7-10850H (6 cores / 12 threads, 2.7 GHz base), two
threads under `SCHED_FIFO` priority 99, 200 million messages per run, 5–12
repetitions until the bootstrap 95% confidence interval of the median throughput
was within ±1%. Latencies are end-to-end: stamped when the message is *created*
and measured when it is *pulled*.

| Configuration                 | Threads on         | Throughput       | per msg | p50    | p99    |
|-------------------------------|--------------------|------------------|---------|--------|--------|
| indexed, bursts of 256        | two physical cores | **1172 M msg/s** | 0.85 ns | 7.0 µs | 8.6 µs |
| indexed, adaptive batch       | two physical cores | 1043 M msg/s     | 0.96 ns | 4.2 µs | 4.9 µs |
| indexed, bursts of 256        | HT siblings        | 1013 M msg/s     | 0.99 ns | 8.6 µs | 9.6 µs |
| indexed, adaptive batch       | HT siblings        | 648 M msg/s      | 1.54 ns | 86 ns  | 111 ns |
| indexed, one message per call | HT siblings        | 535 M msg/s      | 1.87 ns | 196 ns | 277 ns |
| indexed, one message per call | two physical cores | 387 M msg/s      | 2.59 ns | 236 ns | 1.2 µs |
| line, one message per call    | two physical cores | 323 M msg/s      | 3.10 ns | 127 ns | 9.6 µs |
| indexed, bursts of 256        | one logical CPU    | 272 M msg/s      | 3.67 ns | 16 µs  | 44 µs  |

The transfer itself, measured separately with a ping-pong test that keeps exactly
one message in flight:

| Threads on              | One-way latency |
|-------------------------|-----------------|
| HT siblings (shared L1) | **14.6 ns**     |
| Two physical cores      | 56 ns           |
| One logical CPU         | 506 ns          |

**Read the two tables together.** The throughput test saturates the ring on
purpose, so its latency figures are dominated by *queueing*, not by transport: by
Little's law, the top row doing 1172 M msg/s with a 7.0 µs mean is simply holding
**8,283 messages** in the ring at any instant. The adaptive row on siblings holds
**58**, which is why its p99 is 111 ns rather than microseconds — same library,
same machine, a queue two orders of magnitude shorter. If your producer is slower
than your consumer — the normal case in real systems — the ring stays short and
what you will actually observe is the ping-pong number, not the middle of the
histogram.

One caveat that no amount of care removes: the machine had no
`isolcpus`/`nohz_full` isolation, so the worst-case tail belongs to the OS timer
tick rather than to the ring.

### How much does one message vary?

An average is a poor way to buy a ring buffer, so here is the whole spread. Every
figure comes from the same runs as the table above; percentiles are pooled over
all repetitions of a configuration (1.4–2.4 million sampled messages each), and
the last column is the delay band that swallowed the largest share of them.

| Configuration            | min     | p50     | mean    | std     | p99     | p99.9   | Most messages land in |
|--------------------------|---------|---------|---------|---------|---------|---------|-----------------------|
| indexed, adaptive, sib.  | 57 ns   | 86 ns   | 89 ns   | 284 ns  | 112 ns  | 295 ns  | 64–128 ns — **99.4%** |
| indexed, single, sib.    | 45 ns   | 199 ns  | 205 ns  | 231 ns  | 283 ns  | 4.7 µs  | 128–256 ns — 98.4%    |
| indexed, single, cores   | 62 ns   | 236 ns  | 352 ns  | 975 ns  | 7.7 µs  | 16.1 µs | 128–256 ns — 47.3%    |
| line, single, cores      | 42 ns   | 130 ns  | 938 ns  | 2.4 µs  | 10.7 µs | 28.6 µs | 64–128 ns — 48.3%     |
| indexed, adaptive, cores | 112 ns  | 4.1 µs  | 4.1 µs  | 760 ns  | 5.0 µs  | 17.4 µs | 1–4 µs — 51.8%        |
| indexed, b256, cores     | 843 ns  | 7.0 µs  | 7.1 µs  | 1.2 µs  | 10.6 µs | 24.9 µs | 4–16 µs — 98.6%       |
| indexed, b256, sib.      | 544 ns  | 8.5 µs  | 8.3 µs  | 1.0 µs  | 9.6 µs  | 22.2 µs | 4–16 µs — 99.6%       |
| indexed, b256, one CPU   | 15.3 µs | 16.1 µs | 60.9 µs | 6.44 ms | 44.0 µs | 48.6 µs | 4–16 µs — 66.0%       |

Three things are worth reading off this table.

**Saturated batching is remarkably *tight*, not just slow.** The b256 rows put
98–99% of all messages inside a single 2-octave band and hold a standard deviation
around 1 µs on a 7–8 µs mean. That is queueing, and queueing is regular: every
message waits for very nearly the same number of messages ahead of it.

**The unbatched rows are the opposite shape.** `line, single, cores` has a median
of 130 ns but a mean of 938 ns and a standard deviation of 2.4 µs — the mean sits
seven times above the median because a small minority of messages waits far
longer. When a distribution looks like that, the mean is not a description of
anything; the modal band (48% at 64–128 ns) and p99.9 (28.6 µs) are.

**One configuration is both fast and predictable**: adaptive batching on
hyperthread siblings, with 99.4% of messages in the 64–128 ns band, a standard
deviation of 284 ns and p99.9 at 295 ns. If you need an answer to "how long will
this take" rather than "how many can I push", that is the row to copy.

The single worst sample of a run is deliberately absent from this table: one
sample in two million says nothing about a distribution, and on this unisolated
machine it is an OS timer tick every time. `p99.9` is the honest tail number.

The one-CPU row still carries a fingerprint of that: its standard deviation,
6.44 ms, comes from a single 950 ms sample per run, which was Linux real-time
throttling rather than the ring (the note in *Building and integrating* explains
the mechanism and the fix). Every other figure in the row, and every other row,
is unaffected.

---

## How close is this to the hardware limit?

A message cannot travel faster than the cache line carrying it. That sets a floor
no software can go under — and it is a *different* floor depending on where the
two threads sit.

**Between two physical cores** the line must be handed from one core's private
cache to the other's. The cost of that hand-off has been measured and published:

> "On the Xeon, the 44 cycles is the local latency to the LLC, but also
> corresponds to the fastest communication between two cores on the same socket."
>
> — Tudor David, Rachid Guerraoui, Vasileios Trigonakis, *Everything You Always
> Wanted to Know About Synchronization but Were Afraid to Ask*, SOSP '13,
> pp. 33–48, §5.1

And one message needs that line to move **twice** — once so the sender can write
it, once so the receiver can read it:

> "Not surprisingly, a one-way message over cache coherence costs roughly twice
> the latency of transferring a cache line. Once a core x receives a message, it
> brings the receive buffer (i.e., a cache line) to its local caches."
>
> — *ibid.*, §6.2

Everything below is quoted in core cycles, because that is what the hardware and
the literature count in. This machine holds **3.55 GHz** under a two-thread load
(measured: 2,458,367,856 cycles in 692 ms), so **one cycle is 0.28 ns** — all
conversions here use that.

So the floor for one message between two cores is roughly 2 × 44 = 88 core
cycles, and 44 cycles is 12.4 ns here: the floor is **about 25 ns**.

**Between two hyperthreads of one core** there is no transfer at all, because the
two threads share the L1 cache. The same study measures that case on a core whose
hardware threads share their L1:

> "Each L1 cache is shared among the 8 hardware threads of a core and is
> write-through to the LLC."
>
> "On the Niagara, a load costs either an L1 or an L2 access, depending on whether
> the two threads reside on the same core."
>
> — *ibid.*, §3.3 and §5.2

Linux reports the same structure for this laptop:
`/sys/devices/system/cpu/cpu0/cache/index0/shared_cpu_list` reads `0,6` — CPU 0
and CPU 6 share L1d, L1i **and** L2; only the 12 MB L3 is shared by all twelve
threads. With an L1 access at 5 cycles (1.4 ns here) the floor there is about
10 cycles, **≈ 3 ns**.

**What this machine's own counters say.** The theory can be checked against the
hardware directly: `perf mem record` samples the latency *and the data source* of
individual loads, so the cache-to-cache transfer shows up as a measured number
rather than a model.

| Load data source                                         | two physical cores       | HT siblings                |
|----------------------------------------------------------|--------------------------|----------------------------|
| L3 hit + snoop **HitM** — line taken from the other core | 123 cycles = **34.6 ns** | **no such samples**        |
| L3 hit, clean line                                       | 85 cycles = 23.9 ns      | 49 cycles = 13.8 ns        |
| L1 hit                                                   | 38 cycles = 10.7 ns      | 37 cycles = 10.4 ns (bulk) |

(Medians of the sampled loads.)

Between siblings there are **no HitM events at all** — the shared L1 showing up as
an absence, exactly as the topology predicts. (PEBS latency weights include
queueing, so they read higher than textbook cache latencies; the ratio between the
rows is the meaningful part, not the absolute values.)

**Where Nictus Oculi lands.** The comparison below adds a probe that shares one
`volatile` word between the two threads with *no synchronization of any kind* — a
deliberate data race, useless as a queue, and about as fast as the hardware can
move a value:

| Threads on         | Unsynchronized word     | **Nictus Oculi** | Model floor |
|--------------------|-------------------------|------------------|-------------|
| Two physical cores | 31 ns min, 67 ns p50    | **56 ns**        | ≈ 25 ns     |
| HT siblings        | 6.5 ns min, 28.5 ns p50 | **14.6 ns**      | ≈ 3 ns      |

The library lands between the minimum and the median of the unsynchronized probe,
while delivering what the probe does not: FIFO order, publication, and safe reuse
of every slot.

(Unlike the throughput matrix, these two latency probes were run *without*
`SCHED_FIFO`, so they are upper bounds: real-time priority can only remove
preemption, never add it.)

**Throughput has a different ceiling.** At 1172 M msg/s the indexed ring retires a
64-byte line every ~24 cycles, or 6.8 ns — *less than the 12.4 ns of a single LLC
access*. That is only
possible because several line transfers are in flight at once, so the throughput
ceiling is set by how many transfers a core can keep outstanding, not by the
latency of any single one. Latency and throughput are bounded by different things,
which is exactly why one configuration cannot win both.

---

## Correctness

Correctness here is not "the test passed once". `tests/` holds seventeen numbered
checks, each self-contained, each rebuilding the library with the flags it needs:

```sh
./tests/deploy-tests-ubuntu.sh    # installs everything the suite needs
./tests/run-all-tests.sh          # ~4 minutes, one progress line per test
```

| #     | Test                   | What it establishes                                                                                                 |
|-------|------------------------|---------------------------------------------------------------------------------------------------------------------|
| 00    | build gates            | clean under `-Werror`, gcc and clang, both variants                                                                 |
| 01–03 | functional             | single, burst and blocking APIs, both variants                                                                      |
| 04    | acceptance             | the throughput benchmark, FIFO order verified per message                                                           |
| 05    | ASan + UBSan           | no memory errors, no undefined behaviour                                                                            |
| 06    | ThreadSanitizer        | no data races, four operating modes                                                                                 |
| 07    | ordering mutation gate | every release/acquire edge is *load-bearing*: weakening any one of the 20 edges must make TSan fail — and all 20 do |
| 08    | CBMC                   | bounded model checking: memory safety, bounds, overflow, 24 verification legs                                       |
| 09    | static analysis        | `gcc -fanalyzer`, `clang-tidy`, `cppcheck`, all clean                                                               |
| 10    | valgrind               | zero errors, zero leaks                                                                                             |
| 11    | wake-drop injection    | the futex layer survives 50% and 90% of wakeups being lost                                                          |
| 12    | park torture           | both threads on one CPU with spinning disabled                                                                      |
| 13    | codegen gate           | x86-64: no `lock`/`mfence`/`xchg` on the hot paths; AArch64: `ldar`/`stlr` with no `dmb`                            |
| 14    | integer audit          | no implicit narrowing, no signed overflow, no wrap hazards                                                          |
| 15    | AFL++                  | persistent-mode fuzzing; 100% line and branch coverage of `ring_buf.c` in both variants                             |
| 16    | GenMC                  | exhaustive weak-memory model checking under RC11 — proves the ordering correct on architectures weaker than x86     |

Test 07 deserves a note, because it is the one that makes the rest credible: it
does not merely check that the code passes ThreadSanitizer, it deliberately
weakens each memory-ordering annotation in turn and requires the sanitizer to
*catch* it. An edge that can be removed without breaking anything was never doing
any work; here all twenty are.

---

## Tuning it to your own system

The numbers above are from one machine. Yours has different cache sizes, a
different core topology and a different idea of what the scheduler owes you. The
tuning harness measures your machine and tells you what to pick:

```sh
sudo python3 bench_matrix.py      # ~20 minutes, no arguments by design
```

It runs the whole matrix — both variants × three thread placements (two physical
cores / hyperthread siblings / one shared CPU) × fifteen modes (single, twelve
burst sizes, adaptive, blocking) — repeating each configuration until the
bootstrap 95% confidence interval of its median throughput is within 1%, so the
comparisons it prints are real differences rather than run-to-run luck.

You get three things:

- `bench_matrix_report.md` — full tables, a pooled regression over every run that
  separates the per-message floor from the per-batch overhead, and the conclusions
  written out in plain sentences;
- `bench_matrix_results.csv` and `bench_matrix_hist.csv` — every raw run and every
  raw latency distribution, so archived measurements can be re-analysed without
  re-measuring;
- a visual summary on screen: for each thread placement, the three configurations
  you would actually choose between — most throughput, smallest 1-in-100 worst
  case, smallest average delay — each with its latency distribution across ten
  bands on one shared logarithmic scale, and next to it the number of messages
  queued inside the ring, so it is always visible whether you are looking at
  transport cost or at queueing.

It ends with a direct recommendation for each of the three goals. Run it as root:
without `SCHED_FIFO` the threads stay preemptible, and the script says so loudly
rather than letting you trust the wrong numbers.

---

## Building and integrating

```sh
make          # libnictus.a, ring_buf_test.out, ring_buf_ping_pong.out,
              # count_minimal_latency.out
make clean
```

To use it in your own program, include the header and link the archive:

```c
#include "ring_buf.h"
```

```sh
gcc -O2 -o my_program my_code.c libnictus.a
```

`-pthread` is not needed for the polling API — the library itself does not use
pthreads. The blocking API uses futexes directly, also without pthreads. Only the
bundled test programs link `-pthread`, and only to start their own threads.

Both test programs require a placement argument and print usage without one:
`--cores` (two distinct physical cores, the default measurement), `--siblings`
(the two hyperthreads of one physical core: no coherence traffic, shared L1),
`--same-core` (both threads on one logical CPU). Sibling pairs are read from
`/sys/devices/system/cpu/cpu*/topology/thread_siblings_list`. Run them under
`sudo` — see *Quick start* for why.

A note for anyone writing their own real-time benchmark, because this cost a day
to find: both programs raise priority in `main`, *before* `pthread_create`, and
rely on `PTHREAD_INHERIT_SCHED` to pass it to the threads. Doing it the obvious
way instead — each thread raising its own priority after pinning itself — opens a
window in which an ordinary-priority thread sits on a CPU already owned by a
real-time spinner. On `--same-core` that window closes only when Linux's
real-time throttle fires, so the first message of the run records
`/proc/sys/kernel/sched_rt_runtime_us` — 950 ms by default — as its latency.

**Requirements:** GCC or Clang with C11 atomics, a POSIX system (Linux
recommended; the blocking API is Linux-specific), x86-64 or AArch64.

---

## Where it does not belong

Honest limits, so nobody discovers them the hard way:

- **Not multi-producer or multi-consumer.** Use one ring per producer.
- **Not a general-purpose queue.** Capacity is fixed at creation; a full ring
  returns `RB_FULL` and the decision to retry, drop or block is yours.
- **The blocking API is Linux-only** (futex). The polling API is portable to any
  platform with C11 atomics.
- **Batching buys throughput with worst-case latency.** A message in a batch of B
  waits for the batch to fill before it is even sent. That price is deterministic
  and measurable — the tuning matrix prints it — but it is never zero.

---

## License

GNU General Public License. Feel free to modify and distribute.

Developed by Sebastian Mountaniol.
