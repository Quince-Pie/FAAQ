#include "hp.h"

#include <stdio.h>
#include <string.h>
#include <threads.h> // C23 Thread Support Library

/*
 * External dependency: khashl.h
 * A high-performance, embeddable hash table library used for efficient
 * storage and querying of protected pointers during reclamation.
 */
#include "khashl.h"

// Initialize a khashl set specialized for storing uintptr_t keys (pointers).
KHASHL_SET_INIT(KH_LOCAL, ptr_set_t, ptr_set, uintptr_t, kh_hash_uint64, kh_eq_generic)

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
 *  | hprec_list (Scan source)                                                |
 *  | hprec_avail (Available stack - TLC miss/flush)                          |
 *  | hprec_count (Threshold calculation)                                     |
 *  | scan_set (Used only during reclamation)                                 |
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
    _Atomic(hazptr_rec_t*) hprec_avail; // Lock-free stack of available HP records.
    _Atomic(size_t)        hprec_count; // Total count of allocated HP records (H).
    ptr_set_t*             scan_set;    // Hash set used during reclamation scan.

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
} hazptr_tc_t;

// Initialize count to 0 using C23 empty braces {}.
static thread_local hazptr_tc_t tls_cache = {};

/*
 * Thread Specific Storage (TSS) key for automatic TLC cleanup.
 * Rationale: We must return cached HP records (global resources) back to the
 * domain upon thread exit. The C23 TSS API provides the necessary destructor
 * capability. We store the address of the thread's 'tls_cache' in the TSS.
 */
static tss_t     hazptr_tss_key;
static once_flag tss_init_flag = ONCE_FLAG_INIT;

// ----------------------------------------------------------------------------
// Forward Declarations (Internal)
// ----------------------------------------------------------------------------

static hazptr_rec_t* domain_acquire_hprec(hazptr_domain_t* domain);
static void
domain_release_hprec_list(hazptr_domain_t* domain, hazptr_rec_t* head, hazptr_rec_t* tail);
static void domain_do_reclamation(hazptr_domain_t* domain, hazptr_count_t claimed_count);

// ----------------------------------------------------------------------------
// Thread Local Cache (TLC) Management
// ----------------------------------------------------------------------------

/*
 * Flushes the thread local cache back to the domain's available stack.
 */
static void tlc_flush(hazptr_tc_t* tc)
{
    if (tc->count == 0) {
        return;
    }

    // We only support TLC for the default domain in this implementation.
    hazptr_domain_t* domain = &default_domain;

    /*
     * Optimization: Batch the release. We locally link the cached records
     * into a list and attach them to the domain's 'hprec_avail' stack
     * with a single atomic operation.
     */

    // Build the linked list locally.
    hazptr_rec_t* head = tc->records[0];
    hazptr_rec_t* tail = head;
    head->next_avail   = nullptr;

    for (size_t i = 1; i < tc->count; ++i) {
        hazptr_rec_t* rec = tc->records[i];
        rec->next_avail   = nullptr;
        tail->next_avail  = rec;
        tail              = rec;
    }

    // Release the entire list to the domain.
    domain_release_hprec_list(domain, head, tail);
    tc->count = 0;
}

/*
 * TSS Destructor: Called automatically by the C runtime when the thread exits.
 */
static void hazptr_tss_destructor(void* data)
{
    if (data) {
        // Data holds the address of the thread's tls_cache instance.
        tlc_flush((hazptr_tc_t*)data);
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
    // Initialize the global key if necessary (thread-safe).
    call_once(&tss_init_flag, initialize_tss);

    // Check if this thread has already registered its TLC address.
    // Using a thread_local bool is faster than repeatedly calling tss_get().
    static thread_local bool registered = false;
    if (!registered) {
        /*
         * Register the address of this thread's 'tls_cache' with the global TSS key.
         */
        if (tss_set(hazptr_tss_key, &tls_cache) != thrd_success) {
            // Failure to set the TSS value means automatic cleanup won't happen.
            fprintf(stderr,
                    "C23 Hazptr Warning: Failed to set TSS value. Automatic cleanup disabled for "
                    "this thread.\n");
        }
        registered = true;
    }
}

/*
 * Try to acquire a record from the TLC (Fast Path).
 */
static inline hazptr_rec_t* tlc_try_acquire(void)
{
    if (tls_cache.count > 0) {
        // Cache hit. Involves only thread-local operations.
        return tls_cache.records[--tls_cache.count];
    }
    return nullptr;
}

/*
 * Try to release a record to the TLC (Fast Path).
 */
static inline bool tlc_try_release(hazptr_rec_t* rec)
{
    if (tls_cache.count < HP_TLC_CAPACITY) {
        // Cache has space. Involves only thread-local operations.
        tls_cache.records[tls_cache.count++] = rec;
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Domain HP Record Management (Slow Path)
// ----------------------------------------------------------------------------

/*
 * Acquires an HP record from the domain. Used when the TLC is empty.
 */
static hazptr_rec_t* domain_acquire_hprec(hazptr_domain_t* domain)
{
    // 1. Try popping from the domain's lock-free available stack (hprec_avail).
    hazptr_rec_t* rec = atomic_load_explicit(&domain->hprec_avail, memory_order_acquire);
    while (rec) {
        hazptr_rec_t* next = rec->next_avail; // next_avail is stable while on the stack.
        if (atomic_compare_exchange_weak_explicit(
                &domain->hprec_avail,
                &rec,
                next,
                memory_order_release, // Success: Synchronizes with the push (release).
                memory_order_acquire  // Failure: Reload rec with acquire semantics.
                )) {
            rec->next_avail = nullptr;
            return rec;
        }
        // CAS failed, rec is updated by CAS, retry.
    }

    // 2. Stack empty, allocate a new record.
    // Use C23 aligned_alloc to ensure cache line alignment defined in hp.h.
    rec = aligned_alloc(HP_CACHE_LINE_SIZE, sizeof(hazptr_rec_t));
    if (!rec) {
        // Allocation failure is treated as fatal.
        // Failure to acquire protection guarantees future corruption.
        fprintf(stderr, "C23 Hazptr Fatal Error: OOM when allocating hazptr_rec_t.\n");
        abort();
    }

    // 3. Initialize the record.
    atomic_init(&rec->ptr, nullptr);
    rec->domain     = domain;
    rec->next_avail = nullptr;

    // 4. Add to the global hprec_list (required for scanning).
    hazptr_rec_t* head = atomic_load_explicit(&domain->hprec_list, memory_order_relaxed);
    do {
        rec->next = head;
        /* memory_order_release ensures the initialization of the record is
         * visible before it is added to the list. Synchronizes with
         * memory_order_acquire load (L3) in the reclamation scan. */
    } while (!atomic_compare_exchange_weak_explicit(&domain->hprec_list,
                                                    &head,
                                                    rec,
                                                    memory_order_release, // Success
                                                    memory_order_relaxed  // Failure
                                                    ));

    // 5. Update the count. Used for dynamic threshold calculation.
    // Acq_rel ensures synchronization with threshold calculation loads.
    atomic_fetch_add_explicit(&domain->hprec_count, 1, memory_order_acq_rel);
    return rec;
}

/*
 * Releases a list of records (linked via next_avail) back to the domain.
 */
static void
domain_release_hprec_list(hazptr_domain_t* domain, hazptr_rec_t* head, hazptr_rec_t* tail)
{
    assert(tail != nullptr && tail->next_avail == nullptr);

    // Push the entire list onto the lock-free available stack (hprec_avail).
    hazptr_rec_t* old_head = atomic_load_explicit(&domain->hprec_avail, memory_order_relaxed);
    do {
        tail->next_avail = old_head;
        /* memory_order_release ensures the list linkage is visible before
         * the stack head is updated. Synchronizes with the pop in domain_acquire_hprec. */
    } while (!atomic_compare_exchange_weak_explicit(&domain->hprec_avail,
                                                    &old_head,
                                                    head,
                                                    memory_order_release, // Success
                                                    memory_order_relaxed  // Failure
                                                    ));
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
     * Simple hash based on address bits. We shift right to ignore low-order
     * alignment bits (e.g., 4 bits ignores the lowest 16 bytes), which often
     * lack entropy. The mask ensures the result is within bounds.
     */
    return ((uintptr_t)ptr >> 4) & (HP_NUM_SHARDS - 1);
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
 * 2. Initialization: Lazily initialize the scan set. Handle OOM gracefully.
 * 3. Iterative Processing (The Reclamation Loop):
 *    a. Extraction: Atomically extract all retired objects from all shards (X1).
 *    b. Synchronization Fence (F2): Ensure visibility of all hazard pointers.
 *    c. Scan: Load all currently protected pointers into the hash set (L3/L4).
 *    d. Match and Reclaim: Reclaim safe objects, preserve protected ones.
 *    e. Restoration: Return protected objects back to the domain (Shard 0 optimization).
 *    f. Accounting: Adjust the global 'retired_count' based on the net change.
 *    g. Progress Check: Check threshold again (CAS Handoff) and verify all shards
 *       are empty before exiting the lock. Guarantees progress.
 */
static void domain_do_reclamation(hazptr_domain_t* domain, hazptr_count_t claimed_count)
{
    // 1. Serialization
    /*
     * Attempt to acquire the reclamation lock. If exchange returns true (the previous value),
     * another thread is already reclaiming. memory_order_acquire ensures we observe
     * changes made by the previous reclaimer.
     */
    if (atomic_exchange_explicit(&domain->reclaiming, true, memory_order_acquire)) {
        /*
         * Contention: Another thread is active. We must return the claimed count
         * back to the global pool so the active thread can process it later.
         */
        if (claimed_count != 0) {
            atomic_fetch_add_explicit(&domain->retired_count, claimed_count, memory_order_acq_rel);
        }
        return;
    }

    // --- We hold the reclamation lock ---

    // 2. Initialization
    if (domain->scan_set == nullptr) {
        // Lazy initialization of the scan set (hash table).
        domain->scan_set = ptr_set_init();
        if (!domain->scan_set) {
            /*
             * [Refinement]: OOM Handling during initialization.
             * We cannot proceed with scanning. Instead of aborting the process,
             * we gracefully abort the reclamation attempt: return the claimed
             * count and release the lock. This prioritizes system availability.
             */
            if (claimed_count != 0) {
                atomic_fetch_add_explicit(
                    &domain->retired_count, claimed_count, memory_order_acq_rel);
            }
            // Release the lock before returning.
            atomic_store_explicit(&domain->reclaiming, false, memory_order_release);
            fprintf(
                stderr,
                "C23 Hazptr Warning: OOM during scan_set initialization. Reclamation aborted.\n");
            return;
        }
    }

    ptr_set_t*     protected_set = domain->scan_set;
    hazptr_count_t rcount        = claimed_count; // Local tracker for net change (balance).

    // 3. Iterative Processing (The Reclamation Loop)
    while (true) {
        hazptr_obj_t* retired_lists[HP_NUM_SHARDS];
        bool          extracted_any = false;

        // 3a. Extraction (X1)
        for (int i = 0; i < HP_NUM_SHARDS; ++i) {
            hazptr_shard_t* shard = &domain->shards[i];
            /*
             * Atomically swap the head with nullptr.
             * Acquire ensures visibility of items pushed with release (in hazptr_retire, S2).
             */
            retired_lists[i]
                = atomic_exchange_explicit(&shard->retired_head, nullptr, memory_order_acquire);
            if (retired_lists[i]) {
                extracted_any = true;
            }
        }

        if (extracted_any) {
            // 3b. Synchronization Fence (F2).
            /*
             * This SeqCst fence synchronizes with F1 (HAZPTR_PROTECT) and F3 (hazptr_retire).
             * It ensures that we observe all hazard pointers that were set by
             * other threads before this fence executes in the global SeqCst order.
             */
            atomic_thread_fence(memory_order_seq_cst);

            // 3c. Scan
            // Clear the set from the previous iteration.
            ptr_set_clear(protected_set);
            // Load the HP list head (L3). Acquire ensures we see the list structure correctly
            // (synchronizes with record allocation release).
            hazptr_rec_t* rec = atomic_load_explicit(&domain->hprec_list, memory_order_acquire);
            while (rec) {
                // Load HP value (L4). Acquire synchronizes with the release store (S1) in
                // hazptr_reset.
                void const* ptr = atomic_load_explicit(&rec->ptr, memory_order_acquire);
                if (ptr) {
                    int absent;
                    /*
                     * CRITICAL: OOM during insertion (ptr_set_put).
                     * If the hash table needs to resize and fails due to OOM, the scan will be
                     * incomplete, leading to premature reclamation (corruption).
                     * We rely on the khashl implementation being configured to abort() on
                     * allocation failure, which is the safest behavior in this critical scenario.
                     */
                    ptr_set_put(protected_set, (uintptr_t)ptr, &absent);
                }
                rec = rec->next;
            }

            // 3d. Match and Reclaim
            hazptr_obj_t* remaining_head = nullptr;
            hazptr_obj_t* remaining_tail = nullptr;

            for (int i = 0; i < HP_NUM_SHARDS; ++i) {
                hazptr_obj_t* current = retired_lists[i];
                while (current) {
                    hazptr_obj_t* next = current->next_retired;

                    // Check if the pointer exists in the set (kh_end means not found).
                    if (ptr_set_get(protected_set, (uintptr_t)current) < kh_end(protected_set)) {
                        // Protected: Keep it. Prepare for restoration.
                        current->next_retired = nullptr;
                        if (!remaining_head) {
                            remaining_head = current;
                            remaining_tail = current;
                        } else {
                            remaining_tail->next_retired = current;
                            remaining_tail               = current;
                        }
                    } else {
                        // Safe to reclaim.
                        if (current->reclaim) {
                            current->reclaim(current);
                        }
                        /*
                         * Adjust the local balance. This can cause 'rcount' to become
                         * negative if we reclaim more items than initially claimed.
                         */
                        rcount--;
                    }
                    current = next;
                }
            }

            // 3e. Restoration
            /*
             * Optimization: Restore remaining objects to a single shard (Shard 0).
             * This minimizes the number of atomic operations required for restoration.
             */
            if (remaining_head) {
                hazptr_shard_t* shard0 = &domain->shards[0];
                hazptr_obj_t*   head
                    = atomic_load_explicit(&shard0->retired_head, memory_order_relaxed);
                do {
                    remaining_tail->next_retired = head;
                } while (!atomic_compare_exchange_weak_explicit(
                    &shard0->retired_head,
                    &head,
                    remaining_head,
                    memory_order_release, // Success: Make restored items visible.
                    memory_order_relaxed  // Failure: Reload head.
                    ));
                // Note: We do not update the global retired_count here; it's handled by 'rcount'.
            }
        }

        // 3f. Accounting
        /*
         * Apply the net change (claimed - reclaimed) back to the global count.
         */
        if (rcount != 0) {
            atomic_fetch_add_explicit(&domain->retired_count, rcount, memory_order_acq_rel);
        }

        // 3g. Progress Check
        // Check if the threshold is met again due to new arrivals.
        rcount = domain_check_threshold(domain);
        if (rcount == 0) {
            /*
             * Progress Assurance: The count is below the threshold. However, we must
             * ensure all shards are truly empty before exiting the serialized phase.
             * Items might have been added to shards after the count was checked,
             * and might linger if the retirement rate drops.
             */
            bool done = true;
            for (int i = 0; i < HP_NUM_SHARDS; ++i) {
                // Acquire load to ensure we see recent additions.
                if (atomic_load_explicit(&domain->shards[i].retired_head, memory_order_acquire)
                    != nullptr) {
                    done = false;
                    break;
                }
            }
            if (done) {
                break; // Exit the reclamation loop.
            }
            // If not done but rcount is 0, loop again immediately.
        }
        // If rcount > 0, loop again to process the newly claimed batch.
    }

    // Release the reclamation lock.
    // Release semantics ensure all reclamation work (frees) is visible before the lock is released.
    atomic_store_explicit(&domain->reclaiming, false, memory_order_release);
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

    // Cache full (Slow path). Release to the domain list.
    // We use the list release function for a single item.
    rec->next_avail = nullptr;
    domain_release_hprec_list(rec->domain, rec, rec);
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

    // Synchronization Fence (F3).
    /*
     * This SeqCst fence ensures that the atomic operation which removed the
     * object from the data structure (which happened before this function call)
     * is globally visible BEFORE the object is added to the retired list (S2).
     * This synchronizes with F1 (HAZPTR_PROTECT).
     */
    atomic_thread_fence(memory_order_seq_cst);

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

    // Update centralized count. Acq_rel synchronizes with threshold checks.
    atomic_fetch_add_explicit(&domain->retired_count, 1, memory_order_acq_rel);

    // Check threshold and potentially trigger reclamation via CAS Handoff.
    hazptr_count_t rcount = domain_check_threshold(domain);
    if (rcount > 0) {
        domain_do_reclamation(domain, rcount);
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
