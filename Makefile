CC = gcc
#CFLAGS = -Wall -Wextra -std=c11 -O2 -pthread
CFLAGS = -Wall -Wextra -std=c11 -O3 -march=native -flto -funroll-loops -fomit-frame-pointer

TARGET = ring_buf_test.out
LIBNAME = ringbuf.a
ARCHIVE = lib$(LIBNAME)
LIBS=-pthread
SRCS = ring_buf_test_int.c
OBJS = $(SRCS:.c=.o)
RING_BUF_OBJ = ring_buf.o

all: $(ARCHIVE) $(TARGET)

# Step 1: Compile ring_buf.c into an object file
$(RING_BUF_OBJ): ring_buf.c ring_buf.h
	$(CC) $(CFLAGS) -c ring_buf.c -o $(RING_BUF_OBJ)

# Step 2: Create the static library (.a)
$(ARCHIVE): $(RING_BUF_OBJ)
	ar rcs $(ARCHIVE) $(RING_BUF_OBJ)

# Step 3: Compile and link the test program with the static library
$(TARGET): $(OBJS) $(ARCHIVE)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(ARCHIVE) $(LIBS)

# Rule for compiling object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up generated files
clean:
	rm -f $(TARGET) $(OBJS) $(RING_BUF_OBJ) $(ARCHIVE)
