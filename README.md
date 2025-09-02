# Fast FAA Array Queue

A fast, lock-free, multi-producer, multi-consumer (MPMC) queue implementation in C. It uses a Fetch-And-Add (FAA) strategy on array indices for high throughput and relies on a hazard pointer implementation for safe memory reclamation. This is a slightly modified version of [FAAArrayQueue](https://concurrencyfreaks.blogspot.com/2016/11/faaarrayqueue-mpmc-lock-free-queue-part.html) ported to C23.

## USAGE

The API is simple and requires the user to manage thread IDs for concurrent operations.

### 1\. Creation

First, create a queue instance. You must specify the maximum number of threads that will ever access the queue concurrently. This is essential for the underlying memory safety mechanism (hazard pointers).

```c
FAAArrayQueue_t *faa_queue_create(int max_threads);
```

  * `max_threads`: The maximum number of concurrent threads that will use the queue. Must be greater than 0.
  * **Returns**: A pointer to the initialized queue, or `NULL` on failure.

**Example:**

```c
#define NUM_THREADS 8
FAAArrayQueue_t* my_queue = faa_queue_create(NUM_THREADS);
if (!my_queue) {
    // Handle creation failure
}
```

### 2\. Enqueue

Add an item to the tail of the queue. This operation is lock-free and safe to call from multiple threads concurrently.

```c
void faa_queue_enqueue(FAAArrayQueue_t *q, void *item, int tid);
```

  * `q`: A pointer to the queue.
  * `item`: The pointer to the item to be enqueued. It must not be `NULL`.
  * `tid`: The unique thread ID of the calling thread. This ID must be in the range `[0, max_threads - 1]`. Each concurrent thread must have its own unique `tid`.

**Example:**

```c
// In producer thread with tid = 0
int* my_data = malloc(sizeof(int));
*my_data = 123;
faa_queue_enqueue(my_queue, my_data, 0);
```

### 3\. Dequeue

Remove an item from the head of the queue. This operation is lock-free and safe to call from multiple threads concurrently.

```c
void *faa_queue_dequeue(FAAArrayQueue_t *q, int tid);
```

  * `q`: A pointer to the queue.
  * `tid`: The unique thread ID of the calling thread. This ID must be in the range `[0, max_threads - 1]`.
  * **Returns**: A pointer to the dequeued item, or `NULL` if the queue was empty.

**Example:**

```c
// In consumer thread with tid = 1
int* received_data = (int*)faa_queue_dequeue(my_queue, 1);
if (received_data != NULL) {
    printf("Dequeued: %d\n", *received_data);
    free(received_data);
}
```

### 4\. Destruction

Free all memory associated with the queue. This function should **only** be called when no other threads are accessing the queue (i.e., after all producer/consumer threads have been joined). It will drain any remaining items in the queue before freeing memory.

```c
void faa_queue_destroy(FAAArrayQueue_t *q);
```

  * `q`: A pointer to the queue.

**Example:**

```c
// After all threads are joined
faa_queue_destroy(my_queue);
```

### Complete Example

Here is a simple, single-threaded example demonstrating the complete lifecycle.

```c
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include "faa_array_queue.h"

int main() {
    // 1. Create for 1 thread
    FAAArrayQueue_t* q = faa_queue_create(1);
    assert(q != NULL);

    // 2. Enqueue items (using integers as data for simplicity)
    faa_queue_enqueue(q, (void*)10, 0);
    faa_queue_enqueue(q, (void*)20, 0);

    // 3. Dequeue items
    void* item1 = faa_queue_dequeue(q, 0);
    void* item2 = faa_queue_dequeue(q, 0);

    printf("Dequeued: %ld\n", (intptr_t)item1); // Prints 10
    printf("Dequeued: %ld\n", (intptr_t)item2); // Prints 20

    assert((intptr_t)item1 == 10);
    assert((intptr_t)item2 == 20);

    // 4. Destroy the queue
    faa_queue_destroy(q);

    return 0;
}
```


## References

This work is directly inspired by:

- The CPP implementation of [FAAArrayQueue](https://concurrencyfreaks.blogspot.com/2016/11/faaarrayqueue-mpmc-lock-free-queue-part.html)

- Folly's Hazard Pointer Implementation [folly](https://github.com/facebook/folly)

Additionally, the implementation of hazard pointers make use of [khashl](https://github.com/attractivechaos/khashl/blob/main/khashl.h) for faster reclaimation process.
