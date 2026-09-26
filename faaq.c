#include "faaq.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

// ----------------------------------------------------------------------------
// Tunables (all overridable on the command line; fuzz builds shrink them)
// ----------------------------------------------------------------------------

// Per-thread cache of per-queue state (MRU order). A thread that interleaves
// operations on more queues than this evicts and re-attaches (correct, slower).
#ifndef FAAQ_TLS_SLOTS
#    define FAAQ_TLS_SLOTS 4
#endif

// Reclaimed nodes are kept per thread for reuse instead of being freed.
#ifndef FAAQ_NODE_CACHE_CAPACITY
#    define FAAQ_NODE_CACHE_CAPACITY 256
#endif

// Bounded waits for a slow peer (iterations). Bounded keeps every operation
// lock-free: after the bound, an enqueuer links its own node and a dequeuer
// poisons the slot / reports empty. 0 disables waiting (fuzz builds).
#ifndef FAAQ_SPIN_NEXT
#    define FAAQ_SPIN_NEXT 1024
#endif
#ifndef FAAQ_SPIN_ITEM
#    define FAAQ_SPIN_ITEM 1200
#endif

// Speculative claim. A dequeue normally loads deqidx to check for emptiness and
// then fetch-adds it; under contention each of the two touches misses on the
// same line, because other dequeuers steal it in between. When the last enqidx
// this thread saw lies more than this many indices beyond its own last claim,
// producer-claimed items provably lie ahead and the dequeue claims with the
// fetch-add alone. An overshoot (other dequeuers took all the slack) shows up
// as an empty slot, is poisoned at once, and the next attempt takes the checked
// path. 0 disables.
#ifndef FAAQ_SPEC_CLAIM
#    define FAAQ_SPEC_CLAIM 16
#endif

static_assert(FAAQ_TLS_SLOTS >= 1, "need at least one slot");

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
}

// Waiting for an enqueuer to store its item: it may have been preempted, so
// back off exponentially.
static inline void backoff(int spin)
{
    int const n = (spin < 64) ? 1 : (spin < 256) ? 2 : (spin < 512) ? 4 : 8;
    for (int p = 0; p < n; p++) {
        cpu_relax();
    }
}

// Waiting for a peer to link the next node: that lands within a microsecond,
// so poll quickly; a long backoff here stalls every overflowing peer.
static inline void backoff_link(int spin)
{
    cpu_relax();
    if (spin >= 64) {
        cpu_relax();
    }
}

// A dequeuer that gives up waiting for a slow enqueuer poisons the slot with
// this address so the enqueuer's CAS fails and it claims a fresh index. A
// private static object can never equal a caller's item.
alignas(64) static unsigned char faaq_taken_storage[64];
#define TAKEN ((void*)faaq_taken_storage)

// aligned_alloc requires the size to be a multiple of the alignment.
#define ALIGNED_SIZE(sz) (((sz) + FAA_ALIGNMENT - 1) & ~(FAA_ALIGNMENT - 1))

// ----------------------------------------------------------------------------
// Per-thread state
// ----------------------------------------------------------------------------

typedef struct
{
    uint64_t        qid;         // 0 = free
    hazptr_holder_t head_holder; // protects last_head while it is non-null
    hazptr_holder_t tail_holder; // protects last_tail while it is non-null
    Node_t*         last_head;
    Node_t*         last_tail;
    size_t          enq_seen; // lower bound of last_head->enqidx seen on this node
    size_t          deq_last; // last index claimed on last_head (speculation)
} faaq_slot_t;

typedef struct
{
    alignas(64) faaq_slot_t slots[FAAQ_TLS_SLOTS]; // slots[0] = most recently used
    Node_t* nodes[FAAQ_NODE_CACHE_CAPACITY > 0 ? FAAQ_NODE_CACHE_CAPACITY : 1];
    int     node_count;
    bool    registered; // tss destructor armed
    bool    exiting;    // destructor ran: reclaim frees instead of caching
} faaq_tls_t;

// Executables get the local-exec TLS model (one %fs-relative instruction) by
// default; build a shared library with -ftls-model=initial-exec if the extra
// __tls_get_addr call matters there.
static thread_local faaq_tls_t faaq_tls;

static _Atomic(uint64_t) faaq_next_id = 1;

// --- Node cache ---

// HP reclamation callback: the node is unreachable and unprotected.
static void node_reclaim(hazptr_obj_t* obj)
{
    if (!obj) {
        return;
    }
    Node_t*     node = (Node_t*)obj;
    faaq_tls_t* t    = &faaq_tls;
    if (!t->exiting && t->node_count < FAAQ_NODE_CACHE_CAPACITY) {
        t->nodes[t->node_count++] = node;
    } else {
        free(node);
    }
}

// Returns nullptr on allocation failure.
static Node_t* node_new(void* initial_item)
{
    faaq_tls_t* t = &faaq_tls;
    Node_t*     node;
    if (t->node_count > 0) {
        node = t->nodes[--t->node_count];
    } else {
        node = aligned_alloc(FAA_ALIGNMENT, ALIGNED_SIZE(sizeof(Node_t)));
        if (!node) {
            return nullptr;
        }
    }

    node->hp_base = (hazptr_obj_t){};
    atomic_init(&node->deqidx, 0);
    atomic_init(&node->next, nullptr);

    size_t start_idx = 0;
    if (initial_item != nullptr) {
        atomic_init(&node->enqidx, 1);
        atomic_init(&node->items[0], initial_item);
        start_idx = 1;
    } else {
        atomic_init(&node->enqidx, 0);
    }
    if (start_idx < FAA_BUFFER_SIZE) {
        memset((void*)&node->items[start_idx],
               0,
               (FAA_BUFFER_SIZE - start_idx) * sizeof(node->items[0]));
    }
    return node;
}

// --- Thread exit ---

static tss_t        faaq_tss_key;
static _Atomic(int) faaq_tss_state; // 0 = key not created, 1 = creating, 2 = ready

static void slot_release(faaq_slot_t* s)
{
    // Destroying a holder resets its hazard pointer first, so last_head/last_tail
    // stop being protected exactly when we stop dereferencing them.
    hazptr_holder_destroy(&s->head_holder);
    hazptr_holder_destroy(&s->tail_holder);
    s->last_head = nullptr;
    s->last_tail = nullptr;
    s->qid       = 0;
}

static void faaq_tss_dtor(void* p)
{
    faaq_tls_t* t = p;
    for (int i = 0; i < FAAQ_TLS_SLOTS; i++) {
        if (t->slots[i].qid != 0) {
            slot_release(&t->slots[i]);
        }
    }
    // Returns this thread's HP records to the domain and hands its retired
    // nodes over; reclamation may push nodes into our cache, so flush it after.
    hazptr_thread_exit();
    t->exiting = true;
    for (int i = 0; i < t->node_count; i++) {
        free(t->nodes[i]);
    }
    t->node_count = 0;
    t->registered = false;
}

// call_once() would do, but glibc implements it through an internal
// pthread_once that ThreadSanitizer cannot see; a two-phase atomic gives the
// same guarantee and keeps TSan builds clean.
static void faaq_tss_key_once(void)
{
    if (atomic_load_explicit(&faaq_tss_state, memory_order_acquire) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &faaq_tss_state, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
        if (tss_create(&faaq_tss_key, faaq_tss_dtor) != thrd_success) {
            fprintf(stderr, "C23 FAAQueue Fatal Error: tss_create failed.\n");
            abort();
        }
        atomic_store_explicit(&faaq_tss_state, 2, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&faaq_tss_state, memory_order_acquire) != 2) {
        thrd_yield();
    }
}

static void faaq_register_thread(faaq_tls_t* t)
{
    if (t->registered) {
        return;
    }
    faaq_tss_key_once();
    if (tss_set(faaq_tss_key, t) != thrd_success) {
        fprintf(stderr, "C23 FAAQueue Fatal Error: tss_set failed.\n");
        abort();
    }
    t->registered = true;
    t->exiting    = false;
}

// --- Slot lookup ---

__attribute__((noinline, cold)) static void slot_lookup_slow(FAAArrayQueue_t const* q)
{
    faaq_tls_t*    t     = &faaq_tls;
    faaq_slot_t*   slots = t->slots;
    uint64_t const id    = q->id;

    // Hit in a colder slot: move it to the front.
    for (int i = 1; i < FAAQ_TLS_SLOTS; i++) {
        if (slots[i].qid == id) {
            faaq_slot_t const hit = slots[i];
            memmove(&slots[1], &slots[0], (size_t)i * sizeof(faaq_slot_t));
            slots[0] = hit;
            return;
        }
    }

    // Miss: take the first free slot, else evict the least recently used one.
    faaq_register_thread(t);
    int victim = FAAQ_TLS_SLOTS - 1;
    for (int i = 0; i < FAAQ_TLS_SLOTS; i++) {
        if (slots[i].qid == 0) {
            victim = i;
            break;
        }
    }
    if (slots[victim].qid != 0) {
        slot_release(&slots[victim]);
    }
    memmove(&slots[1], &slots[0], (size_t)victim * sizeof(faaq_slot_t));

    faaq_slot_t* s = &slots[0];
    *s             = (faaq_slot_t){.qid = id};
    hazptr_holder_init(&s->head_holder);
    hazptr_holder_init(&s->tail_holder);
}

// Hot path: one thread-local compare (slot_attached). The state a tid used to
// select lives in the thread instead of in the queue. Afterwards slots[0] is
// the attached queue's entry, so the operations address it as a constant
// thread-local (an %fs-relative operand, no base register kept live). A miss
// goes through a tail-calling helper so the hot paths contain no call and
// therefore no stack adjustment at all.
static inline bool slot_attached(FAAArrayQueue_t const* q)
{
    return __builtin_expect(faaq_tls.slots[0].qid == q->id, 1);
}

__attribute__((noinline, cold)) static void enqueue_miss(FAAArrayQueue_t* q, void* item)
{
    slot_lookup_slow(q);
    faa_queue_enqueue(q, item);
}

__attribute__((noinline, cold)) static void* dequeue_miss(FAAArrayQueue_t* q)
{
    slot_lookup_slow(q);
    return faa_queue_dequeue(q);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

FAAArrayQueue_t* faa_queue_create(void)
{
    FAAArrayQueue_t* q = aligned_alloc(FAA_ALIGNMENT, ALIGNED_SIZE(sizeof(FAAArrayQueue_t)));
    if (!q) {
        return nullptr;
    }
    Node_t* sentinel = node_new(nullptr);
    if (!sentinel) {
        free(q);
        return nullptr;
    }
    q->id = atomic_fetch_add_explicit(&faaq_next_id, 1, memory_order_relaxed);
    atomic_init(&q->head, sentinel);
    atomic_init(&q->tail, sentinel);
    return q;
}

void faa_queue_destroy(FAAArrayQueue_t* q, void (*free_payload)(void*))
{
    if (!q) {
        return;
    }

    void* item;
    while ((item = faa_queue_dequeue(q)) != nullptr) {
        if (free_payload) {
            free_payload(item);
        }
    }

    // Drop this thread's cached protection of q's nodes. Other threads' cached
    // entries for q are inert (the id never matches again) and are evicted or
    // released at thread exit; until then each pins at most two node addresses.
    faaq_tls_t* t = &faaq_tls;
    for (int i = 0; i < FAAQ_TLS_SLOTS; i++) {
        if (t->slots[i].qid == q->id) {
            slot_release(&t->slots[i]);
        }
    }

    // Quiescent: after draining, the chain is the single last node.
    Node_t* n = atomic_load_explicit(&q->head, memory_order_relaxed);
    while (n) {
        Node_t* next = atomic_load_explicit(&n->next, memory_order_relaxed);
        node_reclaim(&n->hp_base);
        n = next;
    }

    // Reclaim what draining retired (and any older backlog).
    hazptr_cleanup();

    free(q);
}

/*
 * Memory-order notes (both operations).
 *
 * enqidx/deqidx fetch-adds and the empty-check loads are seq_cst: the
 * linearizability argument of FAA queues ("a dequeue that observes deqidx >=
 * enqidx and next == nullptr saw an empty queue") needs a single total order
 * over the index claims of *both* counters. On x86-64 this costs nothing
 * (lock xadd is already a full barrier; seq_cst loads are plain movs). Item
 * slots and the next/head/tail links use release/acquire pairs, which is all
 * their single-location publication needs.
 */

/*
 * The slow paths of both operations are kept out of line and end by
 * re-entering the operation (a tail call the compiler turns into a jump, so no
 * stack grows). That is the retry loop of the original algorithm, but the hot
 * paths carry no stack frame and no callee-saved register: from the codegen,
 * seven of the twenty-four instructions of the old dequeue fast path were frame
 * and register bookkeeping for the transition paths alone.
 */

__attribute__((noinline, cold)) static void enqueue_reattach(FAAArrayQueue_t* q, void* item)
{
    faaq_slot_t* const s = &faaq_tls.slots[0];
    Node_t*            ltail;
    HAZPTR_PROTECT(ltail, &s->tail_holder, &q->tail);
    s->last_tail = ltail;
    faa_queue_enqueue(q, item);
}

// The cached node is full: link a new node carrying the item, or follow the
// successor another enqueuer linked, then retry.
__attribute__((noinline)) static void
enqueue_full(FAAArrayQueue_t* q, void* item, Node_t* ltail, size_t idx)
{
    faaq_slot_t* const     s = &faaq_tls.slots[0];
    hazptr_holder_t* const h = &s->tail_holder;

    Node_t* lnext = atomic_load_explicit(&ltail->next, memory_order_acquire);

    if (lnext == nullptr && idx > FAA_BUFFER_SIZE) {
        // Another enqueuer overflowed first and is linking a node: wait briefly
        // rather than allocate a node that will lose the CAS.
        for (int spin = 0; spin < FAAQ_SPIN_NEXT && lnext == nullptr; spin++) {
            backoff_link(spin);
            lnext = atomic_load_explicit(&ltail->next, memory_order_acquire);
        }
    }

    if (lnext == nullptr) {
        Node_t* new_node = node_new(item);
        if (!new_node) {
            perror("C23 FAAQueue Fatal Error: Failed to allocate Node_t");
            abort();
        }
        Node_t* expected_next = nullptr;
        if (atomic_compare_exchange_strong_explicit(&ltail->next,
                                                    &expected_next,
                                                    new_node,
                                                    memory_order_release,
                                                    memory_order_acquire)) {
            Node_t* expected_tail = ltail;
            atomic_compare_exchange_strong_explicit(
                &q->tail, &expected_tail, new_node, memory_order_release, memory_order_relaxed);
            s->last_tail = nullptr;
            hazptr_reset(h, nullptr);
            return;
        }
        node_reclaim(&new_node->hp_base); // never published
        lnext = expected_next;
    }

    // Help advance the tail, but only if it still points here: a CAS that would
    // fail still takes the line exclusive, and every overflowing enqueuer gets
    // here at once.
    if (atomic_load_explicit(&q->tail, memory_order_relaxed) == ltail) {
        Node_t* expected_tail = ltail;
        atomic_compare_exchange_strong_explicit(
            &q->tail, &expected_tail, lnext, memory_order_release, memory_order_relaxed);
    }
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(lnext, 1, 3);
#endif
    s->last_tail = nullptr;
    hazptr_reset(h, nullptr);
    faa_queue_enqueue(q, item);
}

void faa_queue_enqueue(FAAArrayQueue_t* q, void* item)
{
    if (item == nullptr || item == TAKEN) {
        return;
    }

    if (!slot_attached(q)) {
        enqueue_miss(q, item);
        return;
    }
    faaq_slot_t* const s     = &faaq_tls.slots[0];
    Node_t* const      ltail = s->last_tail;
    if (ltail == nullptr) {
        enqueue_reattach(q, item);
        return;
    }

    size_t const idx = atomic_fetch_add_explicit(&ltail->enqidx, 1, memory_order_seq_cst);
    if (idx >= FAA_BUFFER_SIZE) {
        enqueue_full(q, item, ltail, idx);
        return;
    }

    size_t const physical_idx = (idx * FAA_STRIDE) & (FAA_BUFFER_SIZE - 1);
    void*        expected     = nullptr;
    if (atomic_compare_exchange_strong_explicit(&ltail->items[physical_idx],
                                                &expected,
                                                item,
                                                memory_order_release,
                                                memory_order_relaxed)) {
        // ltail stays cached (and protected) for the next call.
        return;
    }
    // A dequeuer gave up on this slot and poisoned it: claim a new index.
    faa_queue_enqueue(q, item);
}

__attribute__((noinline, cold)) static void* dequeue_reattach(FAAArrayQueue_t* q)
{
    faaq_slot_t* const s = &faaq_tls.slots[0];
    Node_t*            lhead;
    HAZPTR_PROTECT(lhead, &s->head_holder, &q->head);
    s->last_head = lhead;
    s->enq_seen  = 0;
    s->deq_last  = 0;
    return faa_queue_dequeue(q);
}

// The cached node is drained: move to the successor and retry, or report empty.
__attribute__((noinline)) static void* dequeue_drained(FAAArrayQueue_t* q, Node_t* lhead)
{
    faaq_slot_t* const     s = &faaq_tls.slots[0];
    hazptr_holder_t* const h = &s->head_holder;

    Node_t* lnext = atomic_load_explicit(&lhead->next, memory_order_acquire);
    if (lnext == nullptr) {
        // Only an enqueuer that overflowed this node will link a successor.
        for (int spin = 0; spin < FAAQ_SPIN_NEXT && lnext == nullptr; spin++) {
            if (atomic_load_explicit(&lhead->enqidx, memory_order_seq_cst) <= FAA_BUFFER_SIZE) {
                break;
            }
            backoff_link(spin);
            lnext = atomic_load_explicit(&lhead->next, memory_order_acquire);
        }
        if (lnext == nullptr) {
            // lhead is the last node: the queue is empty. Keep it cached.
            return nullptr;
        }
    }

    // Keep tail >= head, then try to advance head. Each CAS runs only if the
    // pointer still needs it: a CAS that would fail still takes the line
    // exclusive, and every dequeuer that drained this node gets here at once.
    if (atomic_load_explicit(&q->tail, memory_order_relaxed) == lhead) {
        Node_t* expected_tail = lhead;
        atomic_compare_exchange_strong_explicit(
            &q->tail, &expected_tail, lnext, memory_order_release, memory_order_relaxed);
    }
    bool advanced = false;
    if (atomic_load_explicit(&q->head, memory_order_relaxed) == lhead) {
        Node_t* expected_head = lhead;
        advanced              = atomic_compare_exchange_strong_explicit(
            &q->head, &expected_head, lnext, memory_order_release, memory_order_relaxed);
    }
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(lnext, 0, 3);
#endif
    s->last_head = nullptr;
    hazptr_reset(h, nullptr);
    if (advanced) {
        // We unlinked it; nobody can reach it through the queue any more.
        hazptr_retire(&lhead->hp_base, node_reclaim);
    }
    return faa_queue_dequeue(q);
}

// The index is ours but its slot is still empty: the enqueuer that claimed it
// has not stored yet, or, after a speculative claim, nobody has claimed it.
__attribute__((noinline)) static void*
dequeue_wait(FAAArrayQueue_t* q, Node_t* lhead, size_t idx, size_t physical_idx, bool speculative)
{
    faaq_slot_t* const s    = &faaq_tls.slots[0];
    void*              item = nullptr;

    if (speculative) {
        size_t const enq_now = atomic_load_explicit(&lhead->enqidx, memory_order_seq_cst);
        s->enq_seen          = enq_now;
        if (idx >= enq_now) {
            // Nobody has claimed this index for an enqueue: poison it so a later
            // enqueue retries elsewhere, then take the checked path.
            item = atomic_exchange_explicit(
                &lhead->items[physical_idx], TAKEN, memory_order_acquire);
            return item != nullptr ? item : faa_queue_dequeue(q);
        }
    }

    for (int spin = 0; spin < FAAQ_SPIN_ITEM && item == nullptr; spin++) {
        backoff(spin);
        item = atomic_load_explicit(&lhead->items[physical_idx], memory_order_acquire);
    }
    if (item == nullptr) {
        // Poison the slot: either we get the item that just landed, or the
        // enqueuer's CAS fails and it retries with a new index. An exchange
        // returns the old value, so it is never TAKEN.
        item = atomic_exchange_explicit(&lhead->items[physical_idx], TAKEN, memory_order_acquire);
    }
    return item != nullptr ? item : faa_queue_dequeue(q);
}

void* faa_queue_dequeue(FAAArrayQueue_t* q)
{
    if (!slot_attached(q)) {
        return dequeue_miss(q);
    }
    faaq_slot_t* const s     = &faaq_tls.slots[0];
    Node_t* const      lhead = s->last_head;
    if (lhead == nullptr) {
        return dequeue_reattach(q);
    }

    bool speculative = false;
#if FAAQ_SPEC_CLAIM > 0
    speculative = s->enq_seen > s->deq_last + FAAQ_SPEC_CLAIM;
#endif
    if (!speculative) {
        // Empty check. The next == nullptr part is essential: a cached (or
        // freshly loaded) head that has been drained still has items behind it
        // whenever a successor exists, and only the fetch-add path below moves
        // past it.
        size_t const deq = atomic_load_explicit(&lhead->deqidx, memory_order_seq_cst);
        // enqidx only grows, so a value seen earlier on this node is a lower bound:
        // while deqidx is below it the queue is provably non-empty and the
        // enqueuers' hot line need not be read at all.
        if (deq >= s->enq_seen) {
            size_t const enq = atomic_load_explicit(&lhead->enqidx, memory_order_seq_cst);
            s->enq_seen      = enq;
            if (deq >= enq && atomic_load_explicit(&lhead->next, memory_order_acquire) == nullptr) {
                return nullptr;
            }
        }
    }

    size_t const idx = atomic_fetch_add_explicit(&lhead->deqidx, 1, memory_order_seq_cst);
    s->deq_last      = idx;
    if (idx >= FAA_BUFFER_SIZE) {
        return dequeue_drained(q, lhead);
    }

    size_t const physical_idx = (idx * FAA_STRIDE) & (FAA_BUFFER_SIZE - 1);
    // Fast path: the item is usually already there; no RMW needed, the index is
    // ours alone. lhead stays cached (and protected) for the next call.
    void* const item = atomic_load_explicit(&lhead->items[physical_idx], memory_order_acquire);
    if (item == nullptr) {
        return dequeue_wait(q, lhead, idx, physical_idx, speculative);
    }
    return item;
}
