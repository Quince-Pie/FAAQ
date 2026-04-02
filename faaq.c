#include "faaq.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define NODE_CACHE_CAPACITY 256

typedef struct {
    alignas(128) Node_t* nodes[NODE_CACHE_CAPACITY];
    alignas(128) int count;
} NodeCache_t;

alignas(128) static thread_local NodeCache_t local_node_cache = { .count = 0 };

static void node_reclaim(hazptr_obj_t* obj)
{
    if (!obj) {
        return;
    }
    Node_t* node = (Node_t*)obj;
    if (local_node_cache.count < NODE_CACHE_CAPACITY) {
        local_node_cache.nodes[local_node_cache.count++] = node;
    } else {
        free(node);
    }
}

static Node_t* create_node(void* initial_item)
{
    Node_t* node = nullptr;
    if (local_node_cache.count > 0) {
        node = local_node_cache.nodes[--local_node_cache.count];
    } else {
        node = aligned_alloc(FAA_ALIGNMENT, sizeof(Node_t));
        if (!node) {
            perror("C23 FAAQueue Fatal Error: Failed to allocate Node_t");
            abort();
        }
    }

    node->hp_base = (hazptr_obj_t){};

    atomic_init(&node->deqidx, 0);
    atomic_init(&node->next, nullptr);

    size_t start_idx;

    if (initial_item != nullptr) {
        atomic_init(&node->enqidx, 1);
        atomic_store_explicit(&node->items[0], initial_item, memory_order_relaxed);
        start_idx = 1;
    } else {
        atomic_init(&node->enqidx, 0);
        start_idx = 0;
    }

    if (start_idx < FAA_BUFFER_SIZE) {
        memset((void*)&node->items[start_idx], 0,
               (FAA_BUFFER_SIZE - start_idx) * sizeof(node->items[0]));
    }

    return node;
}

FAAArrayQueue_t* faa_queue_create(int max_threads)
{
    if (max_threads <= 0) {
        fprintf(stderr, "C23 FAAQueue Error: max_threads must be > 0.\n");
        return nullptr;
    }

    FAAArrayQueue_t* q = aligned_alloc(FAA_ALIGNMENT, sizeof(FAAArrayQueue_t));
    if (!q) {
        return nullptr;
    }

    q->max_threads = max_threads;

    q->taken_sentinel = aligned_alloc(128, 128);
    if (!q->taken_sentinel) {
        free(q);
        return nullptr;
    }

    Node_t* sentinel = create_node(nullptr);

    atomic_init(&q->head, sentinel);
    atomic_init(&q->tail, sentinel);

    q->holders = calloc(max_threads, sizeof(ThreadHolders_t));
    if (!q->holders) {
        node_reclaim(&sentinel->hp_base);
        free(q->taken_sentinel);
        free(q);
        return nullptr;
    }

    for (int i = 0; i < max_threads; i++) {
        hazptr_holder_init(&q->holders[i].head_holder);
        hazptr_holder_init(&q->holders[i].tail_holder);
        q->holders[i].last_head = nullptr;
        q->holders[i].last_tail = nullptr;
    }

    return q;
}

void faa_queue_destroy(FAAArrayQueue_t* q, void (*free_payload)(void*))
{
    if (!q) {
        return;
    }

    // Drain the queue. We use TID 0 arbitrarily, assuming quiescence and
    // max_threads > 0.
    void* item;
    while ((item = faa_queue_dequeue(q, 0)) != nullptr) {
        if (free_payload) {
            free_payload(item);
        }
    }

    Node_t* sentinel = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (sentinel) {
        // We can free it directly since we assume quiescence.
        node_reclaim(&sentinel->hp_base);
    }

    // Destroy HP holders.
    if (q->holders) {
        for (int i = 0; i < q->max_threads; i++) {
            hazptr_holder_destroy(&q->holders[i].head_holder);
            hazptr_holder_destroy(&q->holders[i].tail_holder);
        }
        free(q->holders);
    }

    // Delete the 'taken_sentinel'.
    if (q->taken_sentinel) {
        free(q->taken_sentinel);
    }

    // Finally, free the queue structure itself.
    free(q);
}

void faa_queue_enqueue(FAAArrayQueue_t* q, void* item, int tid)
{
    if (item == nullptr || item == q->taken_sentinel) {
        return;
    }

    ThreadHolders_t* ths = &q->holders[tid];
    hazptr_holder_t* h   = &ths->tail_holder;

    while (true) {
        Node_t* ltail = ths->last_tail;
        if (ltail == nullptr) {
            ltail = atomic_load_explicit(&q->tail, memory_order_acquire);
            HAZPTR_PROTECT(ltail, h, &q->tail);
            ths->last_tail = ltail;
        }

        size_t const idx = atomic_fetch_add_explicit(&ltail->enqidx, 1, memory_order_relaxed);

        if (idx >= FAA_BUFFER_SIZE) {
            Node_t* lnext = atomic_load_explicit(&ltail->next, memory_order_acquire);

            if (lnext == nullptr) {
                if (idx > FAA_BUFFER_SIZE) {
                    int spin_alloc = 0;
                    while (spin_alloc < 1024 && (lnext = atomic_load_explicit(&ltail->next, memory_order_acquire)) == nullptr) {
                        int const pause_count = (spin_alloc < 64) ? 1 : 2;
                        for (int p = 0; p < pause_count; p++) {
#if defined(__x86_64__) || defined(_M_X64)
                            __builtin_ia32_pause();
#elif defined(__aarch64__)
                            __asm__ volatile("yield" ::: "memory");
#endif
                        }
                        spin_alloc++;
                    }
                }

                if (lnext == nullptr) {
                    Node_t* new_node = create_node(item);

                    Node_t* expected_next = nullptr;
                    if (atomic_compare_exchange_strong_explicit(&ltail->next,
                                                                 &expected_next,
                                                                 new_node,
                                                                 memory_order_release,
                                                                 memory_order_relaxed)) {
                        atomic_compare_exchange_strong_explicit(&q->tail,
                                                                 &ltail,
                                                                 new_node,
                                                                 memory_order_release,
                                                                 memory_order_relaxed);
                        ths->last_tail = nullptr;
                        hazptr_reset(h, nullptr);
                        return;
                    } else {
                        node_reclaim(&new_node->hp_base);
                        lnext = atomic_load_explicit(&ltail->next, memory_order_acquire);
                    }
                }
            }

            if (lnext != nullptr) {
                Node_t* current_tail = ltail;
                atomic_compare_exchange_strong_explicit(
                    &q->tail, &current_tail, lnext, memory_order_release, memory_order_relaxed);
#if defined(__GNUC__) || defined(__clang__)
                __builtin_prefetch(lnext, 1, 3);
#endif
            }
            ths->last_tail = nullptr;
            hazptr_reset(h, nullptr);
            continue;
        }

        size_t const physical_idx = (idx * FAA_STRIDE) & (FAA_BUFFER_SIZE - 1);
        void*        expected     = nullptr;
        if (atomic_compare_exchange_strong_explicit(
                &ltail->items[physical_idx], &expected, item, memory_order_release,
                memory_order_relaxed)) {
            // Keep ltail cached in ths->last_tail to avoid re-protecting on the next call!
            return;
        }
    }
}

void* faa_queue_dequeue(FAAArrayQueue_t* q, int tid)
{
    assert(q != nullptr);
    if (tid < 0 || tid >= q->max_threads) {
        fprintf(stderr, "C23 FAAQueue Error: Invalid thread ID %d.\n", tid);
        assert(false && "Invalid TID");
        return nullptr;
    }

    ThreadHolders_t* ths   = &q->holders[tid];
    hazptr_holder_t* h     = &ths->head_holder;
    void* const      taken = q->taken_sentinel;

    while (true) {
        Node_t* lhead = ths->last_head;
        if (lhead == nullptr) {
            lhead = atomic_load_explicit(&q->head, memory_order_acquire);
            HAZPTR_PROTECT(lhead, h, &q->head);
            ths->last_head = lhead;
        }

        size_t const current_deq = atomic_load_explicit(&lhead->deqidx, memory_order_relaxed);
        size_t const enq_idx = atomic_load_explicit(&lhead->enqidx, memory_order_relaxed);
        if (current_deq >= enq_idx) {
            return nullptr;
        }

        size_t const idx = atomic_fetch_add_explicit(&lhead->deqidx, 1, memory_order_relaxed);

        if (idx >= FAA_BUFFER_SIZE) {
            Node_t* lnext = atomic_load_explicit(&lhead->next, memory_order_acquire);
            int spin_next = 0;
            while (lnext == nullptr) {
                size_t const enq_idx = atomic_load_explicit(&lhead->enqidx, memory_order_relaxed);
                if (idx >= enq_idx) {
                    break;
                }
                if (spin_next < 1024) {
                    int const pause_count = (spin_next < 64) ? 1 : 2;
                    for (int p = 0; p < pause_count; p++) {
#if defined(__x86_64__) || defined(_M_X64)
                        __builtin_ia32_pause();
#elif defined(__aarch64__)
                        __asm__ volatile("yield" ::: "memory");
#endif
                    }
                    spin_next++;
                }
                lnext = atomic_load_explicit(&lhead->next, memory_order_acquire);
            }
            if (lnext == nullptr) {
                break;
            }

            Node_t* current_tail = lhead;
            atomic_compare_exchange_strong_explicit(
                &q->tail, &current_tail, lnext, memory_order_release, memory_order_relaxed);

            if (atomic_compare_exchange_strong_explicit(
                    &q->head, &lhead, lnext, memory_order_release, memory_order_relaxed)) {
#if defined(__GNUC__) || defined(__clang__)
                __builtin_prefetch(lnext, 0, 3);
#endif
                ths->last_head = nullptr;
                hazptr_reset(h, nullptr);
                hazptr_retire(&lhead->hp_base, node_reclaim);
            } else {
                ths->last_head = nullptr;
                hazptr_reset(h, nullptr);
            }
            continue;
        }

        size_t const physical_idx = (idx * FAA_STRIDE) & (FAA_BUFFER_SIZE - 1);
        void*        item         = nullptr;
        int          spins        = 0;

        // Speculative fast-path read
        item = atomic_load_explicit(&lhead->items[physical_idx], memory_order_acquire);
        if (item == nullptr) {
            while (true) {
                item = atomic_load_explicit(&lhead->items[physical_idx], memory_order_relaxed);
                if (item != nullptr) {
                    atomic_thread_fence(memory_order_acquire);
                    break;
                }

                if (spins < 1200) {
                    int const pause_count = (spins < 64) ? 1 : (spins < 256) ? 2 : (spins < 512) ? 4 : 8;
                    for (int p = 0; p < pause_count; p++) {
#if defined(__x86_64__) || defined(_M_X64)
                        __builtin_ia32_pause();
#elif defined(__aarch64__)
                        __asm__ volatile("yield" ::: "memory");
#endif
                    }
                    spins++;
                } else {
                    item = atomic_exchange_explicit(&lhead->items[physical_idx], taken,
                                                    memory_order_acquire);
                    break;
                }
            }
        }

        if (item == nullptr || item == taken) {
            continue;
        }

        // Keep lhead cached in ths->last_head to avoid re-protecting on the next call!
        return item;
    }

    return nullptr;
}
