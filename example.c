#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

#include "faaq.h"

// Configuration for the multi-threaded example
#define NUM_PRODUCERS      2
#define NUM_CONSUMERS      2
#define TOTAL_THREADS      (NUM_PRODUCERS + NUM_CONSUMERS)
#define ITEMS_PER_PRODUCER 10000

// Shared queue for the multi-threaded example
FAAArrayQueue_t *g_queue          = NULL;
_Atomic uint64_t g_dequeued_count = 0;

// --- Single-Threaded Example ---

void
run_simple_example() {
    printf("--- Running Simple Single-Threaded Example ---\n");

    // Create a queue for a single thread.
    // The `max_threads` parameter is crucial for the underlying hazard
    // pointer system to allocate resources for memory safety.
    int              max_threads = 1;
    FAAArrayQueue_t *q           = faa_queue_create(max_threads);
    assert(q != NULL);
    printf("Queue created successfully.\n");

    // Enqueue some items.
    // For this simple example, we cast integers to void pointers.
    // The thread ID (tid) is 0 since we only have one thread.
    printf("Enqueuing items: 10, 20, 30\n");
    faa_queue_enqueue(q, (void *) 10, 0);
    faa_queue_enqueue(q, (void *) 20, 0);
    faa_queue_enqueue(q, (void *) 30, 0);

    // Dequeue items and verify FIFO order.
    void *item1      = faa_queue_dequeue(q, 0);
    void *item2      = faa_queue_dequeue(q, 0);
    void *item3      = faa_queue_dequeue(q, 0);
    void *empty_item = faa_queue_dequeue(q, 0);

    printf("Dequeued items: %ld, %ld, %ld\n", (intptr_t) item1, (intptr_t) item2, (intptr_t) item3);
    assert((intptr_t) item1 == 10);
    assert((intptr_t) item2 == 20);
    assert((intptr_t) item3 == 30);
    assert(empty_item == NULL); // Dequeue from an empty queue returns NULL
    printf("FIFO order verified. Dequeue on empty queue works.\n");

    // Destroy the queue to free all memory.
    // This should only be done when you are sure no other threads are
    // accessing the queue (e.g., after all threads are joined).
    faa_queue_destroy(q);
    printf("Queue destroyed successfully.\n\n");
}

// --- Multi-Threaded Example ---

int
producer_thread(void *arg) {
    int tid = (int) (intptr_t) arg;
    printf("Producer thread %d started.\n", tid);

    for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
        // In a real application, you would allocate memory for your data.
        // We'll enqueue the item's number, ensuring it's not NULL.
        intptr_t value = (intptr_t) (i + 1);
        faa_queue_enqueue(g_queue, (void *) value, tid);
    }

    printf("Producer thread %d finished.\n", tid);
    return 0;
}

int
consumer_thread(void *arg) {
    int tid = (int) (intptr_t) arg;
    printf("Consumer thread %d started.\n", tid);

    while (atomic_load(&g_dequeued_count) < (NUM_PRODUCERS * ITEMS_PER_PRODUCER)) {
        void *item = faa_queue_dequeue(g_queue, tid);
        if (item != NULL) {
            // In a real application, you would process the item here.
            // If the item was dynamically allocated, you would free it.
            atomic_fetch_add(&g_dequeued_count, 1);
        } else {
            // Yield if the queue is temporarily empty to avoid busy-waiting.
            thrd_yield();
        }
    }
    printf("Consumer thread %d finished.\n", tid);
    return 0;
}

void
run_multithread_example() {
    printf("--- Running Multi-Threaded Example ---\n");
    printf("%d Producers, %d Consumers, %d Items per Producer\n", NUM_PRODUCERS, NUM_CONSUMERS, ITEMS_PER_PRODUCER);

    // Create a queue that can be safely accessed by all our threads.
    g_queue = faa_queue_create(TOTAL_THREADS);
    assert(g_queue != NULL);

    thrd_t threads[TOTAL_THREADS];

    // Create and start producer and consumer threads.
    // Thread IDs must be unique and in the range [0, max_threads - 1].
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        int tid = i;
        thrd_create(&threads[tid], producer_thread, (void *) (intptr_t) tid);
    }
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        int tid = NUM_PRODUCERS + i;
        thrd_create(&threads[tid], consumer_thread, (void *) (intptr_t) tid);
    }

    // Wait for all threads to complete.
    for (int i = 0; i < TOTAL_THREADS; ++i) {
        thrd_join(threads[i], NULL);
    }
    printf("All threads have finished.\n");

    // Verification
    uint64_t total_dequeued = atomic_load(&g_dequeued_count);
    uint64_t total_expected = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    printf("Total items dequeued: %llu\n", (unsigned long long) total_dequeued);
    printf("Total items expected: %llu\n", (unsigned long long) total_expected);
    assert(total_dequeued == total_expected);
    assert(faa_queue_dequeue(g_queue, 0) == NULL); // Queue should be empty now
    printf("Verification successful.\n");

    // Destroy the queue.
    faa_queue_destroy(g_queue);
    printf("Queue destroyed successfully.\n");
}

int
main() {
    run_simple_example();
    run_multithread_example();
    return 0;
}
