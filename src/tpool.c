/* tpool.c -- a minimal persistent thread pool for row-parallel matvec.
 *
 * WHY
 * ---
 * Every matrix-vector product in this engine is a loop over independent
 * output rows: y[r] = dot(w_row_r, x). Nothing is shared between rows
 * except the read-only weights and the read-only quantised activation,
 * so splitting the row range across cores needs no locking on the data
 * itself and produces bit-identical results -- each row is computed by
 * exactly the same code, just on a different core.
 *
 * That last point matters: this is not a reassociation. Row r's float
 * sum is accumulated in the same order regardless of which thread runs
 * it, so `--threads 4` and `--threads 1` agree bit-for-bit.
 * tests/t_thread.c asserts that.
 *
 * WHY NOT OpenMP
 * --------------
 * -fopenmp is a compiler extension and a runtime library dependency.
 * The project ships to Sun Studio on Solaris, mingw on Windows XP, and
 * a 486 with no threads at all. A ~200-line pool over the platform's
 * own primitives keeps the dependency list at libc + OS.
 *
 * PORTABILITY
 * -----------
 *   INFER_HAVE_THREADS not defined  -> every function is a stub, the
 *                                      caller runs the whole range
 *                                      inline. This is the 486 path,
 *                                      and it compiles to nothing.
 *   _WIN32                          -> CreateThread + events. XP-safe:
 *                                      no condition variables (Vista+),
 *                                      no InitializeCriticalSectionEx.
 *   otherwise                       -> pthreads.
 *
 * The pool is created once, on first use, and the worker threads park
 * on a semaphore between jobs. Spawning threads per matvec would cost
 * more than the work: at 0.14 s per forward pass and ~100 matvecs in
 * it, each call has roughly a millisecond of work to give away.
 */

#include "tpool.h"

#ifdef INFER_HAVE_THREADS

#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define TP_MAX_THREADS 64

typedef struct {
    tp_fn   fn;
    void   *arg;
    long    lo, hi;      /* row range for THIS worker */
} tp_job;

static int      tp_n        = 0;    /* worker threads, excluding caller */
static int      tp_started  = 0;
static int      tp_quit     = 0;
static tp_job   tp_jobs[TP_MAX_THREADS];

#if defined(_WIN32)
static HANDLE   tp_go[TP_MAX_THREADS];    /* wake worker i */
static HANDLE   tp_done[TP_MAX_THREADS];  /* worker i finished */
static HANDLE   tp_th[TP_MAX_THREADS];
#else
static pthread_t       tp_th[TP_MAX_THREADS];
static pthread_mutex_t tp_mx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  tp_cv_go   = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  tp_cv_done = PTHREAD_COND_INITIALIZER;
static volatile int    tp_gen     = 0;   /* incremented per job batch */
static volatile int    tp_seen[TP_MAX_THREADS];
static volatile int    tp_left    = 0;
#endif

/* ------------------------------------------------------------------ */

#if defined(_WIN32)
static DWORD WINAPI tp_worker(LPVOID p) {
    int id = (int) (INT_PTR) p;
    for (;;) {
        WaitForSingleObject(tp_go[id], INFINITE);
        if (tp_quit) break;
        if (tp_jobs[id].fn && tp_jobs[id].hi > tp_jobs[id].lo)
            tp_jobs[id].fn(tp_jobs[id].arg, tp_jobs[id].lo, tp_jobs[id].hi);
        SetEvent(tp_done[id]);
    }
    return 0;
}
#else
static void *tp_worker(void *p) {
    int id = (int) (long) p;
    int mygen = 0;
    for (;;) {
        pthread_mutex_lock(&tp_mx);
        while (tp_gen == mygen && !tp_quit)
            pthread_cond_wait(&tp_cv_go, &tp_mx);
        if (tp_quit) { pthread_mutex_unlock(&tp_mx); break; }
        mygen = tp_gen;
        pthread_mutex_unlock(&tp_mx);

        if (tp_jobs[id].fn && tp_jobs[id].hi > tp_jobs[id].lo)
            tp_jobs[id].fn(tp_jobs[id].arg, tp_jobs[id].lo, tp_jobs[id].hi);

        pthread_mutex_lock(&tp_mx);
        tp_seen[id] = mygen;
        if (--tp_left == 0) pthread_cond_signal(&tp_cv_done);
        pthread_mutex_unlock(&tp_mx);
    }
    return NULL;
}
#endif

int tp_cpu_count(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int) si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int) n : 1;
#else
    return 1;
#endif
}

int tp_start(int nthreads) {
    int i;

    if (tp_started) return tp_n + 1;
    if (nthreads <= 0) nthreads = tp_cpu_count();
    if (nthreads > TP_MAX_THREADS) nthreads = TP_MAX_THREADS;
    if (nthreads < 1) nthreads = 1;

    /* The calling thread is one of the workers, so only nthreads-1
     * extra threads are spawned. On a single core this is a no-op and
     * the pool never touches an OS primitive. */
    tp_n = nthreads - 1;
    tp_started = 1;
    if (tp_n <= 0) { tp_n = 0; return 1; }

#if defined(_WIN32)
    for (i = 0; i < tp_n; i++) {
        tp_go[i]   = CreateEvent(NULL, FALSE, FALSE, NULL);
        tp_done[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
        tp_th[i]   = CreateThread(NULL, 0, tp_worker,
                                  (LPVOID) (INT_PTR) i, 0, NULL);
        if (!tp_th[i]) { tp_n = i; break; }
    }
#else
    for (i = 0; i < tp_n; i++) {
        tp_seen[i] = 0;
        if (pthread_create(&tp_th[i], NULL, tp_worker, (void *) (long) i)
            != 0) {
            tp_n = i;
            break;
        }
    }
#endif
    return tp_n + 1;
}

int tp_threads(void) { return tp_started ? tp_n + 1 : 1; }

/* Split [0,n) across tp_n+1 workers and run fn on each piece. The
 * caller takes the LAST slice and runs it inline, so on a single core
 * this is a plain call with no synchronisation at all. */
void tp_parallel_for(long n, tp_fn fn, void *arg) {
    long per, lo;
    int i, nw;

    if (!tp_started || tp_n == 0 || n <= 1) {
        fn(arg, 0, n);
        return;
    }

    nw  = tp_n + 1;
    /* Round the split up so the caller's slice is the remainder and is
     * never larger than a worker's -- the caller also pays the
     * dispatch cost. */
    per = (n + nw - 1) / nw;
    if (per < 1) per = 1;

#if defined(_WIN32)
    lo = 0;
    for (i = 0; i < tp_n; i++) {
        long hi = lo + per; if (hi > n) hi = n;
        tp_jobs[i].fn = fn; tp_jobs[i].arg = arg;
        tp_jobs[i].lo = lo; tp_jobs[i].hi = hi;
        lo = hi;
        SetEvent(tp_go[i]);
    }
    if (lo < n) fn(arg, lo, n);
    for (i = 0; i < tp_n; i++) WaitForSingleObject(tp_done[i], INFINITE);
#else
    pthread_mutex_lock(&tp_mx);
    lo = 0;
    for (i = 0; i < tp_n; i++) {
        long hi = lo + per; if (hi > n) hi = n;
        tp_jobs[i].fn = fn; tp_jobs[i].arg = arg;
        tp_jobs[i].lo = lo; tp_jobs[i].hi = hi;
        lo = hi;
    }
    tp_left = tp_n;
    tp_gen++;
    pthread_cond_broadcast(&tp_cv_go);
    pthread_mutex_unlock(&tp_mx);

    if (lo < n) fn(arg, lo, n);

    pthread_mutex_lock(&tp_mx);
    while (tp_left > 0) pthread_cond_wait(&tp_cv_done, &tp_mx);
    pthread_mutex_unlock(&tp_mx);
#endif
}

void tp_stop(void) {
    int i;
    if (!tp_started) return;
    tp_quit = 1;
#if defined(_WIN32)
    for (i = 0; i < tp_n; i++) SetEvent(tp_go[i]);
    for (i = 0; i < tp_n; i++) {
        WaitForSingleObject(tp_th[i], 1000);
        CloseHandle(tp_th[i]); CloseHandle(tp_go[i]); CloseHandle(tp_done[i]);
    }
#else
    pthread_mutex_lock(&tp_mx);
    tp_gen++;
    pthread_cond_broadcast(&tp_cv_go);
    pthread_mutex_unlock(&tp_mx);
    for (i = 0; i < tp_n; i++) pthread_join(tp_th[i], NULL);
#endif
    tp_n = 0; tp_started = 0; tp_quit = 0;
}

#else  /* !INFER_HAVE_THREADS -- 486 and any platform without threads */

void tp_parallel_for(long n, tp_fn fn, void *arg) { fn(arg, 0, n); }
int  tp_start(int nthreads)  { (void) nthreads; return 1; }
int  tp_threads(void)        { return 1; }
int  tp_cpu_count(void)      { return 1; }
void tp_stop(void)           { }

#endif
