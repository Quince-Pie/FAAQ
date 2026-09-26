# C23 FAA array queue: test, benchmark and fuzz targets. GNU Make 4.4.
#
#   make test        ASan+UBSan build of the validation suite, then run it
#   make tsan        ThreadSanitizer build of the suite + fuzz smoke, then run
#   make bench       -O3 -flto benchmark driver (./bench_faaq -h for options)
#   make fuzz-smoke  PRNG-driven run of the model-based harness under ASan+UBSan
#                    (no fuzzing engine needed; gcc is enough)
#   make fuzz-afl    AFL++ build of the harness (afl-clang-lto: collision-free
#                    coverage, CmpLog) and a campaign into fuzz/out
#   make fuzz-libfuzzer
#                    libFuzzer build of the same harness (needs clang: the
#                    flake's `clang` shell), and a run
#
# The fuzz builds shrink the tunables so that node boundaries, hazard-pointer
# scans, slot poisoning, per-thread slot eviction and real free()s all happen
# every few operations.

CC       ?= gcc
# Not AFL_CC: afl-cc reads that environment variable to pick its backend compiler,
# and make exports command-line variables, so AFL_CC=afl-clang-lto recurses.
AFLCC    ?= afl-clang-lto
CLANG    ?= clang

STD      := -std=c23
WARN     := -Wall -Wextra -Wpedantic
LDLIBS   := -lpthread

SRCS     := faaq.c hp.c
HDRS     := faaq.h hp.h test_threads.h

SAN      := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
FUZZ_CFG := -DFAA_BUFFER_SIZE_CFG=4 -DHP_LOCAL_SCAN_INTERVAL=8 -DFAAQ_SPIN_ITEM=0 -DFAAQ_SPIN_NEXT=0 \
            -DFAAQ_SPEC_CLAIM=1 \
            -DFAAQ_TLS_SLOTS=2 -DFAAQ_NODE_CACHE_CAPACITY=0

FUZZ_IN  ?= fuzz/seeds
FUZZ_OUT ?= fuzz/out
FUZZ_SEC ?= 600

.PHONY: all test tsan bench example fuzz-smoke fuzz-afl fuzz-libfuzzer clean

all: test_faaq bench_faaq example

# --- correctness ------------------------------------------------------------

test_faaq: test_faaq.c $(SRCS) $(HDRS)
	$(CC) $(STD) $(WARN) -O1 -g3 $(SAN) test_faaq.c $(SRCS) -o $@ $(LDLIBS)

test: test_faaq
	./test_faaq

tsan_test_faaq: test_faaq.c $(SRCS) $(HDRS)
	$(CC) $(STD) $(WARN) -O1 -g -fsanitize=thread test_faaq.c $(SRCS) -o $@ $(LDLIBS)

tsan_fuzz_faaq: fuzz_faaq.c $(SRCS) $(HDRS)
	$(CC) $(STD) $(WARN) -O1 -g -fsanitize=thread $(FUZZ_CFG) -DFAAQ_FUZZ_MAIN fuzz_faaq.c $(SRCS) -o $@ $(LDLIBS)

tsan: tsan_test_faaq tsan_fuzz_faaq
	FAAQ_TEST_NO_BENCH=1 ./tsan_test_faaq
	FAAQ_FUZZ_ITERS=200 ./tsan_fuzz_faaq

# --- performance -------------------------------------------------------------

bench_faaq: bench_faaq.c $(SRCS) $(HDRS)
	$(CC) $(STD) $(WARN) -O3 -flto -DNDEBUG bench_faaq.c $(SRCS) -o $@ $(LDLIBS)

bench: bench_faaq
	./bench_faaq -s 2 -t 1,2,4,8,16

example: example.c $(SRCS) $(HDRS)
	$(CC) $(STD) $(WARN) -O2 example.c $(SRCS) -o $@ $(LDLIBS)

# --- fuzzing ------------------------------------------------------------------

fuzz_faaq_smoke: fuzz_faaq.c $(SRCS) $(HDRS)
	$(CC) $(STD) $(WARN) -O1 -g3 $(SAN) $(FUZZ_CFG) -DFAAQ_FUZZ_MAIN fuzz_faaq.c $(SRCS) -o $@ $(LDLIBS)

fuzz-smoke: fuzz_faaq_smoke
	FAAQ_FUZZ_ITERS=$(or $(ITERS),500) ./fuzz_faaq_smoke

# libAFLDriver supplies main(); the sanitizers are named explicitly rather than
# through AFL_USE_ASAN/AFL_USE_UBSAN so the build does not depend on the environment.
fuzz_faaq_afl: fuzz_faaq.c $(SRCS) $(HDRS)
	$(AFLCC) $(STD) -O2 -g $(FUZZ_CFG) -fsanitize=fuzzer,address,undefined fuzz_faaq.c $(SRCS) -o $@ $(LDLIBS)

# One primary instance; add secondaries by hand for more cores:
#   afl-fuzz -S s1 -i $(FUZZ_IN) -o $(FUZZ_OUT) -- ./fuzz_faaq_afl
fuzz-afl: fuzz_faaq_afl
	AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 AFL_NO_UI=1 \
	afl-fuzz -M main -V $(FUZZ_SEC) -t 20000 -m none -i $(FUZZ_IN) -o $(FUZZ_OUT) -- ./fuzz_faaq_afl

fuzz_faaq_libfuzzer: fuzz_faaq.c $(SRCS) $(HDRS)
	$(CLANG) $(STD) $(WARN) -O1 -g $(FUZZ_CFG) -fsanitize=fuzzer,address,undefined fuzz_faaq.c $(SRCS) -o $@ $(LDLIBS)

fuzz-libfuzzer: fuzz_faaq_libfuzzer
	mkdir -p fuzz/corpus
	./fuzz_faaq_libfuzzer -max_len=2048 -timeout=20 -max_total_time=$(FUZZ_SEC) fuzz/corpus $(FUZZ_IN)

clean:
	rm -f test_faaq bench_faaq example tsan_test_faaq tsan_fuzz_faaq fuzz_faaq_smoke fuzz_faaq_afl fuzz_faaq_libfuzzer
