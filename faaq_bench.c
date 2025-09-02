#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <unistd.h>

#include "faaq.h"

static constexpr int      NUM_PRODUCERS      = 8;
static constexpr int      NUM_CONSUMERS      = 8;
static constexpr uint64_t TOTAL_ITEMS        = 20000000;

static constexpr int      TOTAL_THREADS      = NUM_PRODUCERS + NUM_CONSUMERS;
static constexpr uint64_t ITEMS_PER_PRODUCER = TOTAL_ITEMS / NUM_PRODUCERS;

#define USE_AFFINITY 1

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif
inline static uint64_t
rdtsc_serialized(void) {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int aux;
    return __rdtscp(&aux);
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t val;
    __asm__ volatile("dmb ish" : : : "memory");
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
#warning "Serialized RDTSC benchmarking not supported on this architecture."
    return 0;
#endif
}

#define RDTSC() rdtsc_serialized()

static FAAArrayQueue_t *g_queue                           = nullptr;
alignas(128) static atomic_uint_fast64_t g_dequeued_count = 0;

static uint64_t             g_start_cycles                = 0;
static atomic_uint_fast64_t g_end_cycles                  = 0;

static mtx_t                g_barrier_mutex;
static cnd_t                g_barrier_cond;
static int                  g_barrier_count  = 0;
static int                  g_barrier_target = 0;

void
barrier_wait() {
    mtx_lock(&g_barrier_mutex);
    g_barrier_count++;
    if (g_barrier_count == g_barrier_target) {
        cnd_broadcast(&g_barrier_cond);
    } else {
        cnd_wait(&g_barrier_cond, &g_barrier_mutex);
    }
    mtx_unlock(&g_barrier_mutex);
}

void
set_affinity(int tid) {
#if USE_AFFINITY
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    // sysconf is a non-standard POSIX function
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores <= 0) {
        return;
    }

    int core_id = tid % num_cores;
    CPU_SET(core_id, &cpuset);

    // sched_setaffinity is a non-standard POSIX function
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
        // Not fatal, but might affect results stability
    }
#endif
}

__attribute__((noinline)) uint64_t
black_box(uint64_t dummy) {
    uint64_t volatile ret = dummy;
    return ret;
}

int
producer_func(void *arg) {
    int tid = (int) (uintptr_t) arg;
    set_affinity(tid);

    barrier_wait();

    void *payload = (void *) (uintptr_t) 1;

    if (payload == g_queue->taken_sentinel) {
        payload = (void *) (uintptr_t) 2;
    }

    for (uint64_t i = 0; i < ITEMS_PER_PRODUCER; ++i) {
        faa_queue_enqueue(g_queue, payload, tid);
    }

    return 0;
}

int
consumer_func(void *arg) {
    int tid = (int) (uintptr_t) arg;
    set_affinity(tid);

    barrier_wait();

    while (true) {
        void *item = faa_queue_dequeue(g_queue, tid);

        if (item != nullptr) {
            uint64_t prev_count = atomic_fetch_add_explicit(&g_dequeued_count, 1, memory_order_seq_cst);

            if (prev_count + 1 == TOTAL_ITEMS) {
                uint64_t cycles = RDTSC();
                atomic_store_explicit(&g_end_cycles, cycles, memory_order_seq_cst);
                break;
            }
        } else {
            if (atomic_load_explicit(&g_dequeued_count, memory_order_seq_cst) >= TOTAL_ITEMS) {
                break;
            }
            thrd_yield();
        }
    }

    return 0;
}

void
run_benchmark() {
    printf("--- FAA Array Queue Throughput Benchmark ---\n");
    printf("Producers: %d, Consumers: %d\n", NUM_PRODUCERS, NUM_CONSUMERS);
    printf("Total Items: %w64u\n", TOTAL_ITEMS);
    printf("FAA Buffer Size: %zu\n", FAA_BUFFER_SIZE);
    printf("CPU Affinity: %s\n", USE_AFFINITY ? "Enabled" : "Disabled");

    g_barrier_target = TOTAL_THREADS + 1;
    g_barrier_count  = 0;
    if (mtx_init(&g_barrier_mutex, mtx_plain) != thrd_success || cnd_init(&g_barrier_cond) != thrd_success) {
        fprintf(stderr, "Failed to initialize mutex/condvar.\n");
        exit(EXIT_FAILURE);
    }

    g_queue = faa_queue_create(TOTAL_THREADS);
    if (!g_queue) {
        fprintf(stderr, "Failed to create FAA Array Queue.\n");
        exit(EXIT_FAILURE);
    }

    thrd_t threads[TOTAL_THREADS];
    printf("Spawning threads...\n");

    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        int tid = NUM_PRODUCERS + i;
        if (thrd_create(&threads[tid], consumer_func, (void *) (uintptr_t) tid) != thrd_success) {
            // FIX: thrd_create does not set errno, so do not use perror.
            fprintf(stderr, "Failed to create consumer thread %d.\n", tid);
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        int tid = i;
        if (thrd_create(&threads[tid], producer_func, (void *) (uintptr_t) tid) != thrd_success) {
            // FIX: thrd_create does not set errno, so do not use perror.
            fprintf(stderr, "Failed to create producer thread %d.\n", tid);
            exit(EXIT_FAILURE);
        }
    }

    printf("Synchronizing start...\n");
    barrier_wait();
    g_start_cycles = RDTSC();
    printf("Running benchmark...\n");

    for (int i = 0; i < TOTAL_THREADS; ++i) {
        thrd_join(threads[i], nullptr);
    }

    uint64_t end_cycles_val = atomic_load_explicit(&g_end_cycles, memory_order_seq_cst);

    printf("Benchmark finished.\n");

    if (end_cycles_val == 0 || g_start_cycles == 0) {
        fprintf(stderr, "Error: Timing data was not captured correctly.\n");
        exit(EXIT_FAILURE);
    }
    if (atomic_load(&g_dequeued_count) != TOTAL_ITEMS) {
        fprintf(stderr, "Error: Count mismatch.\n");
        exit(EXIT_FAILURE);
    }

    uint64_t total_cycles    = end_cycles_val - g_start_cycles;
    double   cycles_per_op   = (double) total_cycles / (TOTAL_ITEMS * 2);
    double   cycles_per_pair = (double) total_cycles / TOTAL_ITEMS;

    printf("\n--- Results ---\n");
    printf("Total Cycles: %w64u\n", total_cycles);
    printf("Cycles per operation (E or D): %.2f\n", cycles_per_op);
    printf("Cycles per pair (E+D):         %.2f\n", cycles_per_pair);

    faa_queue_destroy(g_queue);
    mtx_destroy(&g_barrier_mutex);
    cnd_destroy(&g_barrier_cond);
}

int
main(void) {
    printf("Warming up...\n");
    uint64_t warmup_start = RDTSC();
    uint64_t dummy        = 0;
    for (int i = 0; i < 100000000; i++) {
        dummy = black_box(dummy + i);
    }
    uint64_t warmup_end = RDTSC();
    printf("Warmup finished in %w64u cycles.\n\n", (warmup_end - warmup_start));

    run_benchmark();
    return EXIT_SUCCESS;
}
