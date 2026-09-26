#ifndef FAA_ARRAY_QUEUE_HP_H
#define FAA_ARRAY_QUEUE_HP_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "hp.h"

// Node capacity. Overridable (-DFAA_BUFFER_SIZE_CFG=4) so that fuzz and
// model-checking builds cross a node boundary every few operations instead of
// every 512. Must be a power of two.
#ifndef FAA_BUFFER_SIZE_CFG
#define FAA_BUFFER_SIZE_CFG 512
#endif
constexpr static size_t FAA_BUFFER_SIZE = FAA_BUFFER_SIZE_CFG;
static_assert(FAA_BUFFER_SIZE > 0 && (FAA_BUFFER_SIZE & (FAA_BUFFER_SIZE - 1)) == 0,
              "FAA_BUFFER_SIZE must be a power of two");

constexpr static size_t FAA_ALIGNMENT = 128;

// Stride for mapping logical indices to physically distant array slots.
// Any odd stride is coprime with a power-of-two size, so
// idx -> (idx * FAA_STRIDE) & (FAA_BUFFER_SIZE - 1) is a bijection. 11 * 8 =
// 88 bytes between consecutive indices, eliminating cache-line bouncing on
// adjacent FAA slots.
constexpr static size_t FAA_STRIDE = 11;
static_assert(FAA_STRIDE % 2 == 1, "FAA_STRIDE must be odd (coprime with FAA_BUFFER_SIZE)");

typedef struct FAA_Node Node_t;

struct FAA_Node
{
    // HP reclaimation data
    hazptr_obj_t hp_base;

    alignas(64) _Atomic(size_t) deqidx;

    alignas(64) _Atomic(void*) items[FAA_BUFFER_SIZE];

    alignas(64) _Atomic(size_t) enqidx;

    alignas(64) _Atomic(Node_t*) next;
};

typedef struct
{
    alignas(FAA_ALIGNMENT) _Atomic(Node_t*) head;
    alignas(FAA_ALIGNMENT) _Atomic(Node_t*) tail;

    // Process-unique identity, never reused. Every thread keeps a small
    // thread-local cache of (queue id -> protected head/tail node); keying it
    // by id rather than by address means a queue allocated at a destroyed
    // queue's address can never match a stale entry.
    alignas(FAA_ALIGNMENT) uint64_t id;
} FAAArrayQueue_t;

// ----------------------------------------------------------------------------
// Public API
//
// Threading model: any number of threads may call enqueue/dequeue on any queue
// concurrently; no thread registration and no thread ids are needed. Each
// thread lazily attaches per-queue state (two hazard pointers) on first use,
// caches it for its FAAQ_TLS_SLOTS most recently used queues, and releases it
// automatically when the thread exits (C11/C23 tss destructor) or when the
// queue is destroyed.
// ----------------------------------------------------------------------------

/**
 * @brief Creates and initializes a new FAA Array Queue.
 *
 * @return A pointer to the new queue, or nullptr on allocation failure.
 */
[[nodiscard("Queue creation failure must be handled")]]
FAAArrayQueue_t* faa_queue_create(void);

/**
 * @brief Destroys the queue. Assumes the queue is quiescent (no other threads
 * accessing it).
 *
 * Drains the queue, passing every remaining item to free_payload (if
 * non-null), then frees all associated memory.
 *
 * @param q Pointer to the queue structure (nullptr is a no-op).
 */
void faa_queue_destroy(FAAArrayQueue_t* q, void (*free_payload)(void*));

/**
 * @brief Enqueues an item into the queue. Lock-free.
 *
 * @param q Pointer to the queue structure.
 * @param item The item to enqueue. nullptr is ignored (nullptr means "empty"
 * to dequeue).
 */
void faa_queue_enqueue(FAAArrayQueue_t* q, void* item);

/**
 * @brief Dequeues an item from the queue. Lock-free.
 *
 * @param q Pointer to the queue structure.
 * @return The dequeued item, or nullptr if the queue is empty.
 */
[[nodiscard("A discarded dequeue result is a lost item")]]
void* faa_queue_dequeue(FAAArrayQueue_t* q);

#endif // FAA_ARRAY_QUEUE_HP_H
