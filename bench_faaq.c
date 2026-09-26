// Throughput driver for FAAArrayQueue. Compiles against the tid-free API and,
// with -DFAAQ_API_TID, against the previous tid-taking API so that the two can
// be measured with byte-identical driver code.
//
//   bench_faaq [-s seconds] [-t 1,2,4] [-w sym|pc|mix|both|all] [-r reps] [-p] [-i inflight]
//
// Prints CSV: workload,threads,rep,mops
//   sym: every thread alternates enqueue / dequeue (queue stays near-empty;
//        stresses the index counters and the cached head/tail path).
//   pc : half producers, half consumers, producers throttled to 8192 in
//        flight (queue keeps growing/shrinking through nodes; stresses node
//        allocation, hazard-pointer retire and reclamation).
// -p pins thread i to CPU i.
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#include "faaq.h"
#include "test_threads.h"

#ifdef FAAQ_API_TID
#    define Q_CREATE(n) faa_queue_create(n)
#    define Q_ENQ(q, x, tid) faa_queue_enqueue((q), (x), (tid))
#    define Q_DEQ(q, tid) faa_queue_dequeue((q), (tid))
#else
#    define Q_CREATE(n) faa_queue_create()
#    define Q_ENQ(q, x, tid) faa_queue_enqueue((q), (x))
#    define Q_DEQ(q, tid) faa_queue_dequeue((q))
#endif

static inline void cpu_relax(void)
{
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
}

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct
{
    FAAArrayQueue_t*  q;
    int               tid;
    int               nthreads;
    bool              producer; // pc workload only
    bool              pin;
    _Atomic(int)*     barrier;
    _Atomic(bool)*    stop;
    _Atomic(int64_t)* inflight;
    int64_t           inflight_cap; // pc workload: producers pause above this
    alignas(128) uint64_t ops;
    uint64_t              pushes, pops_ok; // mix workload breakdown
} Ctx;

static void pin_self(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void barrier_wait(Ctx* c)
{
    atomic_fetch_add_explicit(c->barrier, 1, memory_order_acq_rel);
    while (atomic_load_explicit(c->barrier, memory_order_acquire) < c->nthreads) {
        cpu_relax();
    }
}

static int sym_thread(void* arg)
{
    Ctx* c = arg;
    if (c->pin)
        pin_self(c->tid);
    int const tid = c->tid;
    (void)tid;
    void* payload = (void*)(uintptr_t)0xDEADBEEF;
    barrier_wait(c);
    uint64_t ops = 0;
    while (!atomic_load_explicit(c->stop, memory_order_relaxed)) {
        Q_ENQ(c->q, payload, tid);
        while (Q_DEQ(c->q, tid) == nullptr) {
            if (atomic_load_explicit(c->stop, memory_order_relaxed))
                goto done;
            cpu_relax();
        }
        ops += 2;
    }
done:
    c->ops = ops;
    return 0;
}

// 50/50 random mix (the classic regime-free MPMC workload): every thread pushes
// or pops with equal probability; only successful operations count.
static int mix_thread(void* arg)
{
    Ctx* c = arg;
    if (c->pin) pin_self(c->tid);
    int const tid = c->tid;
    (void)tid;
    void*    payload = (void*)(uintptr_t)0xDEADBEEF;
    uint64_t rng     = 0x9E3779B97F4A7C15ull * (uint64_t)(c->tid + 1);
    barrier_wait(c);
    uint64_t ops = 0;
    while (!atomic_load_explicit(c->stop, memory_order_relaxed)) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        if (rng & 1) {
            Q_ENQ(c->q, payload, tid);
            c->pushes++; // live, on this thread's own cache line, for the sampler
        } else if (Q_DEQ(c->q, tid) != nullptr) {
            c->pops_ok++;
        }
        ops++; // every operation counts: a pop that reports empty is a complete operation
    }
    c->ops = ops;
    return 0;
}

static int pc_thread(void* arg)
{
    Ctx* c = arg;
    if (c->pin)
        pin_self(c->tid);
    int const tid = c->tid;
    (void)tid;
    void* payload = (void*)(uintptr_t)0xDEADBEEF;
    barrier_wait(c);
    uint64_t ops = 0;
    if (c->producer) {
        while (!atomic_load_explicit(c->stop, memory_order_relaxed)) {
            if (atomic_load_explicit(c->inflight, memory_order_relaxed) > c->inflight_cap) {
                cpu_relax();
                continue;
            }
            Q_ENQ(c->q, payload, tid);
            atomic_fetch_add_explicit(c->inflight, 1, memory_order_relaxed);
            ops++;
        }
    } else {
        while (!atomic_load_explicit(c->stop, memory_order_relaxed)) {
            if (Q_DEQ(c->q, tid) != nullptr) {
                atomic_fetch_sub_explicit(c->inflight, 1, memory_order_relaxed);
                ops++;
            } else {
                cpu_relax();
            }
        }
    }
    c->ops = ops;
    return 0;
}

static double run(char const* workload, int nthreads, double seconds, bool pin, int64_t inflight_cap)
{
    FAAArrayQueue_t* q        = Q_CREATE(nthreads + 1);
    // Each shared control word on its own line: as plain stack variables they
    // share one, and the resulting false sharing differs per build.
    struct {
        alignas(128) _Atomic(int) barrier;
        alignas(128) _Atomic(bool) stop;
        alignas(128) _Atomic(int64_t) inflight;
    }* sh = aligned_alloc(128, 384);
    atomic_init(&sh->barrier, 0);
    atomic_init(&sh->stop, false);
    atomic_init(&sh->inflight, 0);
    thrd_t*          th       = calloc((size_t)nthreads, sizeof *th);
    Ctx*             ctx      = aligned_alloc(128, (size_t)nthreads * sizeof *ctx);
    bool const pc  = strcmp(workload, "pc") == 0;
    bool const mix = strcmp(workload, "mix") == 0;

    for (int i = 0; i < 64; i++)
        Q_ENQ(q, (void*)(uintptr_t)0xDEADBEEF, nthreads);

    for (int i = 0; i < nthreads; i++) {
        ctx[i] = (Ctx){q, i, nthreads, pc && (i % 2 == 0), pin, &sh->barrier, &sh->stop, &sh->inflight, inflight_cap, 0, 0, 0};
        xthrd_create(&th[i], mix ? mix_thread : pc ? pc_thread : sym_thread, &ctx[i]);
    }
    while (atomic_load_explicit(&sh->barrier, memory_order_acquire) < nthreads)
        cpu_relax();

    double const t0 = now_s();
    // Sample the queue occupancy while the run is in progress (BENCH_VERBOSE).
    double inflight_sum = 0;
    int    samples      = 0;
    for (double t = now_s(); t - t0 < seconds; t = now_s()) {
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 5000000};
        thrd_sleep(&tick, nullptr);
        if (mix) {
            int64_t backlog = 0;
            for (int i = 0; i < nthreads; i++) backlog += (int64_t)ctx[i].pushes - (int64_t)ctx[i].pops_ok;
            inflight_sum += (double)backlog;
        } else {
            inflight_sum += (double)atomic_load_explicit(&sh->inflight, memory_order_relaxed);
        }
        samples++;
    }
    atomic_store_explicit(&sh->stop, true, memory_order_release);

    uint64_t total = 0;
    for (int i = 0; i < nthreads; i++) {
        xthrd_join(th[i], nullptr);
        total += ctx[i].ops;
    }
    double const dt = now_s() - t0;
    if (getenv("BENCH_VERBOSE")) {
        fprintf(stderr, "  %s/%d mean inflight %.0f;", workload, nthreads, samples ? inflight_sum / samples : 0.0);
        if (mix) {
            uint64_t pu = 0, po = 0, at = 0;
            for (int i = 0; i < nthreads; i++) {
                pu += ctx[i].pushes;
                po += ctx[i].pops_ok;
                at += ctx[i].ops - ctx[i].pushes;
            }
            fprintf(stderr, " pushes %.1fM pop-attempts %.1fM pop-hits %.1fM (%.0f%% empty);", pu / 1e6, at / 1e6, po / 1e6, at ? 100.0 * (double)(at - po) / (double)at : 0.0);
        }
        fprintf(stderr, "  %s/%d per-thread Mops:", workload, nthreads);
        for (int i = 0; i < nthreads; i++)
            fprintf(stderr, " %.1f", (double)ctx[i].ops / dt / 1e6);
        fputc('\n', stderr);
    }

    void* item;
    while ((item = Q_DEQ(q, nthreads)) != nullptr) {
    }
    faa_queue_destroy(q, nullptr);
    free(th);
    free(ctx);
    free(sh);
    return (double)total / dt / 1e6;
}

int main(int argc, char** argv)
{
    double seconds      = 2.0;
    int    reps         = 1;
    bool   pin          = false;
    int64_t inflight_cap = 8192;
    char   threads[256] = "1,2,4,8,16";
    char   workload[8]  = "both";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            snprintf(threads, sizeof threads, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc)
            snprintf(workload, sizeof workload, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p"))
            pin = true;
        else if (!strcmp(argv[i], "-i") && i + 1 < argc)
            inflight_cap = atoll(argv[++i]);
        else {
            fprintf(
                stderr, "usage: %s [-s sec] [-t 1,2,4] [-w sym|pc|mix|both|all] [-r reps] [-p] [-i inflight]\n", argv[0]);
            return 2;
        }
    }

    printf("workload,threads,rep,mops\n");
    for (int r = 0; r < reps; r++) {
        char* list = strdup(threads);
        for (char* tok = strtok(list, ","); tok; tok = strtok(nullptr, ",")) {
            int const n = atoi(tok);
            if (n <= 0)
                continue;
            bool const do_sym = !strcmp(workload, "sym") || !strcmp(workload, "both") || !strcmp(workload, "all");
            bool const do_pc  = !strcmp(workload, "pc") || !strcmp(workload, "both") || !strcmp(workload, "all");
            bool const do_mix = !strcmp(workload, "mix") || !strcmp(workload, "all");
            if (do_sym) {
                printf("sym,%d,%d,%.2f\n", n, r, run("sym", n, seconds, pin, inflight_cap));
                fflush(stdout);
            }
            if (do_pc && n >= 2) {
                printf("pc,%d,%d,%.2f\n", n, r, run("pc", n, seconds, pin, inflight_cap));
                fflush(stdout);
            }
            if (do_mix) {
                printf("mix,%d,%d,%.2f\n", n, r, run("mix", n, seconds, pin, inflight_cap));
                fflush(stdout);
            }
        }
        free(list);
    }
    return 0;
}
