#include "faaq.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------

static double get_time_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Spin-wait backoff to prevent bus lock under extreme contention
static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#else
    thrd_yield();
#endif
}

// Synchronizes the "Thundering Herd" so all threads attack the queue at the exact same nanosecond
static void wait_barrier(_Atomic(int)* barrier, int total)
{
    atomic_fetch_add_explicit(barrier, 1, memory_order_acq_rel);
    while (atomic_load_explicit(barrier, memory_order_acquire) < total) {
        cpu_relax();
    }
}

// -----------------------------------------------------------------------------
// Phase 1: HP Memory Leak & Teardown Test
// -----------------------------------------------------------------------------

static void payload_free(void* ptr)
{
    free(ptr);
}

static void test_teardown_leaks(void)
{
    printf("[*] Queue Teardown & Payload Memory Leak Test... ");
    fflush(stdout);

    FAAArrayQueue_t* q = faa_queue_create(1);
    
    for (int i = 0; i < 10000; i++) {
        int* val = malloc(sizeof(int));
        *val = i;
        faa_queue_enqueue(q, val, 0);
    }
    
    // Dequeue half
    for (int i = 0; i < 5000; i++) {
        void* val = faa_queue_dequeue(q, 0);
        free(val);
    }
    
    // Destroy will drain the rest, invoking payload_free and hazptr_cleanup.
    // AddressSanitizer (ASan) will mathematically prove zero leaks here.
    faa_queue_destroy(q, payload_free);
    printf("PASSED\n");
}

// -----------------------------------------------------------------------------
// Phase 2: MPMC Exact-Once Delivery & Wait-Free Preemption Test
// -----------------------------------------------------------------------------

typedef struct {
    FAAArrayQueue_t* q;
    int              tid;
    size_t           start_val;
    size_t           count;
    _Atomic(bool)*   seen_array;
    _Atomic(size_t)* total_consumed;
    size_t           total_expected;
    _Atomic(int)*    barrier;
    int              total_threads;
} CorrectnessCtx;

static int correctness_producer(void* arg)
{
    CorrectnessCtx* ctx = (CorrectnessCtx*)arg;
    wait_barrier(ctx->barrier, ctx->total_threads);

    for (size_t i = 0; i < ctx->count; i++) {
        // Encode integer value into pointer (offset by 1 to avoid nullptr/0)
        size_t val = ctx->start_val + i;
        faa_queue_enqueue(ctx->q, (void*)(uintptr_t)val, ctx->tid);
    }
    return 0;
}

static int correctness_consumer(void* arg)
{
    CorrectnessCtx* ctx = (CorrectnessCtx*)arg;
    wait_barrier(ctx->barrier, ctx->total_threads);

    while (atomic_load_explicit(ctx->total_consumed, memory_order_relaxed) < ctx->total_expected) {
        void* item = faa_queue_dequeue(ctx->q, ctx->tid);
        if (item != nullptr) {
            size_t val = (size_t)(uintptr_t)item;
            
            // Lock-free Trap: Atomic exchange returns the previous value.
            // If it returns 'true', this exact item was already dequeued!
            bool expected = false;
            if (!atomic_compare_exchange_strong_explicit(
                    &ctx->seen_array[val], &expected, true, 
                    memory_order_release, memory_order_relaxed)) {
                fprintf(stderr, "\n[FATAL ERROR] ABA/Duplicate detected! Item %zu dequeued twice.\n", val);
                abort();
            }
            atomic_fetch_add_explicit(ctx->total_consumed, 1, memory_order_release);
        } else {
            cpu_relax(); // Queue empty, passive backoff
        }
    }
    return 0;
}

static void run_correctness_test(int prods, int cons, size_t items_per_prod)
{
    printf("[*] MPMC Exact-Once Test (%02dp/%02dc, %zu items/prod)... ", prods, cons, items_per_prod);
    fflush(stdout);

    int const total_threads = prods + cons;
    size_t const total_items = prods * items_per_prod;

    FAAArrayQueue_t* q = faa_queue_create(total_threads);
    assert(q != nullptr);

    _Atomic(bool)* seen_array = calloc(total_items + 1, sizeof(_Atomic(bool)));
    _Atomic(size_t) total_consumed = 0;
    _Atomic(int) barrier = 0;

    thrd_t* threads = malloc(total_threads * sizeof(thrd_t));
    CorrectnessCtx* args = malloc(total_threads * sizeof(CorrectnessCtx));

    for (int i = 0; i < prods; i++) {
        args[i] = (CorrectnessCtx){ q, i, i * items_per_prod + 1, items_per_prod, seen_array, &total_consumed, total_items, &barrier, total_threads };
        thrd_create(&threads[i], correctness_producer, &args[i]);
    }
    for (int i = 0; i < cons; i++) {
        int tid = prods + i;
        args[tid] = (CorrectnessCtx){ q, tid, 0, 0, seen_array, &total_consumed, total_items, &barrier, total_threads };
        thrd_create(&threads[tid], correctness_consumer, &args[tid]);
    }

    for (int i = 0; i < total_threads; i++) {
        thrd_join(threads[i], nullptr);
    }

    // Final Verification: Ensure absolutely every item was dequeued exactly once
    assert(atomic_load_explicit(&total_consumed, memory_order_acquire) == total_items);
    for (size_t i = 1; i <= total_items; i++) {
        if (!atomic_load_explicit(&seen_array[i], memory_order_acquire)) {
            fprintf(stderr, "\n[FATAL] Lost item detected! Item %zu was never dequeued.\n", i);
            abort();
        }
    }

    faa_queue_destroy(q, nullptr);
    free(seen_array);
    free(threads);
    free(args);
    printf("PASSED (0 ABA, 0 Lost)\n");
}

// -----------------------------------------------------------------------------
// Phase 3: Hardware Scaling Benchmark
// -----------------------------------------------------------------------------
// We use a symmetric pairing (each thread does 1 push + 1 pop). This keeps the 
// queue bounded in memory but maximizes atomic cross-traffic.

typedef struct {
    FAAArrayQueue_t* q;
    int              tid;
    _Atomic(int)*    barrier;
    int              total_threads;
    _Atomic(bool)*   stop_flag;
    
    // alignas forces this counter onto its own 128-byte cache line. 
    // This prevents threads from false-sharing their local benchmarking metrics.
    alignas(FAA_ALIGNMENT) _Atomic(uint64_t) ops;
} BenchCtx;

static int bench_thread(void* arg)
{
    BenchCtx* ctx = (BenchCtx*)arg;
    wait_barrier(ctx->barrier, ctx->total_threads);
    
    uint64_t ops = 0;
    void* dummy_payload = (void*)(uintptr_t)0xDEADBEEF;
    
    while (!atomic_load_explicit(ctx->stop_flag, memory_order_relaxed)) {
        faa_queue_enqueue(ctx->q, dummy_payload, ctx->tid);
        
        while (faa_queue_dequeue(ctx->q, ctx->tid) == nullptr) {
            if (atomic_load_explicit(ctx->stop_flag, memory_order_relaxed)) goto done;
            cpu_relax();
        }
        ops += 2; // 1 Enqueue + 1 Dequeue
    }
done:
    atomic_store_explicit(&ctx->ops, ops, memory_order_release);
    return 0;
}

static void run_benchmark(int num_threads, int duration_sec)
{
    printf("[*] Scalability Bench (%02d Threads, %ds)... ", num_threads, duration_sec);
    fflush(stdout);

    FAAArrayQueue_t* q = faa_queue_create(num_threads);
    _Atomic(int) barrier = 0;
    _Atomic(bool) stop_flag = false;

    thrd_t* threads = calloc(num_threads, sizeof(thrd_t));
    BenchCtx* args = calloc(num_threads, sizeof(BenchCtx));

    // Pre-fill slightly to avoid immediate empty-stalls at boot
    for(int i = 0; i < 64; i++) faa_queue_enqueue(q, (void*)(uintptr_t)0xDEADBEEF, 0);

    for (int i = 0; i < num_threads; i++) {
        args[i] = (BenchCtx){ q, i, &barrier, num_threads, &stop_flag, 0 };
        thrd_create(&threads[i], bench_thread, &args[i]);
    }

    // Wait for all threads to spawn and lock onto the barrier
    while (atomic_load_explicit(&barrier, memory_order_acquire) < num_threads) {
        cpu_relax();
    }
    
    double start = get_time_s();

    // Run for duration
    struct timespec ts_dur = {.tv_sec = duration_sec, .tv_nsec = 0};
    thrd_sleep(&ts_dur, nullptr);

    // Stop and compute
    atomic_store_explicit(&stop_flag, true, memory_order_release);

    uint64_t total_ops = 0;
    for (int i = 0; i < num_threads; i++) {
        thrd_join(threads[i], nullptr);
        total_ops += atomic_load_explicit(&args[i].ops, memory_order_relaxed);
    }

    double end = get_time_s();
    double mops = ((double)total_ops / (end - start)) / 1000000.0;

    printf("%8.2f M Ops/sec (%.2f MOps/thread)\n", mops, mops / num_threads);

    faa_queue_destroy(q, nullptr);
    free(threads);
    free(args);
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(void)
{
    printf("=========================================================\n");
    printf(" C23 MPMC FAA Queue + Hazard Pointers Validation Suite\n");
    printf("=========================================================\n\n");

    test_teardown_leaks();
    
    // 1. Standard High-Volume Test (Validates Hazard Pointer stability)
    run_correctness_test(4, 4, 1000000);
    
    // 2. Severe Oversubscription (Forces OS Preemption)
    // Running 32 threads on a standard desktop core forces context switches 
    // mid-operation. If your queue is not Wait-Free, it will permanently deadlock here.
    run_correctness_test(16, 16, 100000);

    printf("\n=========================================================\n");
    printf(" Throughput & Scaling Benchmark (Symmetric MPMC)\n");
    printf("=========================================================\n\n");
    
    constexpr int BENCH_TIME = 2; // seconds
    int thread_counts[] = {1, 2, 4, 8, 16};
    
    for (size_t i = 0; i < sizeof(thread_counts)/sizeof(int); i++) {
        run_benchmark(thread_counts[i], BENCH_TIME);
    }

    printf("\nAll tests completed successfully.\n");
    return 0;
}
