#include "hp.h"

#include <stdio.h>
#include <string.h>
#include <threads.h>

#include "khashl.h"

KHASHL_SET_INIT(KH_LOCAL, ptr_set_t, ptr_set, uintptr_t, kh_hash_uint64, kh_eq_generic)

static_assert((HP_NUM_SHARDS & (HP_NUM_SHARDS - 1)) == 0, "HP_NUM_SHARDS must be a power of 2");

typedef struct {
    alignas(HP_CACHE_LINE_SIZE) _Atomic(hazptr_obj_t *) retired_head;
} hazptr_shard_t;

struct hazptr_domain {
    // --- HP Record Management (Mostly Cold) ---
    _Atomic(hazptr_rec_t *) hprec_list;
    _Atomic(hazptr_rec_t *) hprec_avail;
    _Atomic(size_t)         hprec_count;
    ptr_set_t              *scan_set;

    // --- Reclamation Control (Potentially Hot) ---

    alignas(HP_CACHE_LINE_SIZE) _Atomic(hazptr_count_t) retired_count;

    alignas(HP_CACHE_LINE_SIZE) _Atomic(bool) reclaiming;

    // --- Sharded Retired Lists (Hot) ---
    hazptr_shard_t shards[HP_NUM_SHARDS];
};

// Global default domain. Statically initialized to zero by C standard.
static hazptr_domain_t default_domain = {};

typedef struct {
    hazptr_rec_t *records[HP_TLC_CAPACITY];
    size_t        count;
} hazptr_tc_t;

static thread_local hazptr_tc_t tls_cache = {};

static tss_t                    hazptr_tss_key;
static once_flag                tss_init_flag = ONCE_FLAG_INIT;

static hazptr_rec_t            *domain_acquire_hprec(hazptr_domain_t *domain);
static void domain_release_hprec_list(hazptr_domain_t *domain, hazptr_rec_t *head, hazptr_rec_t *tail);

// Flushes the thread local cache back to the domain.
static void
tlc_flush(hazptr_tc_t *tc) {
    if (tc->count == 0) {
        return;
    }

    hazptr_domain_t *domain = &default_domain;

    // Build a linked list from the cached array elements locally.
    hazptr_rec_t    *head   = tc->records[0];
    hazptr_rec_t    *tail   = head;
    head->next_avail        = nullptr; // Initialize the first element's next pointer

    for (size_t i = 1; i < tc->count; ++i) {
        hazptr_rec_t *rec = tc->records[i];
        rec->next_avail   = nullptr;
        tail->next_avail  = rec;
        tail              = rec;
    }

    // Release the entire list to the domain in one atomic operation.
    domain_release_hprec_list(domain, head, tail);
    tc->count = 0;
}

// TSS Destructor: Called automatically when the thread exits.
static void
hazptr_tss_destructor(void *data) {
    if (data) {
        // Data points to the thread's tls_cache instance.
        tlc_flush((hazptr_tc_t *) data);
    }
}

// Global initialization of the TSS key.
static void
initialize_tss(void) {
    if (tss_create(&hazptr_tss_key, hazptr_tss_destructor) != thrd_success) {
        fprintf(stderr, "C23 Hazptr Fatal Error: Failed to create TSS key.\n");
        abort();
    }
}

// Ensures the current thread is registered for cleanup.
static inline void
ensure_thread_registered(void) {
    // Initialize the global key if necessary (thread-safe).
    call_once(&tss_init_flag, initialize_tss);

    // Check if this thread has already registered its TLC address.
    static thread_local bool registered = false;
    if (!registered) {
        // Register the address of the tls_cache so the destructor can find it.
        if (tss_set(hazptr_tss_key, &tls_cache) != thrd_success) {
            fprintf(stderr, "C23 Hazptr Error: Failed to set TSS value.\n");
            // Continue, but cleanup won't happen automatically for this thread.
        }
        registered = true;
    }
}

// Try to acquire a record from the TLC.
static inline hazptr_rec_t *
tlc_try_acquire(void) {
    if (tls_cache.count > 0) {
        return tls_cache.records[--tls_cache.count];
    }
    return nullptr;
}

// Try to release a record to the TLC.
static inline bool
tlc_try_release(hazptr_rec_t *rec) {
    if (tls_cache.count < HP_TLC_CAPACITY) {
        tls_cache.records[tls_cache.count++] = rec;
        return true;
    }
    return false;
}

static hazptr_rec_t *
domain_acquire_hprec(hazptr_domain_t *domain) {
    // Try popping from the domain's lock-free available stack.
    hazptr_rec_t *rec = atomic_load_explicit(&domain->hprec_avail, memory_order_acquire);
    while (rec) {
        hazptr_rec_t *next = rec->next_avail;
        if (atomic_compare_exchange_weak_explicit(
                &domain->hprec_avail, &rec, next, memory_order_release, memory_order_acquire
            )) {
            rec->next_avail = nullptr;
            return rec;
        }
        // CAS failed, rec is updated, retry.
    }

    // Stack empty, allocate a new record using C23 aligned_alloc.
    rec = aligned_alloc(HP_CACHE_LINE_SIZE, sizeof(hazptr_rec_t));
    if (!rec) {
        fprintf(stderr, "C23 Hazptr Fatal Error: Failed to allocate hazptr_rec_t.\n");
        abort();
    }

    // Initialize the record.
    atomic_init(&rec->ptr, nullptr);
    rec->domain        = domain;
    rec->next_avail    = nullptr;

    // Add to the global hprec_list (for scanning).
    hazptr_rec_t *head = atomic_load_explicit(&domain->hprec_list, memory_order_relaxed);
    do {
        rec->next = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &domain->hprec_list, &head, rec, memory_order_release, memory_order_relaxed
    ));

    atomic_fetch_add_explicit(&domain->hprec_count, 1, memory_order_acq_rel);
    return rec;
}

// Releases a list of records back to the domain.
static void
domain_release_hprec_list(hazptr_domain_t *domain, hazptr_rec_t *head, hazptr_rec_t *tail) {
    assert(tail != nullptr && tail->next_avail == nullptr);

    // Push the entire list onto the lock-free available stack.
    hazptr_rec_t *old_head = atomic_load_explicit(&domain->hprec_avail, memory_order_relaxed);
    do {
        tail->next_avail = old_head;
    } while (!atomic_compare_exchange_weak_explicit(
        &domain->hprec_avail, &old_head, head, memory_order_release, memory_order_relaxed
    ));
}

static void domain_do_reclamation(hazptr_domain_t *domain, hazptr_count_t claimed_count);

// Helper to calculate the shard index.
static inline size_t
calc_shard(void const *ptr) {
    // Simple hash based on address bits, ignoring low alignment bits (e.g., 4
    // bits = 16 bytes).
    return ((uintptr_t) ptr >> 4) & (HP_NUM_SHARDS - 1);
}

// Helper to calculate the dynamic reclamation threshold.
static hazptr_count_t
calculate_threshold(hazptr_domain_t *domain) {
    hazptr_count_t thresh         = HP_RCOUNT_THRESHOLD;
    // Acquire load synchronizes with hprec creation.
    size_t         hcount         = atomic_load_explicit(&domain->hprec_count, memory_order_acquire);
    hazptr_count_t dynamic_thresh = (hazptr_count_t) (hcount * HP_HCOUNT_MULTIPLIER);

    // Follow Folly: max(RCOUNT_THRESHOLD, HCOUNT * MULTIPLIER)
    return (dynamic_thresh > thresh) ? dynamic_thresh : thresh;
}

// Attempts to claim a batch for reclamation by atomically resetting the count
// (CAS Handoff).
static hazptr_count_t
domain_check_threshold(hazptr_domain_t *domain) {
    hazptr_count_t rcount = atomic_load_explicit(&domain->retired_count, memory_order_acquire);
    hazptr_count_t thresh = calculate_threshold(domain);

    while (rcount >= thresh) {
        // Try to atomically reset the count to 0.
        if (atomic_compare_exchange_weak_explicit(
                &domain->retired_count, &rcount, 0, memory_order_acq_rel, memory_order_relaxed
            )) {
            // Success: we claimed 'rcount' items.
            return rcount;
        }
        // CAS failed, rcount updated, re-evaluate threshold and retry.
        thresh = calculate_threshold(domain);
    }
    return 0;
}

// The core reclamation routine.
static void
domain_do_reclamation(hazptr_domain_t *domain, hazptr_count_t claimed_count) {
    // Optimization: Serialize reclamation attempts.
    if (atomic_exchange_explicit(&domain->reclaiming, true, memory_order_acquire)) {
        // Another thread is reclaiming. Crucially, we must return the claimed count
        // back to the pool so the active thread can process it later.
        if (claimed_count != 0) {
            atomic_fetch_add_explicit(&domain->retired_count, claimed_count, memory_order_acq_rel);
        }
        return;
    }

    if (domain->scan_set == nullptr) {
        domain->scan_set = ptr_set_init();
        if (!domain->scan_set) { /* Handle OOM */
            abort();
        }
    }
    ptr_set_t     *protected_set = domain->scan_set;

    // We hold the reclamation lock.
    hazptr_count_t rcount        = claimed_count;

    // Loop until the count is low AND all shards are confirmed empty.
    while (true) {
        hazptr_obj_t *retired_lists[HP_NUM_SHARDS];
        bool          extracted_any = false;

        // Extract retired objects from all shards atomically.
        for (int i = 0; i < HP_NUM_SHARDS; ++i) {
            hazptr_shard_t *shard = &domain->shards[i];
            // Acquire ensures visibility of items pushed with release.
            retired_lists[i]      = atomic_exchange_explicit(&shard->retired_head, nullptr, memory_order_acquire);
            if (retired_lists[i]) {
                extracted_any = true;
            }
        }

        if (extracted_any) {
            // Synchronization Fence (Heavy Fence - SeqCst).
            // Ensures we observe all hazard pointers set by others before this point.
            // Synchronizes with the light fences in HAZPTR_PROTECT and hazptr_retire.
            atomic_thread_fence(memory_order_seq_cst);

            // Load hazard pointer values into the khashl set.
            // Clear the set from the previous iteration.
            ptr_set_clear(protected_set);
            hazptr_rec_t *rec = atomic_load_explicit(&domain->hprec_list, memory_order_acquire);
            while (rec) {
                // Load HP value with acquire.
                void const *ptr = atomic_load_explicit(&rec->ptr, memory_order_acquire);
                if (ptr) {
                    int absent;
                    ptr_set_put(protected_set, (uintptr_t) ptr, &absent);
                }
                rec = rec->next;
            }

            // Match and reclaim.
            hazptr_obj_t *remaining_head = nullptr;
            hazptr_obj_t *remaining_tail = nullptr;

            for (int i = 0; i < HP_NUM_SHARDS; ++i) {
                hazptr_obj_t *current = retired_lists[i];
                while (current) {
                    hazptr_obj_t *next = current->next_retired;
                    // Check if the pointer exists in the set (kh_end means not found).
                    if (ptr_set_get(protected_set, (uintptr_t) current) < kh_end(protected_set)) {
                        // Protected: Keep it.
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
                        // Adjust the count of items we are responsible for.
                        // Can go negative if we reclaim more than initially claimed.
                        rcount--;
                    }
                    current = next;
                }
            }

            // Optimization: Restore remaining objects to a single shard (Shard 0).
            if (remaining_head) {
                hazptr_shard_t *shard0 = &domain->shards[0];
                hazptr_obj_t   *head   = atomic_load_explicit(&shard0->retired_head, memory_order_relaxed);
                do {
                    remaining_tail->next_retired = head;
                } while (!atomic_compare_exchange_weak_explicit(
                    &shard0->retired_head, &head, remaining_head, memory_order_release, memory_order_relaxed
                ));
                // Note: We do not update the global retired_count here; it's handled by
                // 'rcount'.
            }
        }

        // Account for the remaining rcount (items claimed but not reclaimed, or
        // excess reclaimed).
        if (rcount != 0) {
            atomic_fetch_add_explicit(&domain->retired_count, rcount, memory_order_acq_rel);
        }

        // Check if we need to loop again.
        rcount = domain_check_threshold(domain);
        if (rcount == 0) {
            // ensure all shards are truly empty before exiting the lock.
            bool done = true;
            for (int i = 0; i < HP_NUM_SHARDS; ++i) {
                if (atomic_load_explicit(&domain->shards[i].retired_head, memory_order_acquire) != nullptr) {
                    done = false;
                    break;
                }
            }
            if (done) {
                break; // Exit the reclamation loop.
            }
            // If not done but rcount is 0, loop again to process remaining items.
        }
    }

    // Release the reclamation lock.
    atomic_store_explicit(&domain->reclaiming, false, memory_order_release);
}

// --- Holder API ---

void
hazptr_holder_init(hazptr_holder_t *h) {
    // We optimize TLC for the default domain.
    hazptr_domain_t *domain = &default_domain;

    // Ensure thread cleanup is registered.
    ensure_thread_registered();

    // Try Thread Local Cache (Fast path).
    hazptr_rec_t *rec = tlc_try_acquire();

    if (rec == nullptr) {
        // Cache miss (Slow path).
        rec = domain_acquire_hprec(domain);
    }

    h->hprec = rec;
    hazptr_reset(h, nullptr);
}

void
hazptr_holder_destroy(hazptr_holder_t *h) {
    hazptr_rec_t *rec = h->hprec;
    if (!rec) {
        return;
    }

    // Must reset before releasing.
    hazptr_reset(h, nullptr);

    // Optimization: Try Thread Local Cache (Fast path).
    // (Assumes default domain, as TLC is only used for it).
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

void
hazptr_retire(hazptr_obj_t *obj, hazptr_reclaim_fn reclaim_fn) {
    if (!obj) {
        return;
    }

    obj->reclaim            = reclaim_fn;
    hazptr_domain_t *domain = &default_domain;

    // Ensures that the removal of the object from the data structure
    // (which happened before this call) is visible before the object is retired.
    atomic_thread_fence(memory_order_seq_cst);

    // Push onto the appropriate shard.
    size_t          shard_idx = calc_shard(obj);
    hazptr_shard_t *shard     = &domain->shards[shard_idx];

    hazptr_obj_t   *head      = atomic_load_explicit(&shard->retired_head, memory_order_relaxed);
    do {
        obj->next_retired = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &shard->retired_head, &head, obj, memory_order_release, memory_order_relaxed
    ));

    // Update centralized count.
    atomic_fetch_add_explicit(&domain->retired_count, 1, memory_order_acq_rel);

    // Check threshold and potentially trigger reclamation.
    hazptr_count_t rcount = domain_check_threshold(domain);
    if (rcount > 0) {
        domain_do_reclamation(domain, rcount);
    }
}

void
hazptr_cleanup(void) {
    hazptr_domain_t *domain = &default_domain;

    // Force a reclamation cycle by claiming whatever count remains.
    hazptr_count_t   rcount = atomic_exchange_explicit(&domain->retired_count, 0, memory_order_acq_rel);

    if (rcount < 0) {
        // Handle transient negative count if another reclamation just finished.
        atomic_fetch_add_explicit(&domain->retired_count, rcount, memory_order_acq_rel);
        rcount = 0;
    }

    domain_do_reclamation(domain, rcount);
}
