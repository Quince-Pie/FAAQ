#ifndef FAA_ARRAY_QUEUE_HP_H
#define FAA_ARRAY_QUEUE_HP_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>

#include "hp.h"

constexpr static size_t FAA_BUFFER_SIZE = 1024;

constexpr static size_t FAA_ALIGNMENT   = 128;

typedef struct FAA_Node Node_t;

struct FAA_Node {
    // HP reclaimation data
    hazptr_obj_t hp_base;

    alignas(FAA_ALIGNMENT) _Atomic(size_t) deqidx;

    alignas(FAA_ALIGNMENT) _Atomic(size_t) enqidx;

    alignas(FAA_ALIGNMENT) _Atomic(Node_t *) next;

    alignas(FAA_ALIGNMENT) _Atomic(void *) items[FAA_BUFFER_SIZE];
};

typedef struct {
    alignas(FAA_ALIGNMENT) _Atomic(Node_t *) head;
    alignas(FAA_ALIGNMENT) _Atomic(Node_t *) tail;

    // Sentinel value to mark a dequeued slot (a unique, non-null pointer).
    void            *taken_sentinel;

    // Hazard Pointer management, indexed by thread ID (tid).
    int              max_threads;
    hazptr_holder_t *holders;

} FAAArrayQueue_t;

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

/**
 * @brief Creates and initializes a new FAA Array Queue.
 *
 * @param max_threads The maximum number of threads that will access the queue.
 * Must be > 0.
 * @return A pointer to the new queue, or nullptr on failure.
 */
[[nodiscard("Queue creation failure must be handled")]]
FAAArrayQueue_t *faa_queue_create(int max_threads);

/**
 * @brief Destroys the queue. Assumes the queue is quiescent (no other threads
 * accessing it).
 *
 * Drains the queue and frees all associated memory.
 *
 * @param q Pointer to the queue structure.
 */
void             faa_queue_destroy(FAAArrayQueue_t *q);

/**
 * @brief Enqueues an item into the queue.
 *
 * @param q Pointer to the queue structure.
 * @param item The item to enqueue (must not be nullptr or the internal
 * sentinel).
 * @param tid The thread ID of the caller (0 <= tid < max_threads).
 */
void             faa_queue_enqueue(FAAArrayQueue_t *q, void *item, int tid);

/**
 * @brief Dequeues an item from the queue.
 *
 * @param q Pointer to the queue structure.
 * @param tid The thread ID of the caller (0 <= tid < max_threads).
 * @return The dequeued item, or nullptr if the queue is empty.
 */
void            *faa_queue_dequeue(FAAArrayQueue_t *q, int tid);

#endif // FAA_ARRAY_QUEUE_HP_H
