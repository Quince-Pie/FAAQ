#include "hp.h"

#include <stdio.h>
#include <string.h>
#include <threads.h> // C23 Thread Support Library

/*
 * Comparison function for qsort/bsearch on pointer arrays.
 * Used during reclamation to efficiently match retired objects against
 * the set of currently protected pointers.
 */
static int ptr_compare(const void* a, const void* b)
{
    uintptr_t pa = (uintptr_t)*(void const* const*)a;
    uintptr_t pb = (uintptr_t)*(void const* const*)b;
    return (pa > pb) - (pa < pb);
}

// ----------------------------------------------------------------------------
// Domain Structure Definitions
// ----------------------------------------------------------------------------

/*
 * Shard structure for the retired lists.
 * To minimize contention during retirement (the hot path), retired objects
 * are distributed across multiple lists (shards).
 */
typedef struct
{
    /*
     * Alignment ensures that the head pointers of adjacent shards do not
     * reside on the same cache line, preventing false sharing.
     */
    alignas(HP_CACHE_LINE_SIZE) _Atomic(hazptr_obj_t*) retired_head;
} hazptr_shard_t;

/*
 * Hazard Pointer Domain. Manages the lifecycle of HP records and retired objects.
 *
 * Memory Layout Strategy (Data-Oriented Design):
 * Data within the domain is segregated based on access frequency and contention
 * levels to optimize cache utilization and minimize coherence traffic.
 *
 *  +-------------------------------------------------------------------------+
 *  | hazptr_domain_t                                                         |
 *  |-------------------------------------------------------------------------|
 *  | Group 1: Cold/Moderate Data (HP Record Management)                      |
 *  | hprec_list (Scan source, acquire via active flag)                        |
 *  | hprec_count (Threshold calculation)                                     |
 *  +-------------------------------------------------------------------------+
 *  | Cache Line Boundary (alignas)                                           |
 *  +-------------------------------------------------------------------------+
 *  | Group 2: Hot Data (Reclamation Control)                                 |
 *  | retired_count (High contention: updated on every retirement)            |
 *  +-------------------------------------------------------------------------+
 *  | Cache Line Boundary (alignas)                                           |
 *  +-------------------------------------------------------------------------+
 *  | Group 3: Moderate Data (Reclamation Lock)                               |
 *  | reclaiming (Updated when threshold is reached)                          |
 *  +-------------------------------------------------------------------------+
 *  | Cache Line Boundary (alignas for the following array)                   |
 *  +-------------------------------------------------------------------------+
 *  | Group 4: Hot Data (Sharded Retired Lists)                               |
 *  | shards[0] (Isolated Cache Line)                                         |
 *  | shards[1] (Isolated Cache Line)                                         |
 *  | ...                                                                     |
 *  +-------------------------------------------------------------------------+
 */
struct hazptr_domain
{
    // --- Group 1: HP Record Management ---
    _Atomic(hazptr_rec_t*) hprec_list;  // Global list of all allocated HP records.
    _Atomic(size_t)        hprec_count; // Total count of allocated HP records (H).

    // --- Group 2: Reclamation Control (High Contention) ---

    /* Isolate the highly contended counter on its own cache line. */
    alignas(HP_CACHE_LINE_SIZE) _Atomic(hazptr_count_t) retired_count;

    // --- Group 3: Reclamation Lock (Moderate Contention) ---

    /* Isolate the reclamation lock. */
    alignas(HP_CACHE_LINE_SIZE) _Atomic(bool) reclaiming;

    // --- Group 4: Sharded Retired Lists (Hot) ---
    /* Ensure the array starts on a new cache line. */
    alignas(HP_CACHE_LINE_SIZE) hazptr_shard_t shards[HP_NUM_SHARDS];
};

// Global default domain. Statically initialized to zero by the C standard.
static hazptr_domain_t default_domain = {};

// ----------------------------------------------------------------------------
// Thread Local Storage (TLS) and Cleanup
// ----------------------------------------------------------------------------

/*
 * Thread Local Cache (TLC) for HP Records.
 * Provides fast-path acquisition/release by avoiding atomic operations on the
 * domain's shared 'hprec_avail' stack.
 */
typedef struct
{
    hazptr_rec_t* records[HP_TLC_CAPACITY];
    size_t        count;
    size_t        retired_batch; // Batched retirement count to reduce contention.
} hazptr_tc_t;

/*
 * Pointer to the heap-allocated TLC for this thread. Using calloc + TSS destructor
 * decouples TLC lifetime from thread_local destruction order, which the C standard
 * leaves undefined relative to TSS destructors. The thread_local pointer provides
 * zero-latency access on the hot path; the TSS owns the actual memory for teardown.
 */
static thread_local hazptr_tc_t* local_tc_ptr = nullptr;

/*
 * Thread Specific Storage (TSS) key for automatic TLC cleanup.
 * Rationale: We must return cached HP records (global resources) back to the
 * domain upon thread exit. The C23 TSS API provides the necessary destructor
 * capability. We store the address of the heap-allocated TLC in the TSS.
 */
static tss_t     hazptr_tss_key;
static once_flag tss_init_flag = ONCE_FLAG_INIT;

// ----------------------------------------------------------------------------
// Forward Declarations (Internal)
// ----------------------------------------------------------------------------

static hazptr_rec_t* domain_acquire_hprec(hazptr_domain_t* domain);
static void domain_do_reclamation(hazptr_domain_t* domain, hazptr_count_t claimed_count);

// ----------------------------------------------------------------------------
// Thread Local Cache (TLC) Management
// ----------------------------------------------------------------------------

/*
 * Flushes the thread local cache by marking cached records as inactive.
 * Wait-free: no atomic RMW, just release stores.
 */
static void tlc_flush(hazptr_tc_t* tc)
{
    for (size_t i = 0; i < tc->count; ++i) {
        atomic_store_explicit(&tc->records[i]->active, false, memory_order_release);
    }
    tc->count = 0;

    // Flush any batched retirement count to the global counter.
    if (tc->retired_batch > 0) {
        atomic_fetch_add_explicit(
            &default_domain.retired_count, (hazptr_count_t)tc->retired_batch, memory_order_acq_rel);
        tc->retired_batch = 0;
    }
}

/*
 * TSS Destructor: Called automatically by the C runtime when the thread exits.
 */
static void hazptr_tss_destructor(void* data)
{
    if (data) {
        tlc_flush((hazptr_tc_t*)data);
        free(data); // Safe: heap-allocated, decoupled from TLS destruction order.
    }
}

/*
 * Global initialization of the TSS key. Ensures the key is created exactly once.
 */
static void initialize_tss(void)
{
    if (tss_create(&hazptr_tss_key, hazptr_tss_destructor) != thrd_success) {
        // Failure to create a TSS key is fatal, as it leads to leaked HP records.
        fprintf(stderr, "C23 Hazptr Fatal Error: Failed to create TSS key.\n");
        abort();
    }
}

/*
 * Ensures the current thread is registered for cleanup.
 */
static inline void ensure_thread_registered(void)
{
    if (!local_tc_ptr) {
        call_once(&tss_init_flag, initialize_tss);

        // Heap-allocate the TLC, decoupled from undefined TLS destruction order.
        local_tc_ptr = calloc(1, sizeof(hazptr_tc_t));
        if (!local_tc_ptr) {
            fprintf(stderr, "C23 Hazptr Fatal Error: OOM for TLC allocation.\n");
            abort();
        }

        if (tss_set(hazptr_tss_key, local_tc_ptr) != thrd_success) {
            fprintf(stderr,
                    "C23 Hazptr Warning: Failed to set TSS value. Automatic cleanup disabled for "
                    "this thread.\n");
        }
    }
}

/*
 * Try to acquire a record from the TLC (Fast Path).
 */
static inline hazptr_rec_t* tlc_try_acquire(void)
{
    if (local_tc_ptr && local_tc_ptr->count > 0) {
        return local_tc_ptr->records[--local_tc_ptr->count];
    }
    return nullptr;
}

/*
 * Try to release a record to the TLC (Fast Path).
 */
static inline bool tlc_try_release(hazptr_rec_t* rec)
{
    if (local_tc_ptr && local_tc_ptr->count < HP_TLC_CAPACITY) {
        local_tc_ptr->records[local_tc_ptr->count++] = rec;
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Domain HP Record Management (Slow Path)
// ----------------------------------------------------------------------------

/*
 * Acquires an HP record from the domain. Used when the TLC is empty.
 * Scans the append-only global list for an inactive record (active == false),
 * claiming it via CAS. Allocates a new record only if none are available.
 */
static hazptr_rec_t* domain_acquire_hprec(hazptr_domain_t* domain)
{
    // 1. Scan the global list for an inactive record.
    hazptr_rec_t* rec = atomic_load_explicit(&domain->hprec_list, memory_order_acquire);
    while (rec) {
        bool expected = false;
        if (!atomic_load_explicit(&rec->active, memory_order_relaxed)
            && atomic_compare_exchange_strong_explicit(
                &rec->active, &expected, true, memory_order_acquire, memory_order_relaxed)) {
            return rec;
        }
        rec = rec->next;
    }

    // 2. No inactive record found, allocate a new one.
    rec = aligned_alloc(HP_CACHE_LINE_SIZE, sizeof(hazptr_rec_t));
    if (!rec) {
        fprintf(stderr, "C23 Hazptr Fatal Error: OOM when allocating hazptr_rec_t.\n");
        abort();
    }

    // 3. Initialize the record.
    atomic_init(&rec->ptr, nullptr);
    atomic_init(&rec->active, true);
    rec->domain = domain;

    // 4. Update the count FIRST. This guarantees that max_hps (loaded by a
    // concurrent reclaimer) is always >= the actual list length, preventing
    // scan truncation.
    atomic_fetch_add_explicit(&domain->hprec_count, 1, memory_order_acq_rel);

    // 5. Add to the global hprec_list (append-only, required for scanning).
    hazptr_rec_t* head = atomic_load_explicit(&domain->hprec_list, memory_order_relaxed);
    do {
        rec->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&domain->hprec_list,
                                                    &head,
                                                    rec,
                                                    memory_order_release,
                                                    memory_order_relaxed));
    return rec;
}

// ----------------------------------------------------------------------------
// Reclamation Mechanism
// ----------------------------------------------------------------------------

/*
 * Helper to calculate the shard index based on the pointer address.
 */
static inline size_t calc_shard(void const* ptr)
{
    /*
     * Fibonacci multiplicative hash. Shift right past 128-byte alignment zeros,
     * then multiply by the golden ratio fraction of 2^64 to scatter entropy.
     * A simple mask like (x >> 7) & 7 produces degenerate patterns for
     * sequentially allocated aligned objects (e.g., only shards 0 and 4).
     */
    uintptr_t x = (uintptr_t)ptr >> 7;
    return (size_t)((x * 11400714819323198485ULL) >> 61);
}

/*
 * Calculates the dynamic reclamation threshold.
 * Rationale: The frequency of reclamation should scale with the number of
 * active participants (H) to keep the memory footprint bounded (amortizing the O(H) scan cost).
 */
static hazptr_count_t calculate_threshold(hazptr_domain_t* domain)
{
    hazptr_count_t thresh = HP_RCOUNT_THRESHOLD;

    // Acquire load synchronizes with the hprec count increment (acq_rel) during allocation.
    size_t         hcount = atomic_load_explicit(&domain->hprec_count, memory_order_acquire);
    hazptr_count_t dynamic_thresh = (hazptr_count_t)(hcount * HP_HCOUNT_MULTIPLIER);

    // Threshold = max(RCOUNT_THRESHOLD, HCOUNT * MULTIPLIER)
    return (dynamic_thresh > thresh) ? dynamic_thresh : thresh;
}

/*
 * The CAS Handoff Mechanism.
 * Checks if the threshold is reached and attempts to claim a batch for reclamation.
 */
static hazptr_count_t domain_check_threshold(hazptr_domain_t* domain)
{
    // Acquire load to synchronize with the count increments during retirement.
    hazptr_count_t rcount = atomic_load_explicit(&domain->retired_count, memory_order_acquire);
    hazptr_count_t thresh = calculate_threshold(domain);

    while (rcount >= thresh) {
        /*
         * Attempt to atomically reset the count to 0. If successful, this
         * thread claims responsibility for 'rcount' items (the Handoff).
         * Acq_rel is used for synchronization of the RMW operation.
         */
        if (atomic_compare_exchange_weak_explicit(&domain->retired_count,
                                                  &rcount,
                                                  0,
                                                  memory_order_acq_rel, // Success: Claim the count.
                                                  memory_order_relaxed  // Failure: Reload rcount.
                                                  )) {
            // Success: we claimed 'rcount' items.
            return rcount;
        }
        // CAS failed, rcount updated. Recalculate threshold in case hprec_count changed.
        thresh = calculate_threshold(domain);
    }
    return 0;
}

/*
 * The core reclamation routine.
 *
 * Strategy Overview:
 * 1. Serialization: Ensure only one thread performs reclamation at a time.
 * 2. Iterative Processing (The Reclamation Loop):
 *    a. Extraction: Atomically extract all retired objects from all shards (X1).
 *    b. Synchronization Fence (F2): Ensure visibility of all hazard pointers.
 *    c. Scan: Load all currently protected pointers into a sorted array (L3/L4).
 *    d. Match and Reclaim: Reclaim safe objects, preserve protected ones.
 *    e. Restoration: Return protected objects back to the domain (Shard 0 optimization).
 *    f. Accounting: Adjust the global 'retired_count' based on the net change.
 *    g. Progress Check: Check threshold again (CAS Handoff) and verify all shards
 *       are empty before exiting the lock. Guarantees progress.
 */
static void domain_do_reclamation(hazptr_domain_t* domain, hazptr_count_t claimed_count)
{
    hazptr_count_t rcount = claimed_count;

    /*
     * Outer loop: handles the TOCTOU "Stranded Batch" race iteratively.
     * After releasing the reclamation lock, a concurrent thread may refund its
     * count, leaving the threshold exceeded with no active reclaimer. Instead of
     * recursing (risking stack overflow under sustained load), we loop back to
     * re-acquire the lock in O(1) stack space.
     */
    while (true) {
        // 1. Serialization
        if (atomic_exchange_explicit(&domain->reclaiming, true, memory_order_acquire)) {
            if (rcount != 0) {
                atomic_fetch_add_explicit(&domain->retired_count, rcount, memory_order_acq_rel);
            }
            return;
        }

        // --- We hold the reclamation lock ---

        // 2. Iterative Processing (The Reclamation Loop)
        while (true) {
            hazptr_obj_t* retired_lists[HP_NUM_SHARDS];
            bool          extracted_any = false;

            // 2a. Extraction (X1)
            for (int i = 0; i < HP_NUM_SHARDS; ++i) {
                hazptr_shard_t* shard = &domain->shards[i];
                retired_lists[i]
                    = atomic_exchange_explicit(&shard->retired_head, nullptr, memory_order_acquire);
                if (retired_lists[i]) {
                    extracted_any = true;
                }
            }

            if (extracted_any) {
                // 2b. Synchronization Fence (F2).
                atomic_thread_fence(memory_order_seq_cst);

                // 2c. Scan: Collect protected pointers into a sorted array.
                size_t max_hps = atomic_load_explicit(&domain->hprec_count, memory_order_acquire);
                if (max_hps == 0) {
                    max_hps = 1;
                }

                // L1 cache fast-path: stack buffer avoids malloc for the common case.
                void const*  stack_ptrs[128];
                void const** protected_ptrs = stack_ptrs;
                size_t       capacity       = 128;
                bool         on_heap        = false;

                if (max_hps > 128) {
                    protected_ptrs = malloc(max_hps * sizeof(void*));
                    if (!protected_ptrs) {
                        for (int j = 0; j < HP_NUM_SHARDS; ++j) {
                            if (!retired_lists[j]) {
                                continue;
                            }
                            hazptr_obj_t* tail = retired_lists[j];
                            while (tail->next_retired) {
                                tail = tail->next_retired;
                            }
                            hazptr_shard_t* shard0 = &domain->shards[0];
                            hazptr_obj_t*   s0head
                                = atomic_load_explicit(&shard0->retired_head, memory_order_relaxed);
                            do {
                                tail->next_retired = s0head;
                            } while (!atomic_compare_exchange_weak_explicit(
                                &shard0->retired_head, &s0head, retired_lists[j],
                                memory_order_release, memory_order_relaxed));
                        }
                        if (rcount != 0) {
                            atomic_fetch_add_explicit(
                                &domain->retired_count, rcount, memory_order_acq_rel);
                        }
                        atomic_store_explicit(&domain->reclaiming, false, memory_order_release);
                        fprintf(stderr,
                                "C23 Hazptr Warning: OOM during reclamation scan. Aborted.\n");
                        return;
                    }
                    capacity = max_hps;
                    on_heap  = true;
                }

                size_t hp_count = 0;
                hazptr_rec_t* rec
                    = atomic_load_explicit(&domain->hprec_list, memory_order_acquire);
                while (rec) {
                    void const* ptr = atomic_load_explicit(&rec->ptr, memory_order_acquire);
                    if (ptr) {
                        if (hp_count >= capacity) {
                            size_t        new_cap = capacity * 2;
                            void const**  temp    = malloc(new_cap * sizeof(void*));
                            if (!temp) {
                                if (on_heap) free(protected_ptrs);
                                for (int j = 0; j < HP_NUM_SHARDS; ++j) {
                                    if (!retired_lists[j]) {
                                        continue;
                                    }
                                    hazptr_obj_t* tail = retired_lists[j];
                                    while (tail->next_retired) {
                                        tail = tail->next_retired;
                                    }
                                    hazptr_shard_t* shard0 = &domain->shards[0];
                                    hazptr_obj_t*   s0head = atomic_load_explicit(
                                        &shard0->retired_head, memory_order_relaxed);
                                    do {
                                        tail->next_retired = s0head;
                                    } while (!atomic_compare_exchange_weak_explicit(
                                        &shard0->retired_head, &s0head, retired_lists[j],
                                        memory_order_release, memory_order_relaxed));
                                }
                                if (rcount != 0) {
                                    atomic_fetch_add_explicit(
                                        &domain->retired_count, rcount, memory_order_acq_rel);
                                }
                                atomic_store_explicit(
                                    &domain->reclaiming, false, memory_order_release);
                                fprintf(stderr,
                                        "C23 Hazptr Warning: OOM during scan growth. Aborted.\n");
                                return;
                            }
                            memcpy(temp, protected_ptrs, hp_count * sizeof(void*));
                            if (on_heap) free(protected_ptrs);
                            protected_ptrs = temp;
                            capacity       = new_cap;
                            on_heap        = true;
                        }
                        protected_ptrs[hp_count++] = ptr;
                    }
                    rec = rec->next;
                }
                qsort(protected_ptrs, hp_count, sizeof(void*), ptr_compare);

                // 2d. Match and Reclaim
                hazptr_obj_t* remaining_head = nullptr;
                hazptr_obj_t* remaining_tail = nullptr;

                for (int i = 0; i < HP_NUM_SHARDS; ++i) {
                    hazptr_obj_t* current = retired_lists[i];
                    while (current) {
                        hazptr_obj_t* next = current->next_retired;

                        void const* key = (void const*)current;
                        if (bsearch(&key, protected_ptrs, hp_count, sizeof(void*), ptr_compare)) {
                            current->next_retired = nullptr;
                            if (!remaining_head) {
                                remaining_head = current;
                                remaining_tail = current;
                            } else {
                                remaining_tail->next_retired = current;
                                remaining_tail               = current;
                            }
                        } else {
                            if (current->reclaim) {
                                current->reclaim(current);
                            }
                            rcount--;
                        }
                        current = next;
                    }
                }

                if (on_heap) {
                    free(protected_ptrs);
                }

                // 2e. Restoration
                if (remaining_head) {
                    hazptr_shard_t* shard0 = &domain->shards[0];
                    hazptr_obj_t*   head
                        = atomic_load_explicit(&shard0->retired_head, memory_order_relaxed);
                    do {
                        remaining_tail->next_retired = head;
                    } while (!atomic_compare_exchange_weak_explicit(
                        &shard0->retired_head, &head, remaining_head,
                        memory_order_release, memory_order_relaxed));
                }
            }

            // 2f. Accounting
            if (rcount != 0) {
                atomic_fetch_add_explicit(&domain->retired_count, rcount, memory_order_acq_rel);
            }

            // 2g. Progress Check
            rcount = domain_check_threshold(domain);
            if (rcount == 0) {
                break;
            }
        }

        // Release the reclamation lock.
        atomic_store_explicit(&domain->reclaiming, false, memory_order_release);

        // TOCTOU Stranded Batch Check — iterative, O(1) stack space.
        rcount = domain_check_threshold(domain);
        if (rcount == 0) {
            break; // No stranded batch; exit outer loop.
        }
        // Stranded batch detected; loop back to re-acquire the lock.
    }
}

// ----------------------------------------------------------------------------
// Public API Implementation
// ----------------------------------------------------------------------------

// --- Holder API ---

void hazptr_holder_init(hazptr_holder_t* h)
{
    // We optimize TLC for the default domain.
    hazptr_domain_t* domain = &default_domain;

    // Ensure thread cleanup mechanism is active before potentially caching records.
    ensure_thread_registered();

    // Try Thread Local Cache (Fast path).
    hazptr_rec_t* rec = tlc_try_acquire();

    if (rec == nullptr) {
        // Cache miss (Slow path).
        rec = domain_acquire_hprec(domain);
    }

    h->hprec = rec;
    // Initialize the HP to nullptr (safe state).
    hazptr_reset(h, nullptr);
}

void hazptr_holder_destroy(hazptr_holder_t* h)
{
    hazptr_rec_t* rec = h->hprec;
    if (!rec) {
        return;
    }

    // Must reset the HP to nullptr before releasing the record.
    hazptr_reset(h, nullptr);

    // Try Thread Local Cache (Fast path).
    if (tlc_try_release(rec)) {
        h->hprec = nullptr;
        return;
    }

    // Cache full (Slow path). Release directly by marking inactive.
    atomic_store_explicit(&rec->active, false, memory_order_release);
    h->hprec = nullptr;
}

// --- Retirement API ---

void hazptr_retire(hazptr_obj_t* obj, hazptr_reclaim_fn reclaim_fn)
{
    if (!obj) {
        return;
    }

    obj->reclaim            = reclaim_fn;
    hazptr_domain_t* domain = &default_domain;

    // Guarantee batch flush on thread exit.
    ensure_thread_registered();

    // Push onto the appropriate shard (S2).
    size_t          shard_idx = calc_shard(obj);
    hazptr_shard_t* shard     = &domain->shards[shard_idx];

    // Standard lock-free stack push.
    hazptr_obj_t* head = atomic_load_explicit(&shard->retired_head, memory_order_relaxed);
    do {
        obj->next_retired = head;
        /* memory_order_release ensures the object initialization (reclaim_fn)
         * is visible before the push. Synchronizes with the acquire load (X1)
         * during extraction. */
    } while (!atomic_compare_exchange_weak_explicit(&shard->retired_head,
                                                    &head,
                                                    obj,
                                                    memory_order_release, // Success
                                                    memory_order_relaxed  // Failure
                                                    ));

    // TLS batching: accumulate retirements locally to reduce atomic contention
    // on the centralized retired_count.
    local_tc_ptr->retired_batch++;
    if (local_tc_ptr->retired_batch >= HP_TLC_BATCH_SIZE) {
        atomic_fetch_add_explicit(
            &domain->retired_count, (hazptr_count_t)local_tc_ptr->retired_batch, memory_order_acq_rel);
        local_tc_ptr->retired_batch = 0;

        // Check threshold and potentially trigger reclamation via CAS Handoff.
        hazptr_count_t rcount = domain_check_threshold(domain);
        if (rcount > 0) {
            domain_do_reclamation(domain, rcount);
        }
    }
}

void hazptr_cleanup(void)
{
    hazptr_domain_t* domain = &default_domain;

    /*
     * Force a reclamation cycle by manually performing a CAS handoff:
     * atomically exchange the current count with 0, claiming whatever remains.
     */
    hazptr_count_t rcount
        = atomic_exchange_explicit(&domain->retired_count, 0, memory_order_acq_rel);

    if (rcount < 0) {
        /*
         * Handle transient negative count. This can occur if another reclamation
         * finished and reduced the count below zero just before the exchange.
         * We return the negative value back to the count and proceed with rcount=0.
         */
        atomic_fetch_add_explicit(&domain->retired_count, rcount, memory_order_acq_rel);
        rcount = 0;
    }

    // Trigger reclamation. This will loop internally until all shards are empty.
    domain_do_reclamation(domain, rcount);
}
