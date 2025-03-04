# Ring Buffer Implementation

This project implements an **atomic ring buffer** based on the **LMAX Disruptor** algorithm. The ring buffer is designed for **high-performance, lock-free inter-thread communication**.

## Features
- **Lock-free design** for fast producer-consumer communication.
- **Works entirely in user space**: No system calls, no kernel interaction—only atomic operations.
- **No dependency on pthreads**: The ring buffer itself does not require the `pthread` library. It is implemented using atomic operations and does not rely on mutexes or condition variables.
- **POSIX thread support (`pthread`) is only used in the test program** for benchmarking and validation.
- **Optimized for performance** using `-O3`, `-march=native`, `-flto`, and `-funroll-loops`.
- **Custom memory allocation** with `posix_memalign()` for cache-friendly access.
- **Minimal memory overhead** compared to traditional queue-based IPC mechanisms.
- **Single memory block allocation**: The ring buffer structure and its internal buffer are allocated as a single contiguous memory block, significantly improving CPU cache performance.
- **Support for inter-process communication**: This ring buffer can be used not only for communication between threads but also between processes via shared memory.

## Comparison with Other IPC Mechanisms
### **POSIX IPC (Message Queues, Shared Memory, Pipes)**
| Feature | POSIX Message Queues | POSIX Shared Memory | Ring Buffer |
|---------|----------------------|----------------------|-------------|
| **Latency** | High (system calls) | Medium (sync overhead) | Low (lock-free) |
| **Memory Overhead** | High (kernel buffer) | Medium | Low |
| **Complexity** | High (syscalls, permissions) | Medium | Low |
| **Thread Safety** | Requires mutexes | Requires synchronization | Lock-free |

### **Linked List Queues vs. Ring Buffer**
- **Linked lists** require **dynamic memory allocation**, which is costly in high-speed operations.
- **Ring buffers** use **preallocated fixed memory**, avoiding heap fragmentation and reducing cache misses.
- **Ring buffers** provide **constant-time complexity (O(1))** for both enqueue and dequeue operations, while linked lists have overhead due to pointer dereferencing.

### **Mutex-based Queues vs. Atomic Ring Buffer**
- **Mutexes introduce contention** in high-frequency producer-consumer interactions.
- **The ring buffer is lock-free**, avoiding thread contention and context switching delays.
- **Better scalability** for multi-threaded applications compared to mutex-based synchronization.
- **No kernel calls involved**: Unlike mutex-based solutions that rely on system calls, this ring buffer operates **entirely in user space**, leveraging atomic operations to ensure high-speed execution without kernel overhead.

## Performance
- **Tested on Intel Nuke (Intel(R) Core(TM) i7-10710U CPU @ 1.10GHz)**.
- **Achieves 70 to 160 million integer transfers per second** between two threads.
- **Other high-performance algorithms tested reach at most 30 million per second**.
- **Linked list-based and POSIX IPC solutions only achieve 2-3 million per second**, demonstrating the significant advantage of this ring buffer for high-speed inter-thread communication.
- **Running with Valgrind**: Shows **zero errors**.
- **Memory usage**: 139,685 bytes allocated as reported by Valgrind.

## Compilation
To compile the project, use the provided `Makefile`. This will:
1. Compile `ring_buf.c` into a **static library (`libiringbuf.a`)**.
2. Compile and link the test program (`ring_buf_test.out`) against the static library.

### **Build Instructions**
Run:
```sh
make
```

This will generate the following files:
- `libringbuf.a` (Static library for the ring buffer)
- `ring_buf_test.out` (Test program)

To clean up compiled files:
```sh
make clean
```

## Usage
### **Running the Test Program**
After compilation, execute:
```sh
./ring_buf_test.out
```
This runs the test scenario, which demonstrates the ring buffer's performance under high-load conditions.

### **Integration in Other Projects**
To use the ring buffer in your own project:
1. Include the header file:
   ```c
   #include "ring_buf.h"
   ```
2. Link against the static library:
   ```sh
   gcc -o my_program my_code.c lringbuf.a
   ```
   *(Note: `-pthread` is not required unless using the test program.)*

## Advantages of This Project
- **Minimal latency**: Avoids system calls, unlike POSIX IPC.
- **No dynamic allocation**: Uses preallocated memory, making it ideal for real-time systems.
- **Scalability**: Can handle multiple producers/consumers with atomic operations.
- **CPU Cache efficiency**: Avoids frequent heap access, optimizing memory locality.
- **High Throughput**: Lock-free structure ensures maximum throughput in multi-threaded environments.
- **Optimized Memory Layout**: The ring buffer and its internal array are allocated as a **single contiguous memory block**, significantly improving CPU cache utilization and reducing cache misses.
- **Inter-Process Communication (IPC) Support**: This ring buffer can be placed in **shared memory**, allowing efficient communication between processes without the overhead of system calls.
- **Pure user-space execution**: The ring buffer operates **entirely in user space**, relying only on atomic operations rather than kernel locks or system calls, ensuring low overhead and high efficiency.
- **No pthread dependency**: The ring buffer does **not** use `pthread` internally. The test program uses `pthread` only for benchmarking, but the ring buffer itself is fully independent of thread libraries.

## Dependencies
- **GCC (GNU Compiler Collection)**
- **POSIX-compliant OS** (Linux recommended)

## License
This project is released under the **MIT License**. Feel free to modify and distribute.

---
Developed by **Sebastian Mountaniol** (04/03/2025).

