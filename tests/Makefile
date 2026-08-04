# Builds the functional test binaries against the library source directly
# (each in both variants: line format and -DRB_INT_INDEXED).
# Tests that need other flags (sanitizers, RB_WAIT_SPIN, mutants) rebuild
# ring_buf.c themselves inside their NN-*.sh scripts.

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O3 -march=native -flto -funroll-loops -fomit-frame-pointer
LIBS    = -pthread
B       = build
RB      = ../ring_buf.c
TESTS   = 01-func-single 02-func-burst 03-func-wait 06-tsan-modes

BINS    = $(patsubst %,$(B)/%-line,$(TESTS)) $(patsubst %,$(B)/%-idx,$(TESTS))

all: $(BINS)

$(B):
	mkdir -p $(B)

$(B)/%-line: %.c $(RB) ../ring_buf.h | $(B)
	$(CC) $(CFLAGS) -I.. -o $@ $< $(RB) $(LIBS)

$(B)/%-idx: %.c $(RB) ../ring_buf.h | $(B)
	$(CC) $(CFLAGS) -DRB_INT_INDEXED -I.. -o $@ $< $(RB) $(LIBS)

clean:
	rm -rf $(B) logs

.PHONY: all clean
