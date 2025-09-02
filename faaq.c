#include "faaq.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

// Reclamation function called by the HP library when the node is safe to
// delete.
static void
node_reclaim(hazptr_obj_t *obj) {
    if (!obj) {
        return;
    }
    // The hp_base object is the first member, so casting to Node_t is safe.
    Node_t *node = (Node_t *) obj;
    free(node);
}

static Node_t *
create_node(void *initial_item) {
    Node_t *node = malloc(sizeof(Node_t));
    if (!node) {
        perror("C23 FAAQueue Fatal Error: Failed to allocate Node_t");
        abort(); // Fatal error in lock-free allocation
    }

    node->hp_base = (hazptr_obj_t) {};

    atomic_init(&node->deqidx, 0);
    atomic_init(&node->next, nullptr);

    int start_idx;

    if (initial_item != nullptr) {
        atomic_init(&node->enqidx, 1);
        atomic_store_explicit(&node->items[0], initial_item, memory_order_relaxed);
        start_idx = 1;
    } else {
        atomic_init(&node->enqidx, 0);
        start_idx = 0;
    }

    for (size_t i = start_idx; i < FAA_BUFFER_SIZE; i++) {
        atomic_init(&node->items[i], nullptr);
    }

    return node;
}

FAAArrayQueue_t *
faa_queue_create(int max_threads) {
    if (max_threads <= 0) {
        fprintf(stderr, "C23 FAAQueue Error: max_threads must be > 0.\n");
        return nullptr;
    }

    FAAArrayQueue_t *q = aligned_alloc(FAA_ALIGNMENT, sizeof(FAAArrayQueue_t));
    if (!q) {
        return nullptr;
    }

    q->max_threads    = max_threads;

    q->taken_sentinel = malloc(sizeof(int));
    if (!q->taken_sentinel) {
        free(q);
        return nullptr;
    }

    Node_t *sentinel = create_node(nullptr);

    atomic_init(&q->head, sentinel);
    atomic_init(&q->tail, sentinel);

    q->holders = calloc(max_threads, sizeof(hazptr_holder_t));
    if (!q->holders) {
        node_reclaim(&sentinel->hp_base);
        free(q->taken_sentinel);
        free(q);
        return nullptr;
    }

    for (int i = 0; i < max_threads; i++) {
        hazptr_holder_init(&q->holders[i]);
    }

    return q;
}

void
faa_queue_destroy(FAAArrayQueue_t *q) {
    if (!q) {
        return;
    }

    // Drain the queue. We use TID 0 arbitrarily, assuming quiescence and
    // max_threads > 0.
    while (faa_queue_dequeue(q, 0) != nullptr)
        ;

    Node_t *sentinel = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (sentinel) {
        // We can free it directly since we assume quiescence.
        node_reclaim(&sentinel->hp_base);
    }

    // Destroy HP holders.
    if (q->holders) {
        for (int i = 0; i < q->max_threads; i++) {
            hazptr_holder_destroy(&q->holders[i]);
        }
        free(q->holders);
    }

    // Delete the 'taken_sentinel'.
    if (q->taken_sentinel) {
        free(q->taken_sentinel);
    }

    // Finally, free the queue structure itself.
    free(q);

    // Force cleanup of anything retired during the draining process.
    hazptr_cleanup();
}

void
faa_queue_enqueue(FAAArrayQueue_t *q, void *item, int tid) {
    assert(q != nullptr);
    if (tid < 0 || tid >= q->max_threads) {
        fprintf(stderr, "C23 FAAQueue Error: Invalid thread ID %d.\n", tid);
        assert(false && "Invalid TID");
        return;
    }
    if (item == nullptr) {
        fprintf(stderr, "C23 FAAQueue Error: item cannot be nullptr\n");
        abort();
    }
    if (item == q->taken_sentinel) {
        fprintf(stderr, "C23 FAAQueue Error: item matches internal sentinel\n");
        abort();
    }

    // Get the dedicated holder for this thread.
    hazptr_holder_t *h = &q->holders[tid];

    while (true) {
        Node_t *ltail;
        // 1. Protect the tail pointer using the C23 HP macro.
        HAZPTR_PROTECT(ltail, h, &q->tail);
        // 'h' now protects 'ltail'.
        size_t const idx = atomic_fetch_add_explicit(&ltail->enqidx, 1, memory_order_relaxed);

        if (idx >= FAA_BUFFER_SIZE) {
            // --- Node is full (Slow path) ---

            if (ltail != atomic_load_explicit(&q->tail, memory_order_acquire)) {
                hazptr_reset(h, nullptr);
                continue;
            }

            // Try to advance to the next node or create a new one.
            Node_t *lnext = atomic_load_explicit(&ltail->next, memory_order_acquire);

            if (lnext == nullptr) {
                // No next node. Create one with the item pre-filled.
                Node_t *new_node      = create_node(item);

                Node_t *expected_next = nullptr;
                if (atomic_compare_exchange_weak_explicit(
                        &ltail->next, &expected_next, new_node, memory_order_release, memory_order_relaxed
                    )) {
                    // Success: Attached new node. Now try to swing the tail (helping).
                    atomic_compare_exchange_weak_explicit(
                        &q->tail,
                        &ltail, // Expected value
                        new_node,
                        memory_order_release,
                        memory_order_relaxed
                    );

                    // Clear hazard pointer and return.
                    hazptr_reset(h, nullptr);
                    return;
                } else {
                    // CAS failed, someone else added a node first.
                    // Free the unused node we created.
                    node_reclaim(&new_node->hp_base);
                    // Continue the loop to retry.
                }
            } else {
                atomic_compare_exchange_weak_explicit(
                    &q->tail, &ltail, lnext, memory_order_release, memory_order_relaxed
                );
            }
            // Must retry the enqueue operation. Reset HP before retry.
            hazptr_reset(h, nullptr);
            continue;
        }

        // --- We have a valid index (Fast path) ---

        // 3. Try to store the item in the claimed slot.
        void *expected = nullptr;
        if (atomic_compare_exchange_strong_explicit(
                &ltail->items[idx], &expected, item, memory_order_release, memory_order_relaxed
            )) {
            // Success! Item enqueued.
            hazptr_reset(h, nullptr);
            return;
        }

        // If CAS fails (handled by retrying). Reset HP before retry.
        hazptr_reset(h, nullptr);
    }
}

void *
faa_queue_dequeue(FAAArrayQueue_t *q, int tid) {
    // Input validation.
    assert(q != nullptr);
    if (tid < 0 || tid >= q->max_threads) {
        fprintf(stderr, "C23 FAAQueue Error: Invalid thread ID %d.\n", tid);
        assert(false && "Invalid TID");
        return nullptr;
    }

    hazptr_holder_t *h     = &q->holders[tid];
    void *const      taken = q->taken_sentinel;

    while (true) {
        Node_t *lhead;
        // 1. Protect the head pointer.
        HAZPTR_PROTECT(lhead, h, &q->head);
        // 'h' now protects 'lhead'.

        // Preliminary check if the queue might be empty.
        // Acquire loads ensure visibility of concurrent enqueues.
        size_t  deq_idx = atomic_load_explicit(&lhead->deqidx, memory_order_acquire);
        size_t  enq_idx = atomic_load_explicit(&lhead->enqidx, memory_order_acquire);
        Node_t *lnext   = atomic_load_explicit(&lhead->next, memory_order_acquire);

        // If the current node seems empty AND there is no next node, the queue is
        // likely empty.
        if (deq_idx >= enq_idx && lnext == nullptr) {
            // Appears empty and is the last node.
            break;
        }

        // 2. Claim an index using FAA.
        size_t const idx = atomic_fetch_add_explicit(&lhead->deqidx, 1, memory_order_relaxed);

        if (idx >= FAA_BUFFER_SIZE) {
            // --- Node has been drained (Slow path) ---

            // Reload next pointer (it might have appeared since the initial check if
            // we didn't break).
            lnext = atomic_load_explicit(&lhead->next, memory_order_acquire);

            if (lnext == nullptr) {
                // Still no next node. Queue is empty.
                break;
            }

            // Try to advance the head pointer.
            // We must use strong CAS here to ensure exactly one thread retires the
            // node. lhead is updated by CAS on failure.
            if (atomic_compare_exchange_strong_explicit(
                    &q->head, &lhead, lnext, memory_order_release, memory_order_relaxed
                )) {
                // Success: Head advanced. We are responsible for retiring the old head
                // (lhead).

                // CRITICAL: We reset the hazard pointer BEFORE retiring the object
                // it was protecting. This allows prompt reclamation.
                hazptr_reset(h, nullptr);

                // Retire the old head node using the HP library.
                hazptr_retire(&lhead->hp_base, node_reclaim);
            } else {
                // CAS failed. Reset HP before retrying.
                hazptr_reset(h, nullptr);
            }
            // Retry the loop with the (potentially new) head.
            continue;
        }

        // --- We have a valid index (Fast path) ---

        // 3. Retrieve the item and mark the slot as taken simultaneously using
        // Exchange.
        void *item = atomic_exchange_explicit(&lhead->items[idx], taken,
                                              memory_order_acquire); // NEW

        if (item == nullptr) {
            // The slot was empty. The enqueuer claimed the slot (FAA) but hasn't
            // stored the item (CAS) yet. We must retry the dequeue operation. Reset
            // HP before retry.
            hazptr_reset(h, nullptr);
            thrd_yield();
            continue;
        }

        // Success! Item dequeued.
        hazptr_reset(h, nullptr);
        return item;
    }

    // Queue is empty.
    hazptr_reset(h, nullptr);
    return nullptr;
}
