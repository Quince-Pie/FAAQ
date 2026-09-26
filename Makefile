# C23 FAA array queue: test and benchmark targets. GNU Make 4.4.
#
#   make test        ASan+UBSan build of the validation suite, then run it
#   make tsan        ThreadSanitizer build of the suite, then run it
#   make bench       -O3 -flto benchmark driver (./bench_faaq -h for options)

CC       ?= gcc

STD      := -std=c23
WARN     := -Wall -Wextra -Wpedantic
LDLIBS   := -lpthread

SRCS     := faaq.c hp.c
HDRS     := faaq.h hp.h test_threads.h

SAN      := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

.PHONY: all test tsan bench example clean

all: test_faaq bench_faaq example

# --- correctness ------------------------------------------------------------

test_faaq: test_faaq.c $(SRCS) $(HDRS)
	$(CC) $(STD) $(WARN) -O1 -g3 $(SAN) test_faaq.c $(SRCS) -o $@ $(LDLIBS)

test: test_faaq
	./test_faaq

tsan_test_faaq: test_faaq.c $(SRCS) $(HDRS)
	$(CC) $(STD) $(WARN) -O1 -g -fsanitize=thread test_faaq.c $(SRCS) -o $@ $(LDLIBS)

tsan: tsan_test_faaq
	FAAQ_TEST_NO_BENCH=1 ./tsan_test_faaq

# --- performance -------------------------------------------------------------

bench_faaq: bench_faaq.c $(SRCS) $(HDRS)
	$(CC) $(STD) $(WARN) -O3 -flto -DNDEBUG bench_faaq.c $(SRCS) -o $@ $(LDLIBS)

bench: bench_faaq
	./bench_faaq -s 2 -t 1,2,4,8,16

example: example.c $(SRCS) $(HDRS)
	$(CC) $(STD) $(WARN) -O2 example.c $(SRCS) -o $@ $(LDLIBS)

clean:
	rm -f test_faaq bench_faaq example tsan_test_faaq
