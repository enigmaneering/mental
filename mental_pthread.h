/*
 * Mental - Portable Threading Abstraction
 *
 * Drop-in shim: on Unix, just includes <pthread.h>.
 * On Windows, provides the same API surface using native primitives:
 *   pthread_mutex_t  ->  SRWLOCK  (lightweight, no kernel transition)
 *   pthread_t        ->  HANDLE   (CreateThread / WaitForSingleObject)
 *   _Thread_local    ->  __declspec(thread)  (MSVC < C11)
 */

#ifndef MENTAL_PTHREAD_H
#define MENTAL_PTHREAD_H

#ifdef _WIN32

/* ------------------------------------------------------------------ */
/*  Windows implementation                                            */
/* ------------------------------------------------------------------ */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* --- Mutex (SRWLOCK, always-initialized, no cleanup needed) --- */

typedef SRWLOCK pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    (void)attr;
    InitializeSRWLock(m);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    AcquireSRWLockExclusive(m);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    ReleaseSRWLockExclusive(m);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m; /* SRWLOCK has no cleanup */
    return 0;
}

/* --- Thread (CreateThread / WaitForSingleObject) --- */

typedef HANDLE pthread_t;

/* pthread_create signature: int (pthread_t*, attr, void*(*)(void*), void*) */
typedef void* (*mental_thread_fn)(void*);

/* Trampoline: Windows threads return DWORD, pthreads return void* */
typedef struct {
    mental_thread_fn fn;
    void *arg;
} mental_thread_trampoline_t;

static DWORD WINAPI mental_thread_trampoline(LPVOID param) {
    mental_thread_trampoline_t ctx = *(mental_thread_trampoline_t*)param;
    free(param);
    ctx.fn(ctx.arg);
    return 0;
}

static inline int pthread_create(pthread_t *t, const void *attr,
                                  void* (*fn)(void*), void *arg) {
    (void)attr;
    mental_thread_trampoline_t *ctx = (mental_thread_trampoline_t*)malloc(sizeof(*ctx));
    if (!ctx) return -1;
    ctx->fn = fn;
    ctx->arg = arg;
    *t = CreateThread(NULL, 0, mental_thread_trampoline, ctx, 0, NULL);
    if (!*t) { free(ctx); return -1; }
    return 0;
}

static inline int pthread_join(pthread_t t, void **retval) {
    (void)retval;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

/* --- Thread-local storage --- */

/* MSVC supports _Thread_local as of VS 2019 16.8 (C11 mode).
 * For older MSVC, __declspec(thread) is the equivalent. */
#if defined(_MSC_VER) && !defined(_Thread_local)
#define _Thread_local __declspec(thread)
#endif

#else

/* ------------------------------------------------------------------ */
/*  Unix — just use pthreads directly                                 */
/* ------------------------------------------------------------------ */

#include <pthread.h>

#endif /* _WIN32 */

#endif /* MENTAL_PTHREAD_H */
