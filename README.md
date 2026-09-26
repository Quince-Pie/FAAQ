# Fast FAA Array Queue

A fast, lock-free, multi-producer, multi-consumer (MPMC) queue implementation in C23. It uses a Fetch-And-Add (FAA) strategy on array indices for high throughput and relies on a hazard pointer implementation for safe memory reclamation. This is a modified version of [FAAArrayQueue](https://concurrencyfreaks.blogspot.com/2016/11/faaarrayqueue-mpmc-lock-free-queue-part.html) ported to C23.

## USAGE

The API has no thread ids and no thread registration: any number of threads may use any queue at any time. Each thread lazily attaches per-queue state (two hazard pointers) on first use, caches it for its most recently used queues, and releases it automatically when the thread exits or when the queue is destroyed.

### 1\. Creation

```c
FAAArrayQueue_t *faa_queue_create(void);
```

  * **Returns**: A pointer to the initialized queue, or `nullptr` on allocation failure.

```c
FAAArrayQueue_t* my_queue = faa_queue_create();
if (!my_queue) {
    // Handle creation failure
}
```

### 2\. Enqueue

Add an item to the tail of the queue. Lock-free; safe to call from any number of threads concurrently.

```c
void faa_queue_enqueue(FAAArrayQueue_t *q, void *item);
```

  * `item`: The pointer to enqueue. `nullptr` is ignored (`nullptr` is what dequeue returns for "empty").

```c
int* my_data = malloc(sizeof(int));
*my_data = 123;
faa_queue_enqueue(my_queue, my_data);
```

### 3\. Dequeue

Remove an item from the head of the queue. Lock-free; safe to call from any number of threads concurrently.

```c
void *faa_queue_dequeue(FAAArrayQueue_t *q);
```

  * **Returns**: The dequeued item, or `nullptr` if the queue was empty. The result is `[[nodiscard]]`.

```c
int* received = faa_queue_dequeue(my_queue);
if (received != nullptr) {
    printf("Dequeued: %d\n", *received);
    free(received);
}
```

### 4\. Destruction

Free all memory associated with the queue. Call it only when no other thread is accessing the queue (after all producer/consumer threads have been joined). Remaining items are drained in FIFO order through `free_payload` (if non-null) before the memory is freed.

```c
void faa_queue_destroy(FAAArrayQueue_t *q, void (*free_payload)(void *));
```

```c
faa_queue_destroy(my_queue, free);
```

### Complete Example

```c
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "faaq.h"

int main(void) {
    FAAArrayQueue_t* q = faa_queue_create();
    assert(q != nullptr);

    faa_queue_enqueue(q, (void*)10);
    faa_queue_enqueue(q, (void*)20);

    void* item1 = faa_queue_dequeue(q);
    void* item2 = faa_queue_dequeue(q);

    printf("Dequeued: %ld\n", (intptr_t)item1); // Prints 10
    printf("Dequeued: %ld\n", (intptr_t)item2); // Prints 20
    assert(faa_queue_dequeue(q) == nullptr);   // empty

    faa_queue_destroy(q, nullptr);
    return 0;
}
```

See `example.c` for a multi-threaded producer/consumer example.

## How the per-thread state works (why there is no `tid`)

The original FAAArrayQueue takes a thread id because its hazard pointers live in a per-queue array indexed by `tid`. Here that state lives in the thread instead:

  * Each thread has a small thread-local cache (`FAAQ_TLS_SLOTS`, default 4, most-recently-used first) of per-queue entries: two hazard pointers plus the last head and tail node it protected. The hot path is one thread-local compare of the queue's id against the front entry.
  * Queues carry a process-unique 64-bit id that is never reused, so an entry left behind by a destroyed queue can never match a new queue allocated at the same address.
  * A thread that touches more queues than it has slots evicts the least recently used entry (correct, just slower). A thread that exits releases everything through a C23 `tss_t` destructor; hazard pointer records are recycled for later threads.
  * Stale entries for a destroyed queue in *other* threads are inert; until they are evicted or the thread exits, each one pins at most two node-sized allocations from reuse. This is the one resource cost of the design.

Reclaimed nodes are cached per thread (`FAAQ_NODE_CACHE_CAPACITY`, default 256) and freed at thread exit.

## What is different from the original FAAArrayQueue

Three things, each measured on its own (see the comparison below):

  * **Cached protection.** A thread keeps its hazard pointers on the head and tail nodes it last used, so the protect-validate sequence (with its seq_cst fence) runs only when it moves to another node, not on every operation.
  * **No hot-line read on the pop side.** The empty check compares `deqidx` with `enqidx`, and `enqidx` is the line every enqueuer hammers. It only grows, so a value read earlier on the same node is a lower bound: while `deqidx` is below it the queue is provably non-empty and the read is skipped.
  * **Speculative claim** (`FAAQ_SPEC_CLAIM`, default 16). Under contention the pop's `deqidx` load and its fetch-add each miss on the same line, because other dequeuers steal it in between. When the last `enqidx` a thread saw lies more than the slack beyond its own last claimed index, producer-claimed items provably lie ahead and the dequeue claims with the fetch-add alone. An overshoot (other dequeuers consumed the slack) shows up as an empty slot, is poisoned immediately, and the next attempt takes the checked path; linearizability is unchanged because a claimed index is handled exactly as a claimed-but-not-yet-stored slot already was.

The hot paths of both operations are call-free: every slow path (slot miss, re-protecting the cached node, node full, node drained, waiting for a pending slot) lives in an out-of-line helper that finishes by re-entering the operation, which the compiler turns into a tail jump. The fast dequeue is 20 instructions with no stack frame and no saved register; the fast enqueue inlines into its caller as 11 instructions from the queue-id compare to the CAS.

Things that were tried and measured as useless or harmful on this code, so they are not in it: pacing the fetch-adds by observed contention, a per-operation fence, tighter or delayed polling of pending slots, consumer hold-off heuristics, backoff on empty pops, validating the cached node against the live head/tail pointer, alternative node alignments, and shorter reclamation scan intervals.

## Building and testing

```
make test          # ASan + UBSan build of test_faaq.c, then run
make tsan          # ThreadSanitizer build of the suite and of the fuzz harness, then run
make bench         # -O3 -flto benchmark driver: ./bench_faaq [-s sec] [-t 1,2,4] [-w sym|pc|mix|both|all] [-r reps] [-p] [-i inflight]
make example
```

The optimized targets pass `-fomit-frame-pointer` explicitly: the nixpkgs cc wrapper appends `-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer` to every x86-64 compile (it is not part of the hardening that the flake disables, and there is no knob), and a flag on the command line wins. Sanitizer builds keep frame pointers for their stack traces.

`test_faaq.c` covers teardown/leaks, a deterministic regression for a thread whose cached head node was drained and unlinked by other threads, many queues per thread (slot eviction), 300 short-lived threads (hazard pointer record recycling), exact-once MPMC delivery at 4+4 and 16+16 threads, and a scaling benchmark. Under ThreadSanitizer the suite shrinks the concurrent phases (1/20 scale, and 8+8 threads at 1/100 for the oversubscribed one: TSan serializes every atomic through per-address locks, and 32 polling threads turn that into a convoy) and routes thread creation through pthreads (`test_threads.h`), because glibc's `thrd_create` calls its pthread internals directly and bypasses TSan's interceptors.

## Fuzzing

`fuzz_faaq.c` is a model-based harness in the libFuzzer convention (`LLVMFuzzerTestOneInput`). The input bytes are a small program over six live queues: enqueue, dequeue, bulk enqueue/dequeue, destroy+recreate, explicit hazard pointer cleanup, drain, and BURST, which runs a real multi-threaded phase (1-4 producers, 1-4 consumers) and checks exact-once delivery, per-producer FIFO order, emptiness afterwards, and (with one consumer) that a dequeue never reports "empty" once all producers have finished. Every single-threaded dequeue is checked against a reference FIFO and every destroy must drain exactly the model's contents in order.

The fuzz builds shrink the tunables (`FUZZ_CFG` in the Makefile: 4-slot nodes, hazard pointer scans every 8 retirements, no spin-waits, 2 thread-local slots, no node cache) so node boundaries, reclamation, slot poisoning, per-thread slot eviction and real `free()`s all happen every few operations.

```
make fuzz-smoke                 # gcc + ASan/UBSan, PRNG-driven, no fuzzing engine needed
make fuzz-afl FUZZ_SEC=600      # AFL++ (afl-clang-lto, ASan+UBSan), campaign into fuzz/out
make fuzz-libfuzzer FUZZ_SEC=300 # libFuzzer, in the flake's clang shell (nix develop .#clang)
```

The flake's dev shells provide AFL++ (`aflplusplus`, the actively developed general-purpose fuzzer for C; libFuzzer has been maintenance-only since 2022). `afl-clang-lto` gives collision-free LTO edge coverage and CmpLog; the same harness runs unchanged under libFuzzer. For more cores, add secondary instances: `afl-fuzz -S s1 -i fuzz/seeds -o fuzz/out -- ./fuzz_faaq_afl`. Do not set `AFL_CC` in the environment: afl-cc reads it as the name of its backend compiler.

## Comparison with xenium's `ramalhete_queue`

[xenium](https://github.com/mpoeter/xenium) has a C++17 FAAArrayQueue (`ramalhete_queue`) with pluggable reclamation. `bench_xenium.cpp` is a line-for-line mirror of `bench_faaq.c` (same barrier, pinning, isolated control words, pre-fill, drain, CSV), each queue in its own language, both built by the same GCC 15 with `-O3 -flto -DNDEBUG`. Following xenium's own harness, every batch of 100 operations runs inside a `region_guard`. Workloads: `sym` (every thread alternates push and pop; the queue stays near-empty and pushers and poppers contend on the same node), `mix` (every thread pushes or pops with equal probability; the random walk keeps a backlog of thousands of items, so pushers and poppers work on different nodes; every operation counts), and `pc` (half producers, half consumers, producers throttled to 8192 items in flight).

```
make bench_xenium XENIUM_DIR=/path/to/xenium
make compare-xenium XENIUM_DIR=/path/to/xenium > compare.csv
```

Results on a Ryzen 9 9950X3D (one thread per physical core, pinned, 2 s runs, medians of 5 interleaved repetitions, spread within ±1 percent unless noted, both sides built with `-O3 -flto -fomit-frame-pointer`), in M ops/s. `x-hp` is xenium with hazard pointers, `x-qsbr` with quiescent-state-based reclamation; the other three xenium reclaimers (EBR, NEBR, DEBRA) score between those two or below.

| workload | threads | ours | x-hp | x-qsbr | ours / best xenium |
|---|---|---|---|---|---|
| sym | 1 | 174.9 | 59.7 | 108.2 | 1.57x (NEBR 111.2) |
| sym | 2 | 71.4 | 47.8 | 51.2 | 1.40x |
| sym | 4 | 73.8 | 61.0 | 48.2 | 1.21x |
| sym | 8 | 73.3 | 66.2 | 47.4 | 1.11x |
| sym | 16 | 43.9 | 40.4 | 30.1 | 1.09x |
| mix | 1 | 162.6 | 63.6 | 103.6 | 1.54x (NEBR 105.4) |
| mix | 2 | 85.4 | 63.6 | 60.6 | 1.34x |
| mix | 4 | 90.6 | 90.1 | 58.6 | 1.01x |
| mix | 8 | 88.7 | 109.5 | 60.0 | 0.81x |
| mix | 16 | 60.6 | 77.9 | 42.1 | 0.78x |
| pc | 2 | 42.3 | 31.2 | 35.7 | 1.18x (NEBR 35.8) |
| pc | 4 | 43.3 | 55.9 | 34.8 | 0.77x |
| pc | 8 | 42.1 | 62.2 | 36.5 | 0.68x |
| pc | 16 | 25.0 | 29.1 | 25.2 | 0.86x |

Reading: against xenium's fence-free reclaimers (QSBR, NEBR, EBR, DEBRA), which pay the same per-operation cost we do, this queue is 1.1x to 1.6x faster than the best of them at every point but the throttled split at 16 threads, where they and ours are within 4 percent of each other, and up to 2.2x faster than the slowest of them. xenium's hazard-pointer variant is the slowest of the five single-threaded (a seq_cst fence per operation) but the fastest under contention, and it holds three regions: the random mix at 8 and 16 threads, and the throttled producer/consumer split at 4, 8 and 16 threads. The latter is a regime artifact of that workload: sampling the queue occupancy shows xenium-HP sitting at the 8192-item cap (its slower pop keeps the queue full, so producers idle on the driver's throttle and pops never wait) while every other variant, ours included, sits near empty. The mix gap is real. Hardware counters at 8 threads show the same cycle budget, xenium-HP retiring 111 instructions per operation at IPC 0.28 against our 46 at IPC 0.09, i.e. we stall more per access on the contended lines; giving our queue the same per-operation fence, or any of the other mechanisms listed above, makes ours slower, so the cause is not the fence itself and was not isolated.

## References

This work is directly inspired by:

- The CPP implementation of [FAAArrayQueue](https://concurrencyfreaks.blogspot.com/2016/11/faaarrayqueue-mpmc-lock-free-queue-part.html)

- Folly's Hazard Pointer Implementation [folly](https://github.com/facebook/folly)
