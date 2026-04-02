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
    if (pa < pb) return -1;
    if (pa > pb) return 1;
    return 0;
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
    hazptr_obj_t* retired_list;  // Thread-local retired list.
    size_t        retired_count; // Number of retired objects in the local list.
    size_t        newly_retired; // Number of objects retired since last scan.
} hazptr_tc_t;

/*
 * Direct thread_local structure to hold the Thread Local Cache (TLC).
 * This eliminates double-pointer indirection and memory allocation overhead.
 */
static thread_local hazptr_tc_t local_tc = {};

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

    // Flush thread-local retired list to the global default domain shards.
    if (tc->retired_list) {
        hazptr_obj_t* tail = tc->retired_list;
        while (tail->next_retired) {
            tail = tail->next_retired;
        }
        hazptr_domain_t* domain = &default_domain;
        hazptr_shard_t* shard0 = &domain->shards[0];
        hazptr_obj_t* head = atomic_load_explicit(&shard0->retired_head, memory_order_relaxed);
        do {
            tail->next_retired = head;
        } while (!atomic_compare_exchange_weak_explicit(
            &shard0->retired_head, &head, tc->retired_list,
            memory_order_release, memory_order_relaxed));

        atomic_fetch_add_explicit(&domain->retired_count, tc->retired_count, memory_order_release);
        tc->retired_list = nullptr;
        tc->retired_count = 0;
    }
}


/*
 * Try to acquire a record from the TLC (Fast Path).
 */
static inline hazptr_rec_t* tlc_try_acquire(void)
{
    if (local_tc.count > 0) {
        return local_tc.records[--local_tc.count];
    }
    return nullptr;
}

/*
 * Try to release a record to the TLC (Fast Path).
 */
static inline bool tlc_try_release(hazptr_rec_t* rec)
{
    if (local_tc.count < HP_TLC_CAPACITY) {
        local_tc.records[local_tc.count++] = rec;
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
    uintptr_t x = (uintptr_t)ptr;
    // Faster, low-latency hash mixing for pointers
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return (size_t)((x * 0x2545F4914F6CDD1DULL) >> 61) % HP_NUM_SHARDS;
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
                    void const* ptr = atomic_load_explicit(&rec->ptr, memory_order_relaxed);
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
                if (hp_count > 1) {
                    qsort(protected_ptrs, hp_count, sizeof(void*), ptr_compare);
                }

                // 2d. Match and Reclaim
                hazptr_obj_t* remaining_head = nullptr;
                hazptr_obj_t* remaining_tail = nullptr;

                for (int i = 0; i < HP_NUM_SHARDS; ++i) {
                    hazptr_obj_t* current = retired_lists[i];
                    while (current) {
                        hazptr_obj_t* next = current->next_retired;
                        void const* key = (void const*)current;
                        bool found = false;

                        if (hp_count <= 16) {
                            // Direct scan is significantly faster than bsearch for small arrays
                            for (size_t k = 0; k < hp_count; ++k) {
                                if (protected_ptrs[k] == key) {
                                    found = true;
                                    break;
                                }
                            }
                        } else {
                            found = (bsearch(&key, protected_ptrs, hp_count, sizeof(void*), ptr_compare) != nullptr);
                        }

                        if (found) {
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

static void tc_do_reclamation(hazptr_tc_t* tc)
{
    hazptr_domain_t* domain = &default_domain;

    // 1. Synchronization Fence (F2).
    atomic_thread_fence(memory_order_seq_cst);

    // 2. Scan: Collect protected pointers into a sorted array.
    size_t max_hps = atomic_load_explicit(&domain->hprec_count, memory_order_acquire);
    if (max_hps == 0) {
        max_hps = 1;
    }

    void const*  stack_ptrs[128];
    void const** protected_ptrs = stack_ptrs;
    size_t       capacity       = 128;
    bool         on_heap        = false;

    if (max_hps > 128) {
        protected_ptrs = malloc(max_hps * sizeof(void*));
        if (!protected_ptrs) {
            return;
        }
        capacity = max_hps;
        on_heap  = true;
    }

    size_t hp_count = 0;
    hazptr_rec_t* rec = atomic_load_explicit(&domain->hprec_list, memory_order_acquire);
    while (rec) {
        void const* ptr = atomic_load_explicit(&rec->ptr, memory_order_relaxed);
        if (ptr) {
            if (hp_count >= capacity) {
                size_t        new_cap = capacity * 2;
                void const**  temp    = malloc(new_cap * sizeof(void*));
                if (!temp) {
                    if (on_heap) free(protected_ptrs);
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

    if (hp_count > 1) {
        qsort(protected_ptrs, hp_count, sizeof(void*), ptr_compare);
    }

    // 3. Match and Reclaim
    hazptr_obj_t* remaining_head = nullptr;
    hazptr_obj_t* remaining_tail = nullptr;
    size_t        remaining_count = 0;

    hazptr_obj_t* current = tc->retired_list;
    while (current) {
        hazptr_obj_t* next = current->next_retired;
        void const* key = (void const*)current;
        bool found = false;

        if (hp_count <= 16) {
            for (size_t k = 0; k < hp_count; ++k) {
                if (protected_ptrs[k] == key) {
                    found = true;
                    break;
                }
            }
        } else {
            found = (bsearch(&key, protected_ptrs, hp_count, sizeof(void*), ptr_compare) != nullptr);
        }

        if (found) {
            current->next_retired = nullptr;
            if (!remaining_head) {
                remaining_head = current;
                remaining_tail = current;
            } else {
                remaining_tail->next_retired = current;
                remaining_tail               = current;
            }
            remaining_count++;
        } else {
            if (current->reclaim) {
                current->reclaim(current);
            }
        }
        current = next;
    }

    if (on_heap) {
        free(protected_ptrs);
    }

    tc->retired_list = remaining_head;
    tc->retired_count = remaining_count;
}

void hazptr_retire(hazptr_obj_t* obj, hazptr_reclaim_fn reclaim_fn)
{
    if (!obj) {
        return;
    }

    obj->reclaim = reclaim_fn;

    hazptr_tc_t* tc = &local_tc;

    // Push onto the thread-local retired list (zero contention!).
    obj->next_retired = tc->retired_list;
    tc->retired_list = obj;
    tc->retired_count++;

    tc->newly_retired++;
    if (tc->newly_retired >= 2048) {
        tc_do_reclamation(tc);
        tc->newly_retired = 0;
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

