// Mirror of bench_faaq.c for xenium::ramalhete_queue (FAAArrayQueue in C++).
// Same workloads, barrier, pinning, isolated control words, pre-fill, drain and
// CSV output; the only difference is the queue under test. Following xenium's
// own benchmark harness, every batch of 100 operations runs inside a
// reclaimer::region_guard (a no-op for hazard pointers and EBR/DEBRA, the
// critical region for NEBR, the non-quiescent span for QSBR).
//
//   bench_xenium -q hp|ebr|nebr|debra|qsbr [-s sec] [-t 1,2,4] [-w sym|pc|mix|both|all] [-r reps] [-p] [-i inflight]
//
// Build (needs a checkout of https://github.com/mpoeter/xenium):
//   make bench_xenium XENIUM_DIR=/path/to/xenium
#include <xenium/policy.hpp>
#include <xenium/ramalhete_queue.hpp>
#include <xenium/reclamation/generic_epoch_based.hpp>
#include <xenium/reclamation/hazard_pointer.hpp>
#include <xenium/reclamation/quiescent_state_based.hpp>

#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

namespace xr = xenium::reclamation;
using hp_t    = xr::hazard_pointer<>;
using ebr_t   = xr::generic_epoch_based<>::with<xenium::policy::scan<xr::scan::all_threads>,
                                                 xenium::policy::region_extension<xr::region_extension::none>>;
using nebr_t  = xr::generic_epoch_based<>::with<xenium::policy::scan<xr::scan::all_threads>,
                                                 xenium::policy::region_extension<xr::region_extension::eager>>;
using debra_t = xr::generic_epoch_based<>::with<xenium::policy::scan<xr::scan::one_thread>,
                                                 xenium::policy::region_extension<xr::region_extension::none>>;
using qsbr_t  = xr::quiescent_state_based;

static constexpr int BATCH = 100; // operations per region_guard, as in xenium's harness

static inline void cpu_relax() {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
}

static double now_s() {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int g_dummy;

struct Shared {
    alignas(128) std::atomic<int> barrier{0};
    alignas(128) std::atomic<bool> stop{false};
    alignas(128) std::atomic<int64_t> inflight{0};
};

template <class Q>
struct Ctx {
    Q* q;
    int tid;
    int nthreads;
    bool producer;
    bool pin;
    Shared* sh;
    int64_t inflight_cap;
    alignas(128) uint64_t ops = 0;
    uint64_t pushes = 0, pops_ok = 0;
};

static void pin_self(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

template <class Q>
static void barrier_wait(Ctx<Q>* c) {
    c->sh->barrier.fetch_add(1, std::memory_order_acq_rel);
    while (c->sh->barrier.load(std::memory_order_acquire) < c->nthreads) cpu_relax();
}

template <class Q>
static void sym_thread(Ctx<Q>* c) {
    if (c->pin) pin_self(c->tid);
    int* payload = &g_dummy;
    barrier_wait(c);
    uint64_t ops = 0;
    while (!c->sh->stop.load(std::memory_order_relaxed)) {
        [[maybe_unused]] typename Q::reclaimer::region_guard rg;
        for (int k = 0; k < BATCH; k++) {
            c->q->push(payload);
            int* v;
            while (!c->q->try_pop(v)) {
                if (c->sh->stop.load(std::memory_order_relaxed)) goto done;
                cpu_relax();
            }
            ops += 2;
        }
    }
done:
    c->ops = ops;
}

// 50/50 random mix (the classic regime-free MPMC workload): every thread pushes
// or pops with equal probability; only successful operations count.
template <class Q>
static void mix_thread(Ctx<Q>* c) {
    if (c->pin) pin_self(c->tid);
    int* payload = &g_dummy;
    uint64_t rng = 0x9E3779B97F4A7C15ull * (uint64_t)(c->tid + 1);
    barrier_wait(c);
    uint64_t ops = 0;
    while (!c->sh->stop.load(std::memory_order_relaxed)) {
        [[maybe_unused]] typename Q::reclaimer::region_guard rg;
        for (int k = 0; k < BATCH; k++) {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            if (rng & 1) {
                c->q->push(payload);
                c->pushes++;
            } else {
                int* v;
                if (c->q->try_pop(v)) c->pops_ok++;
            }
            ops++; // every operation counts: a pop that reports empty is a complete operation
        }
    }
    c->ops = ops;

}

template <class Q>
static void pc_thread(Ctx<Q>* c) {
    if (c->pin) pin_self(c->tid);
    int* payload = &g_dummy;
    barrier_wait(c);
    uint64_t ops = 0;
    if (c->producer) {
        while (!c->sh->stop.load(std::memory_order_relaxed)) {
            [[maybe_unused]] typename Q::reclaimer::region_guard rg;
            for (int k = 0; k < BATCH; k++) {
                if (c->sh->inflight.load(std::memory_order_relaxed) > c->inflight_cap) {
                    cpu_relax();
                    continue;
                }
                c->q->push(payload);
                c->sh->inflight.fetch_add(1, std::memory_order_relaxed);
                ops++;
            }
        }
    } else {
        while (!c->sh->stop.load(std::memory_order_relaxed)) {
            [[maybe_unused]] typename Q::reclaimer::region_guard rg;
            for (int k = 0; k < BATCH; k++) {
                int* v;
                if (c->q->try_pop(v)) {
                    c->sh->inflight.fetch_sub(1, std::memory_order_relaxed);
                    ops++;
                } else {
                    cpu_relax();
                }
            }
        }
    }
    c->ops = ops;
}

template <class Q>
static double run(char const* workload, int nthreads, double seconds, bool pin, int64_t inflight_cap) {
    Q* q = new Q();
    Shared* sh = new Shared();
    bool const pc = strcmp(workload, "pc") == 0;
    bool const mix = strcmp(workload, "mix") == 0;
    std::vector<Ctx<Q>> ctx(nthreads);
    std::vector<std::thread> th;

    for (int i = 0; i < 64; i++) q->push(&g_dummy);

    for (int i = 0; i < nthreads; i++) {
        ctx[i] = Ctx<Q>{q, i, nthreads, pc && (i % 2 == 0), pin, sh, inflight_cap};
    }
    for (int i = 0; i < nthreads; i++) th.emplace_back(mix ? mix_thread<Q> : pc ? pc_thread<Q> : sym_thread<Q>, &ctx[i]);
    while (sh->barrier.load(std::memory_order_acquire) < nthreads) cpu_relax();

    double const t0 = now_s();
    double inflight_sum = 0;
    int samples = 0;
    for (double t = now_s(); t - t0 < seconds; t = now_s()) {
        struct timespec tick = {0, 5000000};
        nanosleep(&tick, nullptr);
        if (mix) {
            int64_t backlog = 0;
            for (int i = 0; i < nthreads; i++) backlog += (int64_t)ctx[i].pushes - (int64_t)ctx[i].pops_ok;
            inflight_sum += (double)backlog;
        } else {
            inflight_sum += (double)sh->inflight.load(std::memory_order_relaxed);
        }
        samples++;
    }
    sh->stop.store(true, std::memory_order_release);

    uint64_t total = 0;
    for (int i = 0; i < nthreads; i++) {
        th[i].join();
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
        for (int i = 0; i < nthreads; i++) fprintf(stderr, " %.1f", (double)ctx[i].ops / dt / 1e6);
        fputc('\n', stderr);
    }
    int* v;
    while (q->try_pop(v)) {}
    delete q;
    delete sh;
    return (double)total / dt / 1e6;
}

template <class R>
static int run_all(char const* threads, char const* workload, double seconds, int reps, bool pin, int64_t cap) {
    using Q = xenium::ramalhete_queue<int*, xenium::policy::reclaimer<R>, xenium::policy::entries_per_node<512>>;
    printf("workload,threads,rep,mops\n");
    for (int r = 0; r < reps; r++) {
        std::string list(threads);
        for (char* tok = strtok(list.data(), ","); tok; tok = strtok(nullptr, ",")) {
            int const n = atoi(tok);
            if (n <= 0) continue;
            bool const do_sym = !strcmp(workload, "sym") || !strcmp(workload, "both") || !strcmp(workload, "all");
            bool const do_pc = !strcmp(workload, "pc") || !strcmp(workload, "both") || !strcmp(workload, "all");
            bool const do_mix = !strcmp(workload, "mix") || !strcmp(workload, "all");
            if (do_sym) {
                printf("sym,%d,%d,%.2f\n", n, r, run<Q>("sym", n, seconds, pin, cap));
                fflush(stdout);
            }
            if (do_pc && n >= 2) {
                printf("pc,%d,%d,%.2f\n", n, r, run<Q>("pc", n, seconds, pin, cap));
                fflush(stdout);
            }
            if (do_mix) {
                printf("mix,%d,%d,%.2f\n", n, r, run<Q>("mix", n, seconds, pin, cap));
                fflush(stdout);
            }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    double seconds = 2.0;
    int reps = 1;
    bool pin = false;
    int64_t cap = 8192;
    std::string threads = "1,2,4,8,16", workload = "both", variant = "hp";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) threads = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) workload = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p")) pin = true;
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) cap = atoll(argv[++i]);
        else if (!strcmp(argv[i], "-q") && i + 1 < argc) variant = argv[++i];
        else {
            fprintf(stderr, "usage: %s -q hp|ebr|nebr|debra|qsbr [-s sec] [-t 1,2,4] [-w sym|pc|both] [-r reps] [-p] [-i inflight]\n", argv[0]);
            return 2;
        }
    }
    if (variant == "hp") return run_all<hp_t>(threads.c_str(), workload.c_str(), seconds, reps, pin, cap);
    if (variant == "ebr") return run_all<ebr_t>(threads.c_str(), workload.c_str(), seconds, reps, pin, cap);
    if (variant == "nebr") return run_all<nebr_t>(threads.c_str(), workload.c_str(), seconds, reps, pin, cap);
    if (variant == "debra") return run_all<debra_t>(threads.c_str(), workload.c_str(), seconds, reps, pin, cap);
    if (variant == "qsbr") return run_all<qsbr_t>(threads.c_str(), workload.c_str(), seconds, reps, pin, cap);
    fprintf(stderr, "unknown -q %s\n", variant.c_str());
    return 2;
}
