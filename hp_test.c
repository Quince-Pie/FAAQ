#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For memset
#include <threads.h>

#include "hp.h"

#define NUM_READERS      8
#define NUM_WRITERS      8
#define TEST_DURATION_MS 5000 // Run the stress test for 2 seconds

#define MAGIC_NUMBER     0xC23FEEDFACEBABEULL
#define POISON_BYTE      0xCC

typedef struct Node {
    // Hazard Pointer base object. Must be the first member for direct casting
    // safety if the implementation relies on protecting the container pointer.
    hazptr_obj_t base;
    uint64_t     id;
    uint64_t     magic; // Safety check
    char         padding[64];
} Node_t;

// The shared resource protected by hazard pointers
_Atomic(Node_t *) g_shared_ptr        = nullptr;

// Control flag for the test duration
_Atomic(bool)     g_running           = true;

// Verification counters
_Atomic(uint64_t) g_objects_created   = 0;
_Atomic(uint64_t) g_objects_reclaimed = 0;
_Atomic(uint64_t) g_read_operations   = 0;

void
node_reclaim(hazptr_obj_t *obj) {
    Node_t *node = (Node_t *) obj;

    if (node->magic != MAGIC_NUMBER) {
        fprintf(stderr, "FATAL: Magic mismatch during reclamation! ID: %w64u, Magic: %w64x\n", node->id, node->magic);
        abort();
    }

    // Poison the memory before freeing to maximize detection of use-after-free
    // (UAF).
    memset(node, POISON_BYTE, sizeof(Node_t));
    free(node);
    atomic_fetch_add_explicit(&g_objects_reclaimed, 1, memory_order_acq_rel);
}

Node_t *
node_create(uint64_t id) {
    Node_t *node = malloc(sizeof(Node_t));
    if (!node) {
        perror("Failed to allocate Node_t");
        abort();
    }
    *node = (Node_t) { .base = (hazptr_obj_t) {}, .id = id, .magic = MAGIC_NUMBER };

    atomic_fetch_add_explicit(&g_objects_created, 1, memory_order_acq_rel);
    return node;
}

int
reader_thread(void *arg) {
    uint64_t        thread_id = (uint64_t) (uintptr_t) arg;
    uint64_t        local_ops = 0;
    hazptr_holder_t h;

    hazptr_holder_init(&h);

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        Node_t *local_ptr;

        HAZPTR_PROTECT(local_ptr, &h, &g_shared_ptr);

        if (local_ptr) {
            if (local_ptr->magic != MAGIC_NUMBER) {
                // If this fails, the HP implementation failed to protect the memory.
                fprintf(
                    stderr,
                    "SAFETY VIOLATION (UAF): Reader %w64u accessed poisoned "
                    "memory! ID: %w64u, Magic: %w64x\n",
                    thread_id,
                    local_ptr->id,
                    local_ptr->magic
                );
                abort();
            }
            // Simulate work by reading the ID
            [[maybe_unused]]
            uint64_t id
                = local_ptr->id;

            if (local_ops % 1000 == 0) {
                thrd_yield();
            }
        }

        hazptr_reset(&h, nullptr);
        local_ops++;
    }

    hazptr_holder_destroy(&h);
    atomic_fetch_add_explicit(&g_read_operations, local_ops, memory_order_acq_rel);
    return 0;
}

int
writer_thread(void *arg) {
    uint64_t thread_id  = (uint64_t) (uintptr_t) arg;
    // Assign a unique ID space for this thread's allocations to prevent
    // collisions
    uint64_t id_counter = thread_id << 48;
    uint64_t operations = 0;

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        Node_t *new_node = node_create(id_counter++);

        Node_t *old_node = atomic_exchange_explicit(&g_shared_ptr, new_node, memory_order_acq_rel);

        if (old_node) {
            hazptr_retire(&old_node->base, node_reclaim);
        }
        operations++;

        // Yield occasionally to ensure readers and reclamation make progress
        if (operations % 500 == 0) {
            thrd_yield();
        }
    }
    return 0;
}

static bool
verify_results(void) {
    uint64_t created   = atomic_load(&g_objects_created);
    uint64_t reclaimed = atomic_load(&g_objects_reclaimed);
    uint64_t read_ops  = atomic_load(&g_read_operations);

    printf("\n--- Verification Results ---\n");
    printf("Total Read Operations: %w64u\n", read_ops);
    printf("Objects Created:       %w64u\n", created);
    printf("Objects Reclaimed:     %w64u\n", reclaimed);
    printf("----------------------------\n");

    return created == reclaimed;
}

int
main(void) {
    printf("C23 Hazard Pointer Concurrent MWMR Stress Test\n");
    printf("Readers: %d, Writers: %d, Duration: %dms\n", NUM_READERS, NUM_WRITERS, TEST_DURATION_MS);

    atomic_store(&g_shared_ptr, node_create(0));

    thrd_t readers[NUM_READERS];
    thrd_t writers[NUM_WRITERS];

    for (int i = 0; i < NUM_READERS; ++i) {
        // Pass thread ID via pointer casting
        if (thrd_create(&readers[i], reader_thread, (void *) (uintptr_t) i) != thrd_success) {
            perror("Failed to create reader thread");
            return EXIT_FAILURE;
        }
    }

    for (int i = 0; i < NUM_WRITERS; ++i) {
        // Start writer IDs after reader IDs
        if (thrd_create(&writers[i], writer_thread, (void *) (uintptr_t) (i + NUM_READERS)) != thrd_success) {
            perror("Failed to create writer thread");
            return EXIT_FAILURE;
        }
    }

    printf("Running...\n");
    struct timespec duration = { .tv_sec = TEST_DURATION_MS / 1000, .tv_nsec = (TEST_DURATION_MS % 1000) * 1000000L };
    thrd_sleep(&duration, nullptr);

    printf("Stopping threads...\n");
    atomic_store_explicit(&g_running, false, memory_order_release);

    for (int i = 0; i < NUM_WRITERS; ++i) {
        thrd_join(writers[i], nullptr);
    }
    for (int i = 0; i < NUM_READERS; ++i) {
        thrd_join(readers[i], nullptr);
    }

    printf("Threads joined. Performing final cleanup...\n");

    Node_t *last_ptr = atomic_exchange_explicit(&g_shared_ptr, nullptr, memory_order_acq_rel);
    if (last_ptr) {
        hazptr_retire(&last_ptr->base, node_reclaim);
    }

    // Force reclamation of all outstanding objects.
    hazptr_cleanup();

    // Verification
    if (verify_results()) {
        printf(
            "SUCCESS: All objects successfully reclaimed. No memory leaks "
            "detected.\n"
        );
        return EXIT_SUCCESS;
    }

    // If verification failed, it might be due to delayed thread termination (TSS
    // destructors haven't run yet to flush Thread Local Caches). Wait briefly and
    // try cleanup again.
    printf(
        "\nWARNING: Leak detected. Waiting for potential delayed TSS cleanup "
        "and retrying...\n"
    );
    thrd_sleep(&(struct timespec) { .tv_sec = 1 }, nullptr);
    hazptr_cleanup();

    if (verify_results()) {
        printf("SUCCESS (After Retry): All objects reclaimed.\n");
        return EXIT_SUCCESS;
    } else {
        printf("FAILURE: Memory leak persisted after retry!\n");
        return EXIT_FAILURE;
    }
}
