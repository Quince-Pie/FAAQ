#ifndef HP_H
#define HP_H

/*
 * C23 High-Performance Hazard Pointer Implementation
 *
 * Overview:
 * This implementation aims for state-of-the-art performance by employing:
 * 1. Thread Local Caching (TLC) of HP Records for fast acquisition/release.
 * 2. Sharded Retired Lists to reduce contention during retirement.
 * 3. CAS Handoff for efficient batch reclamation triggering.
 * 4. Serialized Reclamation to prevent redundant scanning.
 * 5. Data-Oriented Design (alignment) to minimize false sharing.
 */

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// ----------------------------------------------------------------------------
// Configuration Constants
// ----------------------------------------------------------------------------

#define HP_CACHE_LINE_SIZE                                                                         \
    64 // Assumed cache line size (e.g., x86_64, ARMv8). Used to prevent false sharing.
#define HP_TLC_CAPACITY 8        // Capacity of the Thread Local Cache for HP records.
#define HP_NUM_SHARDS 8          // Number of retired list shards.
#define HP_RCOUNT_THRESHOLD 1000 // Base threshold for reclamation (R).
#define HP_HCOUNT_MULTIPLIER 2   // Dynamic threshold multiplier (K). Threshold = max(R, H*K).
#define HP_TLC_BATCH_SIZE 16     // Retirement count batch size for TLS batching.

// Constraint: HP_NUM_SHARDS must be a power of 2 for efficient indexing via masking.
static_assert((HP_NUM_SHARDS > 0) && ((HP_NUM_SHARDS & (HP_NUM_SHARDS - 1)) == 0),
              "HP_NUM_SHARDS must be a power of 2");

// ----------------------------------------------------------------------------
// Forward Declarations and Types
// ----------------------------------------------------------------------------

typedef struct hazptr_obj    hazptr_obj_t;
typedef struct hazptr_domain hazptr_domain_t;
typedef struct hazptr_rec    hazptr_rec_t;

/*
 * Type for the centralized retired count. It must be signed (int64_t),
 * as the accounting mechanism during reclamation allows it to become
 * transiently negative if a thread reclaims more than it initially claimed.
 * C23 mandates two's complement representation.
 */
typedef int64_t hazptr_count_t;

typedef void (*hazptr_reclaim_fn)(hazptr_obj_t*);

/*
 * Base structure for objects managed by hazard pointers.
 * This structure is typically embedded within the data structure being protected.
 */
struct hazptr_obj
{
    hazptr_obj_t*     next_retired; // Link for the retired list (sharded).
    hazptr_reclaim_fn reclaim;      // Function pointer for final deletion.
};

/**
 * @brief Retires an object. It will be reclaimed when safe.
 *
 * CRITICAL: The object MUST be atomically removed from the data structure
 * BEFORE this function is called. Synchronization is enforced internally.
 *
 * @param obj The object to retire.
 * @param reclaim_fn The function to call for deletion.
 */
void hazptr_retire(hazptr_obj_t* obj, hazptr_reclaim_fn reclaim_fn);

/**
 * @brief Manually triggers reclamation on the default domain.
 *
 * Attempts to reclaim all currently retired objects. Useful for shutdown
 * procedures or testing. This may block if another thread is currently reclaiming.
 */
void hazptr_cleanup(void);

/*
 * Hazard Pointer Record. Stores a single protected pointer.
 *
 * Layout Invariant: The structure is aligned to a cache line boundary, and
 * the 'ptr' field is placed first and aligned to minimize false sharing
 * between threads writing to their respective records or during scanning.
 */
struct hazptr_rec
{
    alignas(HP_CACHE_LINE_SIZE) _Atomic(void const*) ptr;
    _Atomic(bool) active; // True when the record is in use. Solves ABA.

    /* Metadata (COLD fields) */
    hazptr_rec_t* next; // Link for the global list of all records (hprec_list).
    hazptr_domain_t* domain;
};

/*
 * Hazard Pointer Holder. Used by reader threads to manage the lifecycle
 * of a single acquired hazptr_rec_t. Typically resides on the stack.
 */
typedef struct
{
    hazptr_rec_t* hprec;
} hazptr_holder_t;

/**
 * @brief Initializes a holder and acquires an HP record.
 *
 * Fast Path: Acquires from the Thread Local Cache (TLC).
 * Slow Path: Acquires from the domain or allocates a new one.
 *
 * Ensures the thread is registered for automatic cleanup (TLC flush) on exit.
 */
void hazptr_holder_init(hazptr_holder_t* h);

/**
 * @brief Destroys the holder and releases the HP record.
 *
 * Fast Path: Releases to the TLC.
 * Slow Path: Releases to the domain.
 *
 * The protected pointer is automatically reset to C23 nullptr.
 */
void hazptr_holder_destroy(hazptr_holder_t* h);

/**
 * @brief Sets the protection to a specific pointer (or C23 nullptr to reset).
 *
 * Uses memory_order_release to ensure visibility to reclaiming threads.
 */
static inline void hazptr_reset(hazptr_holder_t* h, void const* ptr)
{
    if (h && h->hprec) {
        /* S1 (Store 1) in the synchronization protocol.
         * Release semantics ensure that operations preceding this store
         * (e.g., accesses to a previously protected object) are not reordered
         * past it, and that this write is visible to the reclaimer's acquire load (L4).
         */
        atomic_store_explicit(&h->hprec->ptr, ptr, memory_order_release);
    }
}

/*
 * ----------------------------------------------------------------------------
 * Hazard Pointer Synchronization Protocol (The Core Mechanism)
 * ----------------------------------------------------------------------------
 *
 * The safety of hazard pointers relies on strict synchronization between the
 * Protector (P), the Reclaimer (R), and the Retiree (T). We use SeqCst fences
 * to establish a Single Total Order for robust portability across all C23 platforms.
 *
 * Protector (HAZPTR_PROTECT):
 *   L1: Load shared pointer (Relaxed).
 *   S1: Store pointer to HP record (Release). (In hazptr_reset)
 *   F1: Fence (SeqCst).
 *   L2: Validate by reloading shared pointer (Acquire).
 *
 * Retiree (hazptr_retire):
 *   (Prior: Remove object from data structure)
 *   S2: Push onto retired list (Release).
 *   NOTE: F3 (SeqCst fence) is deliberately omitted. The release store in S2
 *   synchronizes-with the acquire exchange in X1, forming a happens-before edge
 *   that makes F3 mathematically redundant (saving a costly barrier on hot path).
 *
 * Reclaimer (domain_do_reclamation):
 *   X1: Extract retired lists (Acquire).
 *   F2: Fence (SeqCst).
 *   L3: Load HP list structure (Acquire).
 *   L4: Load HP values during scan (Acquire).
 *
 * Safety Proof Sketch:
 *
 * Case 1: F1 happens-before F2 (in the SeqCst total order).
 *   S1 -> F1 -> F2 -> L4. The protection (S1) is visible to the scan (L4). The object is safe.
 *
 * Case 2: F2 happens-before F1.
 *   X1 -> F2 -> F1 -> L2. The extraction (X1) happens before the validation (L2).
 *   If validation (L2) succeeds, the object must have been present in the data
 *   structure at the time of L2. Therefore, its retirement (S2) must have occurred
 *   after X1. The object is not in the batch being reclaimed. The object is safe.
 *
 * This guarantees that an object is reclaimed if and only if no hazard pointer
 * refers to it at the time of the scan (synchronized by F2).
 * ----------------------------------------------------------------------------
 */

/**
 * @brief Macro for the standard Load-Protect-Validate pattern.
 *
 * Safely loads a pointer from an atomic source and protects it.
 *
 * @param result_ The variable to store the protected pointer (T*).
 * @param h_ Pointer to the hazptr_holder_t.
 * @param src_ptr_ Pointer to the atomic source pointer (_Atomic(T*)*).
 */
/* Utilizes C23 typeof_unqual for robust type inference. */
#define HAZPTR_PROTECT(result_, h_, src_ptr_)                                                      \
    do {                                                                                           \
        /* Determine the raw pointer type (e.g., T*) from the atomic source. */                    \
        typeof_unqual(*(src_ptr_)) p_;                                                             \
        typeof_unqual(*(src_ptr_)) v_;                                                             \
                                                                                                   \
        /* Initial relaxed load optimization (L1). Provides a starting value. */                   \
        p_ = atomic_load_explicit((src_ptr_), memory_order_relaxed);                               \
                                                                                                   \
        while (true) {                                                                             \
            /* 1. Protect the observed value (S1). Uses memory_order_release internally. */        \
            hazptr_reset((h_), p_);                                                                \
                                                                                                   \
            /* 2. Synchronization Fence (F1). */                                                   \
            /* This SeqCst fence ensures that the HP write (S1) is globally visible */             \
            /* before the validation load (L2) executes. It prevents Store-Load reordering */      \
            /* and synchronizes with F2. */                                                         \
            atomic_thread_fence(memory_order_seq_cst);                                             \
                                                                                                   \
            /* 3. Validate by reloading the source (L2). */                                        \
            /* Acquire ensures that subsequent dereferences of the pointer are not */              \
            /* reordered before this load, and synchronizes with the writer's update. */           \
            v_ = atomic_load_explicit((src_ptr_), memory_order_acquire);                           \
                                                                                                   \
            if (p_ == v_) {                                                                        \
                /* Success: The protected value matches the current value. */                      \
                (result_) = p_;                                                                    \
                break;                                                                             \
            }                                                                                      \
            /* Validation failed. The pointer changed; retry with the new value. */                \
            p_ = v_;                                                                               \
        }                                                                                          \
    } while (0)

#endif // HP_H
