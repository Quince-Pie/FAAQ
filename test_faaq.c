#include "faaq.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

#include "test_threads.h"
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

// Integers travel through the queue as tagged pointers (never nullptr).
static inline void* tag(uint64_t v)
{
    return (void*)(uintptr_t)((v << 1) | 1);
}

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "\n[FATAL] %s:%d: ", __FILE__, __LINE__);                              \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

// ThreadSanitizer serializes every atomic through its own locks, and 32 threads
// hammering two counters turn that into a convoy; race detection does not need
// millions of items, so TSan builds shrink the concurrent phases (1/20, and
// 1/100 for the oversubscribed one).
#if defined(__SANITIZE_THREAD__)
#define FAAQ_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FAAQ_TEST_TSAN 1
#endif
#endif
#ifdef FAAQ_TEST_TSAN
constexpr static size_t TEST_SCALE         = 20;
constexpr static size_t TEST_SCALE_OVERSUB = 100;
constexpr static int    OVERSUB_SIDE       = 8; // 16+16 pollers convoy on TSan's per-address locks
#else
constexpr static size_t TEST_SCALE         = 1;
constexpr static size_t TEST_SCALE_OVERSUB = 1;
constexpr static int    OVERSUB_SIDE       = 16;
#endif

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

    FAAArrayQueue_t* q = faa_queue_create();
    CHECK(q != nullptr, "create failed");

    for (int i = 0; i < 10000; i++) {
        int* val = malloc(sizeof(int));
        *val     = i;
        faa_queue_enqueue(q, val);
    }

    // Dequeue half, in order.
    for (int i = 0; i < 5000; i++) {
        int* val = faa_queue_dequeue(q);
        CHECK(val != nullptr && *val == i, "FIFO order broken at %d", i);
        free(val);
    }

    // Destroy drains the rest through payload_free. LeakSanitizer proves the
    // rest: nodes retired while draining are reclaimed by hazptr_cleanup().
    faa_queue_destroy(q, payload_free);
    printf("PASSED\n");
}

// -----------------------------------------------------------------------------
// Phase 1b: Stale cached head regression (deterministic)
//
// A thread's cached head node can be drained and unlinked by *other* threads
// while the thread is away. When it comes back, its dequeue must walk to the
// live head instead of reporting "empty" (which it did permanently before the
// `next == nullptr` guard was restored in the empty check).
// -----------------------------------------------------------------------------

typedef struct {
    FAAArrayQueue_t* q;
    uint64_t         start;
    uint64_t         count;
} SeqCtx;

static int seq_producer(void* arg)
{
    SeqCtx* c = arg;
    for (uint64_t i = 0; i < c->count; i++) {
        faa_queue_enqueue(c->q, tag(c->start + i));
    }
    return 0;
}

static int seq_consumer(void* arg)
{
    SeqCtx* c = arg;
    for (uint64_t got = 0; got < c->count;) {
        void* item = faa_queue_dequeue(c->q);
        if (item) {
            CHECK(item == tag(c->start + got), "consumer FIFO order broken");
            got++;
        } else {
            cpu_relax();
        }
    }
    return 0;
}

static void test_stale_head_regression(void)
{
    printf("[*] Stale Cached Head Regression Test... ");
    fflush(stdout);

    constexpr uint64_t FILL  = 6 * FAA_BUFFER_SIZE; // several nodes
    constexpr uint64_t OTHER = 3 * FAA_BUFFER_SIZE; // moves the head 3 nodes on

    FAAArrayQueue_t* q = faa_queue_create();
    CHECK(q != nullptr, "create failed");

    // This thread caches the first node as its head and tail.
    faa_queue_enqueue(q, tag(1));
    CHECK(faa_queue_dequeue(q) == tag(1), "warm-up dequeue");

    thrd_t t;
    SeqCtx fill = { q, 2, FILL };
    CHECK(xthrd_create(&t, seq_producer, &fill) == thrd_success, "thrd_create");
    xthrd_join(t, nullptr);

    SeqCtx other = { q, 2, OTHER };
    CHECK(xthrd_create(&t, seq_consumer, &other) == thrd_success, "thrd_create");
    xthrd_join(t, nullptr); // its exit releases its hazard pointers (tss destructor)

    // Alone now, with a cached head that is several retired nodes behind.
    for (uint64_t i = 0; i < FILL - OTHER; i++) {
        void* item = faa_queue_dequeue(q);
        CHECK(item == tag(2 + OTHER + i), "dequeue %llu returned %p (spurious empty?)",
              (unsigned long long)i, item);
    }
    CHECK(faa_queue_dequeue(q) == nullptr, "queue should be empty");

    // Same for a stale cached tail: enqueue must walk to the live tail.
    faa_queue_enqueue(q, tag(1));
    CHECK(faa_queue_dequeue(q) == tag(1), "tail warm-up");
    faa_queue_destroy(q, nullptr);
    printf("PASSED\n");
}

// -----------------------------------------------------------------------------
// Phase 1c: Many queues per thread (thread-local slot eviction)
// -----------------------------------------------------------------------------

static void test_multi_queue_single_thread(void)
{
    printf("[*] Multi-Queue Per Thread (slot eviction) Test... ");
    fflush(stdout);

    constexpr int    NQ     = 6; // more than the per-thread slot cache
    constexpr uint64_t ROUNDS = 3 * FAA_BUFFER_SIZE;
    FAAArrayQueue_t* qs[NQ];
    for (int i = 0; i < NQ; i++) {
        qs[i] = faa_queue_create();
        CHECK(qs[i] != nullptr, "create failed");
    }
    for (uint64_t r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < NQ; i++) {
            faa_queue_enqueue(qs[i], tag(r * NQ + (uint64_t)i + 1));
        }
    }
    for (uint64_t r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < NQ; i++) {
            void* item = faa_queue_dequeue(qs[i]);
            CHECK(item == tag(r * NQ + (uint64_t)i + 1), "queue %d FIFO broken at round %llu", i,
                  (unsigned long long)r);
        }
    }
    for (int i = 0; i < NQ; i++) {
        CHECK(faa_queue_dequeue(qs[i]) == nullptr, "queue %d not empty", i);
        faa_queue_destroy(qs[i], nullptr);
    }
    printf("PASSED\n");
}

// -----------------------------------------------------------------------------
// Phase 1d: Thread churn (per-thread resources are recycled at thread exit)
// -----------------------------------------------------------------------------

static int churn_thread(void* arg)
{
    SeqCtx* c = arg;
    for (uint64_t i = 0; i < c->count; i++) {
        faa_queue_enqueue(c->q, tag(c->start + i));
    }
    for (uint64_t i = 0; i < c->count / 2; i++) {
        CHECK(faa_queue_dequeue(c->q) == tag(c->start + i), "churn FIFO broken");
    }
    return 0;
}

static void test_thread_churn(void)
{
    printf("[*] Thread Churn (HP record recycling) Test... ");
    fflush(stdout);

    constexpr int    ROUNDS = 300;
    constexpr uint64_t N      = 2 * FAA_BUFFER_SIZE + 3;
    FAAArrayQueue_t* q      = faa_queue_create();
    CHECK(q != nullptr, "create failed");

    size_t const before = hazptr_record_count();
    for (int r = 0; r < ROUNDS; r++) {
        thrd_t t;
        SeqCtx c = { q, (uint64_t)r * N + 1, N };
        CHECK(xthrd_create(&t, churn_thread, &c) == thrd_success, "thrd_create");
        xthrd_join(t, nullptr);
        for (uint64_t i = N / 2; i < N; i++) {
            CHECK(faa_queue_dequeue(q) == tag(c.start + i), "main FIFO broken");
        }
        CHECK(faa_queue_dequeue(q) == nullptr, "queue should be empty");
    }
    size_t const after = hazptr_record_count();
    // Each thread needs two records; exited threads' records are reused, so
    // the total must stay bounded by a few concurrent users, not by ROUNDS.
    CHECK(after <= before + 8, "HP records leaked across thread exits: %zu -> %zu", before, after);

    faa_queue_destroy(q, nullptr);
    printf("PASSED (%zu HP records for %d threads)\n", after, ROUNDS + 1);
}

// -----------------------------------------------------------------------------
// Phase 2: MPMC Exact-Once Delivery & Wait-Free Preemption Test
// -----------------------------------------------------------------------------

typedef struct {
    FAAArrayQueue_t* q;
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
        faa_queue_enqueue(ctx->q, (void*)(uintptr_t)val);
    }
    return 0;
}

static int correctness_consumer(void* arg)
{
    CorrectnessCtx* ctx = (CorrectnessCtx*)arg;
    wait_barrier(ctx->barrier, ctx->total_threads);

    while (atomic_load_explicit(ctx->total_consumed, memory_order_relaxed) < ctx->total_expected) {
        void* item = faa_queue_dequeue(ctx->q);
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
        } else if (TEST_SCALE > 1) {
            thrd_yield(); // TSan: polling starves the producers' RMWs
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

    FAAArrayQueue_t* q = faa_queue_create();
    assert(q != nullptr);

    _Atomic(bool)* seen_array = calloc(total_items + 1, sizeof(_Atomic(bool)));
    _Atomic(size_t) total_consumed = 0;
    _Atomic(int) barrier = 0;

    thrd_t* threads = malloc(total_threads * sizeof(thrd_t));
    CorrectnessCtx* args = malloc(total_threads * sizeof(CorrectnessCtx));

    for (int i = 0; i < prods; i++) {
        args[i] = (CorrectnessCtx){ q, i * items_per_prod + 1, items_per_prod, seen_array, &total_consumed, total_items, &barrier, total_threads };
        xthrd_create(&threads[i], correctness_producer, &args[i]);
    }
    for (int i = 0; i < cons; i++) {
        int t = prods + i;
        args[t] = (CorrectnessCtx){ q, 0, 0, seen_array, &total_consumed, total_items, &barrier, total_threads };
        xthrd_create(&threads[t], correctness_consumer, &args[t]);
    }

    for (int i = 0; i < total_threads; i++) {
        xthrd_join(threads[i], nullptr);
    }

    // Final Verification: Ensure absolutely every item was dequeued exactly once
    assert(atomic_load_explicit(&total_consumed, memory_order_acquire) == total_items);
    for (size_t i = 1; i <= total_items; i++) {
        if (!atomic_load_explicit(&seen_array[i], memory_order_acquire)) {
            fprintf(stderr, "\n[FATAL] Lost item detected! Item %zu was never dequeued.\n", i);
            abort();
        }
    }
    CHECK(faa_queue_dequeue(q) == nullptr, "queue not empty after all items consumed");

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
        faa_queue_enqueue(ctx->q, dummy_payload);

        while (faa_queue_dequeue(ctx->q) == nullptr) {
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

    FAAArrayQueue_t* q = faa_queue_create();
    _Atomic(int) barrier = 0;
    _Atomic(bool) stop_flag = false;

    thrd_t* threads = calloc(num_threads, sizeof(thrd_t));
    BenchCtx* args = calloc(num_threads, sizeof(BenchCtx));

    // Pre-fill slightly to avoid immediate empty-stalls at boot
    for(int i = 0; i < 64; i++) faa_queue_enqueue(q, (void*)(uintptr_t)0xDEADBEEF);

    for (int i = 0; i < num_threads; i++) {
        args[i] = (BenchCtx){ q, &barrier, num_threads, &stop_flag, 0 };
        xthrd_create(&threads[i], bench_thread, &args[i]);
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
        xthrd_join(threads[i], nullptr);
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
    test_stale_head_regression();
    test_multi_queue_single_thread();
    test_thread_churn();

    // 1. Standard High-Volume Test (Validates Hazard Pointer stability)
    run_correctness_test(4, 4, 1000000 / TEST_SCALE);

    // 2. Severe Oversubscription (Forces OS Preemption)
    // Running 32 threads on a standard desktop core forces context switches
    // mid-operation. If your queue is not Wait-Free, it will permanently deadlock here.
    run_correctness_test(OVERSUB_SIDE, OVERSUB_SIDE, 100000 / TEST_SCALE_OVERSUB);

    if (getenv("FAAQ_TEST_NO_BENCH")) {
        printf("\nAll tests completed successfully.\n");
        return 0;
    }

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
