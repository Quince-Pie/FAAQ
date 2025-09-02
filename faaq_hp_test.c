#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For memset_explicit
#include <threads.h>

#include "faaq.h"

static constexpr int           MPMC_PRODUCERS     = 8;
static constexpr int           MPMC_CONSUMERS     = 8;
static constexpr uint64_t      ITEMS_PER_PRODUCER = 1000000;

static constexpr int           MPMC_TOTAL_THREADS = MPMC_PRODUCERS + MPMC_CONSUMERS;
static constexpr uint64_t      MPMC_TOTAL_ITEMS   = (uint64_t) MPMC_PRODUCERS * ITEMS_PER_PRODUCER;

static constexpr uint64_t      PAYLOAD_MAGIC      = 0xC23FAAC0FFEE;
static constexpr unsigned char POISON_BYTE        = 0xEE;

static FAAArrayQueue_t        *g_mpmc_queue       = nullptr;

static _Atomic(bool)           g_verification[MPMC_TOTAL_ITEMS];

alignas(64) static _Atomic(uint64_t) g_dequeued_count = 0;

typedef struct {
    uint64_t item_id;      // Unique global ID (0 to TOTAL_ITEMS - 1)
    uint64_t producer_tid; // The TID of the producer
    uint64_t magic;        // Integrity check
    char     padding[32];  // Add some size to stress the memory system
} Payload_t;

// Creates a new payload on the heap.
[[nodiscard("payload must be used or at least freed")]]
static Payload_t *
create_payload(uint64_t item_id, uint64_t producer_tid) {
    Payload_t *p = malloc(sizeof(Payload_t));
    if (!p) {
        perror("Failed to allocate Payload");
        abort();
    }
    *p = (Payload_t) { .item_id = item_id, .producer_tid = producer_tid, .magic = PAYLOAD_MAGIC };
    return p;
}

void
run_basic_tests(void) {
    printf("--- Starting Basic Single-Threaded Tests ---\n");
    FAAArrayQueue_t *q = faa_queue_create(1);
    assert(q != nullptr);

    void *item = faa_queue_dequeue(q, 0);
    assert(item == nullptr);
    printf("Test 1 (Empty Dequeue): PASSED\n");

    // Test 2: Enqueue and Dequeue FIFO order
    // We use pointer tagging (casting integers to pointers) for simplicity here.
    uintptr_t val1 = 0xAAA;
    uintptr_t val2 = 0xBBB;
    faa_queue_enqueue(q, (void *) val1, 0);
    faa_queue_enqueue(q, (void *) val2, 0);

    item = faa_queue_dequeue(q, 0);
    assert(item == (void *) val1);

    item = faa_queue_dequeue(q, 0);
    assert(item == (void *) val2);

    item = faa_queue_dequeue(q, 0);
    assert(item == nullptr);
    printf("Test 2 (FIFO Order): PASSED\n");

    // Test 3: Buffer boundary conditions (stress node allocation and reclamation)
    // FAA_BUFFER_SIZE is defined in faa_array_queue.h
    printf("Test 3 (Buffer Boundary/Allocation): ");

    // Enqueue enough items to fill multiple buffers.
    uint64_t const boundary_count = FAA_BUFFER_SIZE * 2 + 50;

    for (uint64_t i = 1; i <= boundary_count; i++) {
        faa_queue_enqueue(q, (void *) (uintptr_t) i, 0);
    }

    // Dequeue and verify order. This forces node reclamation via Hazard Pointers.
    for (uint64_t i = 1; i <= boundary_count; i++) {
        item = faa_queue_dequeue(q, 0);
        if (item != (void *) (uintptr_t) i) {
            fprintf(stderr, "FAILED! Expected %w64u, got %ju\n", i, (uintptr_t) item);
            assert(false);
        }
    }
    item = faa_queue_dequeue(q, 0);
    assert(item == nullptr);
    printf("PASSED\n");

    faa_queue_destroy(q);
    printf("Basic tests finished successfully.\n");
}

int
producer_thread(void *arg) {
    int tid = (int) (uintptr_t) arg;
    assert(tid >= 0 && tid < MPMC_PRODUCERS);

    // Calculate the unique range of global item IDs this producer is responsible
    // for.
    uint64_t start_id = (uint64_t) tid * ITEMS_PER_PRODUCER;
    uint64_t end_id   = start_id + ITEMS_PER_PRODUCER;

    printf("Producer %d starting (Items %w64u to %w64u)\n", tid, start_id, end_id - 1);

    for (uint64_t i = start_id; i < end_id; ++i) {
        Payload_t *payload = create_payload(i, (uint64_t) tid);

        faa_queue_enqueue(g_mpmc_queue, payload, tid);

        if (i % 50000 == 0) {
            thrd_yield();
        }
    }

    printf("Producer %d finished.\n", tid);
    return 0;
}

int
consumer_thread(void *arg) {
    int tid = (int) (uintptr_t) arg;
    assert(tid >= MPMC_PRODUCERS && tid < MPMC_TOTAL_THREADS);

    printf("Consumer %d starting.\n", tid);
    uint64_t local_dequeue_count = 0;

    while (atomic_load_explicit(&g_dequeued_count, memory_order_acquire) < MPMC_TOTAL_ITEMS) {
        void *item = faa_queue_dequeue(g_mpmc_queue, tid);

        if (item != nullptr) {
            Payload_t *payload = (Payload_t *) item;

            // 1. Validation: Check payload integrity (ensures HP protected the
            // memory).
            if (payload->magic != PAYLOAD_MAGIC) {
                // Use C23 %w64x format specifier for hex output.
                fprintf(
                    stderr,
                    "FATAL ERROR (UAF/Corruption): Consumer %d dequeued corrupted "
                    "payload. Magic: "
                    "%w64x\n",
                    tid,
                    payload->magic
                );
                abort();
            }

            uint64_t id = payload->item_id;

            // 2. Validation: Check ID range.
            if (id >= MPMC_TOTAL_ITEMS) {
                fprintf(stderr, "FATAL ERROR: Consumer %d dequeued invalid ID %w64u\n", tid, id);
                abort();
            }

            // 3. Validation: Check for duplicate dequeues (Exactly-Once Semantics).
            bool already_dequeued = atomic_exchange_explicit(&g_verification[id], true, memory_order_acq_rel);

            if (already_dequeued) {
                fprintf(
                    stderr,
                    "FATAL ERROR (Duplicate): Consumer %d detected duplicate "
                    "dequeue for ID %w64u "
                    "(Producer %w64u)\n",
                    tid,
                    id,
                    payload->producer_tid
                );
                abort();
            }

            // 4. Cleanup the payload.
            // Poison the memory before freeing to maximize detection of UAF if HP
            // failed.
            memset(payload, POISON_BYTE, sizeof(Payload_t));
            free(payload);

            // Increment the global counter atomically.
            atomic_fetch_add_explicit(&g_dequeued_count, 1, memory_order_acq_rel);
            local_dequeue_count++;
        } else {
            // Queue was momentarily empty. Yield to allow producers to catch up.
            thrd_yield();
        }
    }

    printf("Consumer %d finished (Dequeued: %w64u).\n", tid, local_dequeue_count);
    return 0;
}

void
run_mpmc_test(void) {
    printf(
        "\n--- Starting MPMC Concurrent Stress Test (Exactly-Once "
        "Verification) ---\n"
    );
    printf("Producers: %d, Consumers: %d\n", MPMC_PRODUCERS, MPMC_CONSUMERS);
    printf("Items per producer: %w64u, Total items: %w64u\n", ITEMS_PER_PRODUCER, MPMC_TOTAL_ITEMS);

    // 1. Initialize the queue.
    g_mpmc_queue = faa_queue_create(MPMC_TOTAL_THREADS);
    if (!g_mpmc_queue) {
        fprintf(stderr, "Failed to create FAA Array Queue for MPMC test.\n");
        exit(EXIT_FAILURE);
    }

    atomic_init(&g_dequeued_count, 0);

    thrd_t producers[MPMC_PRODUCERS];
    thrd_t consumers[MPMC_CONSUMERS];

    printf("Spawning threads...\n");

    for (int i = 0; i < MPMC_CONSUMERS; ++i) {
        int tid = MPMC_PRODUCERS + i;
        if (thrd_create(&consumers[i], consumer_thread, (void *) (uintptr_t) tid) != thrd_success) {
            perror("Failed to create consumer thread");
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < MPMC_PRODUCERS; ++i) {
        int tid = i;
        if (thrd_create(&producers[i], producer_thread, (void *) (uintptr_t) tid) != thrd_success) {
            perror("Failed to create producer thread");
            exit(EXIT_FAILURE);
        }
    }

    printf("Waiting for threads to complete...\n");

    for (int i = 0; i < MPMC_PRODUCERS; ++i) {
        thrd_join(producers[i], nullptr);
    }
    for (int i = 0; i < MPMC_CONSUMERS; ++i) {
        thrd_join(consumers[i], nullptr);
    }

    printf("All threads joined.\n");

    printf("\n--- MPMC Verification ---\n");
    uint64_t final_count = atomic_load(&g_dequeued_count);
    printf("Total Dequeued Count: %w64u\n", final_count);

    bool success = true;
    if (final_count != MPMC_TOTAL_ITEMS) {
        fprintf(stderr, "ERROR: Count mismatch! Expected %w64u, Got %w64u.\n", MPMC_TOTAL_ITEMS, final_count);
        success = false;
    }

    // Check the verification array for any missed items (Verification of No
    // Loss).
    uint64_t missing_count = 0;
    for (uint64_t i = 0; i < MPMC_TOTAL_ITEMS; ++i) {
        if (!atomic_load_explicit(&g_verification[i], memory_order_relaxed)) {
            if (missing_count < 10) { // Limit output spam
                fprintf(stderr, "ERROR (Missed): Item ID %w64u was never dequeued!\n", i);
            }
            missing_count++;
            success = false;
        }
    }

    if (missing_count > 0) {
        printf("Total Missing Items: %w64u\n", missing_count);
    }

    printf("Destroying queue...\n");
    if (faa_queue_dequeue(g_mpmc_queue, 0) != nullptr) {
        printf("ERROR: Queue not empty after test completion!\n");
        success = false;
    }
    faa_queue_destroy(g_mpmc_queue);
    g_mpmc_queue = nullptr;

    if (success) {
        printf(
            "MPMC SUCCESS: Queue operated correctly. Exactly-once semantics "
            "verified.\n"
        );
    } else {
        printf("MPMC FAILURE: Queue operation failed verification.\n");
        exit(EXIT_FAILURE);
    }
}

int
main(void) {
    printf("C23 FAA Array Queue Example and Test Suite\n\n");

    run_basic_tests();
    run_mpmc_test();

    printf("\nAll tests completed successfully.\n");
    return EXIT_SUCCESS;
}
