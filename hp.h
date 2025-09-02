#ifndef HP_H
#define HP_H

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define HP_CACHE_LINE_SIZE   64   // Assumed cache line size for alignment
#define HP_TLC_CAPACITY      8    // Capacity of the Thread Local Cache
#define HP_NUM_SHARDS        8    // Number of retired list shards (MUST be power of 2)
#define HP_RCOUNT_THRESHOLD  1000 // Base threshold for reclamation
#define HP_HCOUNT_MULTIPLIER 2    // Dynamic threshold multiplier

// ----------------------------------------------------------------------------
// Forward Declarations and Types
// ----------------------------------------------------------------------------

typedef struct hazptr_obj    hazptr_obj_t;
typedef struct hazptr_domain hazptr_domain_t;
typedef struct hazptr_rec    hazptr_rec_t;

// Type for centralized retired count. Must be signed, as it can be transiently
// negative.
typedef int64_t              hazptr_count_t;

typedef void (*hazptr_reclaim_fn)(hazptr_obj_t *);

struct hazptr_obj {
    hazptr_obj_t     *next_retired;
    hazptr_reclaim_fn reclaim;
};

/**
 * @brief Retires an object. It will be reclaimed when safe.
 *
 * @param obj The object to retire.
 * @param reclaim_fn The function to call for deletion.
 */
void hazptr_retire(hazptr_obj_t *obj, hazptr_reclaim_fn reclaim_fn);

/**
 * @brief Manually triggers reclamation. Useful for shutdown or testing.
 */
void hazptr_cleanup(void);

struct hazptr_rec {
    alignas(HP_CACHE_LINE_SIZE) _Atomic(void const *) ptr;
    hazptr_rec_t    *next;
    hazptr_rec_t    *next_avail;
    hazptr_domain_t *domain;
};

typedef struct {
    hazptr_rec_t *hprec;
} hazptr_holder_t;

/**
 * @brief Initializes a holder and acquires an HP record (Fast path: TLC).
 *
 * Ensures the thread is registered for cleanup on exit.
 */
void hazptr_holder_init(hazptr_holder_t *h);

/**
 * @brief Destroys the holder and releases the HP record (Fast path: TLC).
 */
void hazptr_holder_destroy(hazptr_holder_t *h);

/**
 * @brief Sets the protection to a specific pointer (or nullptr to reset).
 */
static inline void
hazptr_reset(hazptr_holder_t *h, void const *ptr) {
    if (h && h->hprec) {
        // Release semantics ensure the write is visible to reclaiming threads.
        atomic_store_explicit(&h->hprec->ptr, ptr, memory_order_release);
    }
}

/**
 * @brief Macro for the standard Load-Protect-Validate pattern.
 *
 * Safely loads a pointer from an atomic source and protects it.
 *
 * Usage:
 * _Atomic(MyStruct*) shared_ptr = ...;
 * hazptr_holder_t h;
 * hazptr_holder_init(&h);
 * MyStruct* local_ptr;
 * HAZPTR_PROTECT(local_ptr, &h, &shared_ptr);
 * // local_ptr is now safe to use
 *
 * @param result_ The variable to store the protected pointer (T*).
 * @param h_ Pointer to the hazptr_holder_t.
 * @param src_ptr_ Pointer to the atomic source pointer (_Atomic(T*)*).
 */
// Utilizes C23 typeof_unqual for type safety.
#define HAZPTR_PROTECT(result_, h_, src_ptr_)                                                                          \
    do {                                                                                                               \
        /* Determine the raw pointer type (e.g., T*) */                                                                \
        typeof_unqual(*(src_ptr_)) p_;                                                                                 \
        typeof_unqual(*(src_ptr_)) v_;                                                                                 \
                                                                                                                       \
        /* Initial relaxed load optimization */                                                                        \
        p_ = atomic_load_explicit((src_ptr_), memory_order_relaxed);                                                   \
                                                                                                                       \
        while (true) {                                                                                                 \
            /* 1. Protect the observed value */                                                                        \
            hazptr_reset((h_), p_);                                                                                    \
                                                                                                                       \
            /* 2. Synchronization Fence (Light Fence - SeqCst) */                                                      \
            /* Ensures the HP write is visible before the validation load. */                                          \
            atomic_thread_fence(memory_order_seq_cst);                                                                 \
                                                                                                                       \
            /* 3. Validate by reloading the source (Acquire) */                                                        \
            v_ = atomic_load_explicit((src_ptr_), memory_order_acquire);                                               \
                                                                                                                       \
            if (p_ == v_) {                                                                                            \
                /* Success */                                                                                          \
                (result_) = p_;                                                                                        \
                break;                                                                                                 \
            }                                                                                                          \
            p_ = v_;                                                                                                   \
        }                                                                                                              \
    } while (0)

#endif // HP_H
