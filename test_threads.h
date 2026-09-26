// Thread creation for the test/bench/fuzz drivers.
//
// glibc implements thrd_create() and thrd_join() by calling its pthread
// internals directly (__pthread_create_2_1, __pthread_join), which bypasses
// ThreadSanitizer's interceptors: the new thread starts with no TSan state
// (SEGV in __tsan_func_entry) and the join creates no happens-before edge.
// Under TSan the drivers therefore go through pthread_create/pthread_join;
// everywhere else they are the C23 <threads.h> calls. glibc's thrd_t is its
// pthread_t, which the casts below rely on.
#ifndef FAAQ_TEST_THREADS_H
#define FAAQ_TEST_THREADS_H

#include <stdint.h>
#include <threads.h>

#if defined(__SANITIZE_THREAD__)
#    define XTHRD_USE_PTHREAD 1
#elif defined(__has_feature)
#    if __has_feature(thread_sanitizer)
#        define XTHRD_USE_PTHREAD 1
#    endif
#endif

#ifdef XTHRD_USE_PTHREAD
#    include <pthread.h>
#    include <stdlib.h>

typedef struct
{
    thrd_start_t fn;
    void*        arg;
} xthrd_tramp_t;

static void* xthrd_tramp(void* p)
{
    xthrd_tramp_t const t = *(xthrd_tramp_t*)p;
    free(p);
    return (void*)(intptr_t)t.fn(t.arg);
}

static inline int xthrd_create(thrd_t* thr, thrd_start_t fn, void* arg)
{
    xthrd_tramp_t* t = malloc(sizeof *t);
    if (!t)
        return thrd_nomem;
    *t = (xthrd_tramp_t){fn, arg};
    pthread_t pt;
    if (pthread_create(&pt, nullptr, xthrd_tramp, t) != 0) {
        free(t);
        return thrd_error;
    }
    *thr = (thrd_t)pt;
    return thrd_success;
}

static inline int xthrd_join(thrd_t thr, int* res)
{
    void* r;
    if (pthread_join((pthread_t)thr, &r) != 0)
        return thrd_error;
    if (res)
        *res = (int)(intptr_t)r;
    return thrd_success;
}
#else
static inline int xthrd_create(thrd_t* thr, thrd_start_t fn, void* arg)
{
    return thrd_create(thr, fn, arg);
}
static inline int xthrd_join(thrd_t thr, int* res)
{
    return thrd_join(thr, res);
}
#endif

#endif // FAAQ_TEST_THREADS_H
