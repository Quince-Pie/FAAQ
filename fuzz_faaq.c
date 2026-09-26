// Model-based fuzz harness for FAAArrayQueue.
//
// Entry point: LLVMFuzzerTestOneInput, the libFuzzer convention that AFL++
// (afl-clang-lto -fsanitize=fuzzer, via libAFLDriver), libFuzzer and honggfuzz
// all drive directly. The input bytes are a small program: each byte selects
// an operation and a lane (one of NQ live queues); operands follow. Every
// dequeue is checked against a reference FIFO, every destroy must drain
// exactly the model's contents in order, and BURST runs a real multi-threaded
// phase (P producers, C consumers) that checks exact-once delivery, per-producer
// FIFO order, emptiness afterwards and - with a single consumer - that a
// dequeue never reports "empty" once all producers have finished.
//
// Build it with the shrunken tunables (see Makefile: FUZZ_CFG) so node
// boundaries, hazard-pointer scans, slot poisoning, per-thread slot eviction
// and real free()s happen every few operations instead of every few thousand.
//
// -DFAAQ_FUZZ_MAIN adds a main() that replays crash files, or, with no
// arguments, runs a PRNG campaign (FAAQ_FUZZ_ITERS / FAAQ_FUZZ_SEED env) so the
// same checks run under gcc + ASan/UBSan/TSan without a fuzzing engine.

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "faaq.h"
#include "test_threads.h"

enum
{
    NQ              = 6, // live queues; more than FAAQ_TLS_SLOTS in fuzz builds
    MAX_PRODUCERS   = 4,
    MAX_CONSUMERS   = 4,
    MAX_BURST_PER_P = 2048, // items per producer
    MAX_BULK        = 4096, // ENQ_N / DEQ_N count
};

[[noreturn]]
static void fail(char const* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("\n[FUZZ FAIL] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}

// ---------------------------------------------------------------------------
// Reference model: unbounded FIFO of 64-bit values
// ---------------------------------------------------------------------------

typedef struct
{
    uint64_t* buf;
    size_t    cap, head, len;
} model_t;

static void model_push(model_t* m, uint64_t v)
{
    if (m->len == m->cap) {
        size_t    ncap = m->cap ? m->cap * 2 : 64;
        uint64_t* nb   = malloc(ncap * sizeof *nb);
        if (!nb)
            fail("oom");
        for (size_t i = 0; i < m->len; i++)
            nb[i] = m->buf[(m->head + i) % m->cap];
        free(m->buf);
        m->buf  = nb;
        m->cap  = ncap;
        m->head = 0;
    }
    m->buf[(m->head + m->len) % m->cap] = v;
    m->len++;
}

static bool model_pop(model_t* m, uint64_t* out)
{
    if (m->len == 0)
        return false;
    *out    = m->buf[m->head];
    m->head = (m->head + 1) % m->cap;
    m->len--;
    return true;
}

static void model_free(model_t* m)
{
    free(m->buf);
    *m = (model_t){};
}

// Values travel as tagged pointers: never nullptr, never 64-byte aligned (the
// queue's private "taken" sentinel is), so no collision with either.
static inline void* tag(uint64_t v)
{
    return (void*)(uintptr_t)((v << 1) | 1);
}
static inline uint64_t untag(void* p)
{
    return (uint64_t)(uintptr_t)p >> 1;
}

// ---------------------------------------------------------------------------
// Lanes
// ---------------------------------------------------------------------------

typedef struct
{
    FAAArrayQueue_t* q;
    model_t          m;
    uint64_t         next_val;
} lane_t;

static lane_t   g_lanes[NQ];
static model_t* g_drain_model; // model being drained by faa_queue_destroy

static void on_drained(void* item)
{
    uint64_t expect;
    if (!model_pop(g_drain_model, &expect))
        fail("destroy drained more items than the model holds");
    if (untag(item) != expect)
        fail("destroy drained %" PRIu64 ", model expected %" PRIu64, untag(item), expect);
}

static void lane_open(lane_t* l)
{
    l->q = faa_queue_create();
    if (!l->q)
        fail("faa_queue_create failed");
}

static void lane_close(lane_t* l)
{
    g_drain_model = &l->m;
    faa_queue_destroy(l->q, on_drained);
    g_drain_model = nullptr;
    if (l->m.len != 0)
        fail("destroy left %zu items undrained", l->m.len);
    l->q = nullptr;
}

static void lane_enq(lane_t* l)
{
    uint64_t const v = ++l->next_val;
    faa_queue_enqueue(l->q, tag(v));
    model_push(&l->m, v);
}

static void lane_deq(lane_t* l)
{
    uint64_t   expect;
    bool const have = model_pop(&l->m, &expect);
    void*      item = faa_queue_dequeue(l->q);
    if (!have) {
        if (item != nullptr)
            fail("dequeue returned %" PRIu64 " from an empty queue", untag(item));
        return;
    }
    if (item == nullptr)
        fail("dequeue reported empty; model expected %" PRIu64 " (%zu more queued)",
             expect,
             l->m.len);
    if (untag(item) != expect)
        fail("FIFO violation: got %" PRIu64 ", expected %" PRIu64, untag(item), expect);
}

// ---------------------------------------------------------------------------
// Concurrent burst
// ---------------------------------------------------------------------------

typedef struct
{
    FAAArrayQueue_t*        q;
    int                     id;    // producer index
    uint32_t                per_p; // items per producer
    int                     producers;
    _Atomic(uint32_t)*      consumed;
    uint32_t                total;
    _Atomic(int)*           barrier;
    int                     nthreads;
    _Atomic(bool)*          producers_done;
    bool                    sole_consumer;
    _Atomic(unsigned char)* seen;
} burst_ctx_t;

static void burst_barrier(burst_ctx_t* c)
{
    atomic_fetch_add_explicit(c->barrier, 1, memory_order_acq_rel);
    while (atomic_load_explicit(c->barrier, memory_order_acquire) < c->nthreads) {
        thrd_yield();
    }
}

static int burst_producer(void* arg)
{
    burst_ctx_t* c = arg;
    burst_barrier(c);
    for (uint32_t seq = 0; seq < c->per_p; seq++) {
        faa_queue_enqueue(c->q, tag(((uint64_t)(c->id + 1) << 32) | seq));
    }
    return 0;
}

static int burst_consumer(void* arg)
{
    burst_ctx_t* c = arg;
    uint32_t     last[MAX_PRODUCERS];
    for (int i = 0; i < MAX_PRODUCERS; i++)
        last[i] = UINT32_MAX;
    burst_barrier(c);

    while (atomic_load_explicit(c->consumed, memory_order_acquire) < c->total) {
        bool const  done = atomic_load_explicit(c->producers_done, memory_order_acquire);
        void* const item = faa_queue_dequeue(c->q);
        if (item == nullptr) {
            // With one consumer, nobody else can be holding the missing items.
            if (done && c->sole_consumer
                && atomic_load_explicit(c->consumed, memory_order_acquire) < c->total) {
                fail("spurious empty: producers finished, %" PRIu32 " of %" PRIu32
                     " items still queued",
                     c->total - atomic_load_explicit(c->consumed, memory_order_acquire),
                     c->total);
            }
            thrd_yield();
            continue;
        }
        uint64_t const v   = untag(item);
        int const      p   = (int)(v >> 32) - 1;
        uint32_t const seq = (uint32_t)v;
        if (p < 0 || p >= c->producers || seq >= c->per_p)
            fail("foreign item %#" PRIx64, v);
        if (last[p] != UINT32_MAX && seq <= last[p])
            fail(
                "per-producer FIFO violation: p%d seq %" PRIu32 " after %" PRIu32, p, seq, last[p]);
        last[p] = seq;
        if (atomic_exchange_explicit(&c->seen[(size_t)p * c->per_p + seq], 1, memory_order_acq_rel)
            != 0) {
            fail("duplicate delivery: p%d seq %" PRIu32, p, seq);
        }
        atomic_fetch_add_explicit(c->consumed, 1, memory_order_acq_rel);
    }
    return 0;
}

static void lane_burst(lane_t* l, int producers, int consumers, uint32_t per_p)
{
    // Start from an empty queue so that every delivered item is a burst item.
    uint64_t v;
    while (model_pop(&l->m, &v)) {
        void* item = faa_queue_dequeue(l->q);
        if (item == nullptr || untag(item) != v)
            fail("pre-burst drain mismatch");
    }
    if (faa_queue_dequeue(l->q) != nullptr)
        fail("pre-burst: queue not empty");

    uint32_t const          total = (uint32_t)producers * per_p;
    _Atomic(unsigned char)* seen  = calloc(total, sizeof *seen);
    if (!seen)
        fail("oom");
    _Atomic(uint32_t) consumed       = 0;
    _Atomic(int)      barrier        = 0;
    _Atomic(bool)     producers_done = false;
    int const         n              = producers + consumers;
    thrd_t            th[MAX_PRODUCERS + MAX_CONSUMERS];
    burst_ctx_t       ctx[MAX_PRODUCERS + MAX_CONSUMERS];

    for (int i = 0; i < n; i++) {
        ctx[i] = (burst_ctx_t){l->q,
                               i,
                               per_p,
                               producers,
                               &consumed,
                               total,
                               &barrier,
                               n,
                               &producers_done,
                               consumers == 1,
                               seen};
        if (xthrd_create(&th[i], i < producers ? burst_producer : burst_consumer, &ctx[i])
            != thrd_success) {
            fail("thrd_create");
        }
    }
    for (int i = 0; i < producers; i++)
        xthrd_join(th[i], nullptr);
    atomic_store_explicit(&producers_done, true, memory_order_release);
    for (int i = producers; i < n; i++)
        xthrd_join(th[i], nullptr);

    if (atomic_load_explicit(&consumed, memory_order_acquire) != total)
        fail("burst count mismatch");
    for (uint32_t i = 0; i < total; i++) {
        if (atomic_load_explicit(&seen[i], memory_order_relaxed) != 1)
            fail("burst item %" PRIu32 " lost", i);
    }
    if (faa_queue_dequeue(l->q) != nullptr)
        fail("post-burst: queue not empty");
    free(seen);
}

// ---------------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------------

typedef struct
{
    uint8_t const* d;
    size_t         n, i;
} cursor_t;

static uint8_t next_u8(cursor_t* c)
{
    return c->i < c->n ? c->d[c->i++] : 0;
}
static uint16_t next_u16(cursor_t* c)
{
    uint16_t lo = next_u8(c);
    return (uint16_t)(lo | (uint16_t)(next_u8(c) << 8));
}

enum
{
    OP_ENQ,
    OP_DEQ,
    OP_ENQ_N,
    OP_DEQ_N,
    OP_RECREATE,
    OP_BURST,
    OP_CLEANUP,
    OP_DRAIN,
    OP_COUNT
};

int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    cursor_t c = {data, size, 0};
    for (int i = 0; i < NQ; i++) {
        g_lanes[i] = (lane_t){};
        lane_open(&g_lanes[i]);
    }

    while (c.i < c.n) {
        uint8_t const b    = next_u8(&c);
        lane_t*       lane = &g_lanes[(b >> 3) % NQ];
        switch (b & 7) {
            case OP_ENQ:
                lane_enq(lane);
                break;
            case OP_DEQ:
                lane_deq(lane);
                break;
            case OP_ENQ_N: {
                uint32_t n = 1u + (next_u16(&c) % MAX_BULK);
                while (n--)
                    lane_enq(lane);
                break;
            }
            case OP_DEQ_N: {
                uint32_t n = 1u + (next_u16(&c) % MAX_BULK);
                while (n--)
                    lane_deq(lane);
                break;
            }
            case OP_RECREATE:
                lane_close(lane);
                lane_open(lane);
                break;
            case OP_BURST: {
                int const      p     = 1 + (next_u8(&c) % MAX_PRODUCERS);
                int const      cons  = 1 + (next_u8(&c) % MAX_CONSUMERS);
                uint32_t const per_p = 1u + (next_u16(&c) % MAX_BURST_PER_P);
                lane_burst(lane, p, cons, per_p);
                break;
            }
            case OP_CLEANUP:
                hazptr_cleanup();
                break;
            case OP_DRAIN: {
                uint64_t v;
                while (model_pop(&lane->m, &v)) {
                    void* item = faa_queue_dequeue(lane->q);
                    if (item == nullptr || untag(item) != v)
                        fail("drain mismatch");
                }
                if (faa_queue_dequeue(lane->q) != nullptr)
                    fail("drain: queue not empty");
                break;
            }
            default:
                break;
        }
    }

    for (int i = 0; i < NQ; i++) {
        lane_close(&g_lanes[i]);
        model_free(&g_lanes[i].m);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Standalone driver (no fuzzing engine)
// ---------------------------------------------------------------------------

#ifdef FAAQ_FUZZ_MAIN
static uint64_t rng_state[4];
static uint64_t rotl(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}
static uint64_t rng_next(void) // xoshiro256**
{
    uint64_t const r = rotl(rng_state[1] * 5, 7) * 9;
    uint64_t const t = rng_state[1] << 17;
    rng_state[2] ^= rng_state[0];
    rng_state[3] ^= rng_state[1];
    rng_state[1] ^= rng_state[2];
    rng_state[0] ^= rng_state[3];
    rng_state[2] ^= t;
    rng_state[3] = rotl(rng_state[3], 45);
    return r;
}

static int run_file(char const* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long const n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)(n > 0 ? n : 1));
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        return 1;
    }
    fclose(f);
    LLVMFuzzerTestOneInput(buf, (size_t)n);
    free(buf);
    printf("ok  %s (%ld bytes)\n", path, n);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1) {
        int rc = 0;
        for (int i = 1; i < argc; i++)
            rc |= run_file(argv[i]);
        return rc;
    }
    char const* it   = getenv("FAAQ_FUZZ_ITERS");
    char const* sd   = getenv("FAAQ_FUZZ_SEED");
    long const  iter = it ? atol(it) : 300;
    uint64_t    seed = sd ? strtoull(sd, nullptr, 0) : 1;
    for (int i = 0; i < 4; i++) {
        seed         = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        rng_state[i] = seed;
    }

    uint8_t buf[768];
    for (long i = 0; i < iter; i++) {
        size_t const len = 1 + (size_t)(rng_next() % sizeof buf);
        for (size_t j = 0; j < len; j++)
            buf[j] = (uint8_t)rng_next();
        LLVMFuzzerTestOneInput(buf, len);
        if ((i + 1) % 100 == 0) {
            printf("  %ld/%ld inputs ok\n", i + 1, iter);
            fflush(stdout);
        }
    }
    printf("fuzz smoke: %ld random inputs, no failures\n", iter);
    return 0;
}
#endif
