CC = gcc
#CFLAGS = -Wall -Wextra -std=c11 -O2 -pthread
CFLAGS = -Wall -Wextra -std=c11 -O3 -march=native -flto -funroll-loops -fomit-frame-pointer

TARGET = ring_buf_test.out
PING = ring_buf_ping_pong.out
MINLAT = count_minimal_latency.out
NAIVE = ring_buf_test_naive.out
LIBNAME = nictus.a
ARCHIVE = lib$(LIBNAME)
LIBS=-pthread -lm
SRCS = ring_buf_test_int.c
OBJS = $(SRCS:.c=.o)
RING_BUF_OBJ = ring_buf.o

all: $(ARCHIVE) $(TARGET) $(PING) $(MINLAT) $(NAIVE)

# Step 1: Compile ring_buf.c into an object file
$(RING_BUF_OBJ): ring_buf.c ring_buf.h
	$(CC) $(CFLAGS) -c ring_buf.c -o $(RING_BUF_OBJ)

# Step 2: Create the static library (.a)
$(ARCHIVE): $(RING_BUF_OBJ)
	ar rcs $(ARCHIVE) $(RING_BUF_OBJ)

# Step 3: Compile and link the test program with the static library
$(TARGET): $(OBJS) $(ARCHIVE)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(ARCHIVE) $(LIBS)

# Ping-pong latency test
$(PING): ring_buf_ping_pong.o $(ARCHIVE)
	$(CC) $(CFLAGS) -o $(PING) ring_buf_ping_pong.o $(ARCHIVE) $(LIBS)

# Raw one-way latency floor probe (no ring buffer, no synchronization)
$(MINLAT): count_minimal_latency.c
	$(CC) $(CFLAGS) -o $(MINLAT) count_minimal_latency.c $(LIBS)

# Naive mutex reference: the SAME test program linked against a textbook
# ring buffer (one mutex per message) instead of the lock-free library
$(NAIVE): $(OBJS) rb_naive.c ring_buf.h
	$(CC) $(CFLAGS) -o $(NAIVE) $(OBJS) rb_naive.c $(LIBS)

# Rule for compiling object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up generated files
clean:
	rm -f $(TARGET) $(PING) $(MINLAT) $(NAIVE) $(OBJS) ring_buf_ping_pong.o $(RING_BUF_OBJ) $(ARCHIVE)
