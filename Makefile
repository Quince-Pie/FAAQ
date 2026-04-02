CC = gcc
# Enforce strict C23 compliance, maximal architecture optimizations
CFLAGS_BASE = -std=c2x -march=native -Wall -Wextra -Wpedantic
LDFLAGS = -lpthread

# Targets
TARGET_TEST = test_faaq
TARGET_BENCH = bench_faaq

# ASan for rigorous memory leak testing in the Hazard Pointer system
CFLAGS_TEST = $(CFLAGS_BASE) -O1 -fsanitize=address -g3 -fno-omit-frame-pointer
CFLAGS_BENCH = $(CFLAGS_BASE) -O3 -flto -DNDEBUG

SRCS = test_faaq.c faaq.c hp.c

all: test bench

test: $(SRCS)
	$(CC) $(CFLAGS_TEST) $^ -o $(TARGET_TEST) $(LDFLAGS)

bench: $(SRCS)
	$(CC) $(CFLAGS_BENCH) $^ -o $(TARGET_BENCH) $(LDFLAGS)

clean:
	rm -f $(TARGET_TEST) $(TARGET_BENCH)
