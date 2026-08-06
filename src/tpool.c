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

/* Compiler barrier. x86 and SPARC (TSO) do not reorder the
 * store/load pairs we rely on; this only stops the COMPILER from
 * hoisting the spin loads out. */
#if defined(__GNUC__)
#define TP_BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#define TP_BARRIER() do { } while (0)
#endif

/* Hand the core back for one scheduling quantum.
 *
 * sched_yield() lives in librt on Solaris, not libc, so calling it
 * unconditionally would add a link dependency to a platform that
 * builds with plain -lpthread. pthread_yield is not portable either.
 * The spin path only runs when the pool is NOT oversubscribed, so
 * every worker already owns a core and the yield is a rare
 * belt-and-braces case: doing nothing is correct there, just slightly
 * less polite. Where a yield is free we use it. */
#if defined(_WIN32)
#define TP_YIELD() Sleep(0)
#elif defined(_POSIX_PRIORITY_SCHEDULING)
#include <sched.h>
#define TP_YIELD() sched_yield()
#else
#define TP_YIELD() do { } while (0)
#endif

/* Spin budget before falling back to the condition variable.
 *
 * Spinning only pays when every worker owns a core. If the user asks
 * for more threads than there are cores, the spinners steal the very
 * CPU the running workers need and throughput collapses (measured:
 * -T 4 on 2 cores, 17.2 -> 10.4 tok/s). So the budget is set to zero
 * whenever the pool is oversubscribed, which restores the pure
 * condition-variable behaviour exactly. */
static long tp_spin = 0L;
static volatile int tp_sdone[TP_MAX_THREADS];
static volatile int tp_sgen = 0;

/* ------------------------------------------------------------------ */

#if defined(_WIN32)
static DWORD WINAPI tp_worker(LPVOID p) {
    int id = (int) (INT_PTR) p;
    int mygen = 0;
    for (;;) {
        long spins = 0;
        /* Same trade as the pthread path: poll the generation stamp
         * first so a back-to-back batch costs no kernel transition,
         * and only fall back to the event when the budget runs out.
         * tp_spin is 0 when oversubscribed, which blocks immediately
         * and reproduces the original behaviour exactly. */
        for (;;) {
            TP_BARRIER();
            if (tp_sgen != mygen || tp_quit) break;
            if (++spins > tp_spin) break;
        }
        TP_BARRIER();
        /* MUST be a loop, not an `if`.
         *
         * tp_go is an AUTO-RESET event and the dispatcher signals it
         * unconditionally on every batch, whether or not this worker
         * was blocked. A signal that arrives while the worker is
         * spinning is therefore still latched, and the NEXT wait
         * returns immediately without a new batch existing. With a
         * bare `if` the worker fell through, read tp_sgen unchanged,
         * re-ran the PREVIOUS slice's indices and stamped tp_sdone
         * with a stale generation -- so the dispatcher waited forever
         * for a stamp that had already been written, or accepted one
         * batch early. The pthread path always looped here; this one
         * did not. (Finding 55.)
         */
        while (tp_sgen == mygen && !tp_quit)
            WaitForSingleObject(tp_go[id], INFINITE);
        if (tp_quit) break;
        mygen = tp_sgen;

        if (tp_jobs[id].fn && tp_jobs[id].hi > tp_jobs[id].lo)
            tp_jobs[id].fn(tp_jobs[id].arg, tp_jobs[id].lo, tp_jobs[id].hi);

        TP_BARRIER();
        tp_sdone[id] = mygen;
        TP_BARRIER();
        if (tp_spin <= 0) SetEvent(tp_done[id]);
    }
    return 0;
}
#else
static void *tp_worker(void *p) {
    int id = (int) (long) p;
    int mygen = 0;
    for (;;) {
        long spins = 0;
        /* Spin first: at 2-core the next batch usually arrives within
         * a few microseconds, and a futex round trip costs far more
         * than burning those cycles. tp_spin is 0 when oversubscribed,
         * which skips this entirely and blocks straight away. */
        for (;;) {
            TP_BARRIER();
            if (tp_sgen != mygen || tp_quit) break;
            if (++spins > tp_spin) break;
        }
        TP_BARRIER();
        if (tp_sgen == mygen && !tp_quit) {
            pthread_mutex_lock(&tp_mx);
            while (tp_sgen == mygen && !tp_quit)
                pthread_cond_wait(&tp_cv_go, &tp_mx);
            pthread_mutex_unlock(&tp_mx);
        }
        if (tp_quit) break;
        mygen = tp_sgen;

        if (tp_jobs[id].fn && tp_jobs[id].hi > tp_jobs[id].lo)
            tp_jobs[id].fn(tp_jobs[id].arg, tp_jobs[id].lo, tp_jobs[id].hi);

        TP_BARRIER();
        if (tp_spin > 0) {
            /* Spin mode: publish a stamp the joiner polls. */
            tp_sdone[id] = mygen;
            TP_BARRIER();
        } else {
            /* Blocking mode: the joiner is asleep on tp_cv_done, so
             * signal it the same way the original pool did. */
            pthread_mutex_lock(&tp_mx);
            tp_sdone[id] = mygen;
            if (--tp_left == 0) pthread_cond_signal(&tp_cv_done);
            pthread_mutex_unlock(&tp_mx);
        }
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

    /* Only spin when we are not oversubscribed. */
    tp_spin = (nthreads <= tp_cpu_count()) ? 40000L : 0L;

#if defined(_WIN32)
    /* Every event must exist before ANY worker runs.
     *
     * Creating handle i and then immediately spawning worker i means
     * worker 0 can reach WaitForSingleObject(tp_go[0]) while this loop
     * is still creating tp_go[1]. That is survivable, but the same
     * interleaving lets a worker observe a half-initialised table --
     * and on the failure path (`if (!tp_th[i]) { tp_n = i; break; }`)
     * it leaves already-running workers with an id >= tp_n, which the
     * dispatcher then never schedules and never joins.
     *
     * Two passes: allocate everything, then start everything. */
    for (i = 0; i < tp_n; i++) {
        tp_sdone[i] = 0;
        tp_go[i]   = CreateEvent(NULL, FALSE, FALSE, NULL);
        tp_done[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!tp_go[i] || !tp_done[i]) { tp_n = i; break; }
    }
    for (i = 0; i < tp_n; i++) {
        tp_th[i] = CreateThread(NULL, 0, tp_worker,
                                (LPVOID) (INT_PTR) i, 0, NULL);
        if (!tp_th[i]) { tp_n = i; break; }
    }
#else
    for (i = 0; i < tp_n; i++) {
        tp_seen[i] = 0; tp_sdone[i] = 0;
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
    }
    TP_BARRIER();
    tp_sgen++;
    TP_BARRIER();
    /* A spinning worker sees the stamp; one that already blocked needs
     * the event. Setting it either way is harmless -- an auto-reset
     * event left signalled just makes the next wait return at once,
     * and the generation stamp is what actually gates the work. */
    for (i = 0; i < tp_n; i++) SetEvent(tp_go[i]);

    if (lo < n) fn(arg, lo, n);

    if (tp_spin > 0) {
        for (i = 0; i < tp_n; i++) {
            long spins = 0;
            for (;;) {
                TP_BARRIER();
                if (tp_sdone[i] == tp_sgen) break;
                if (++spins > tp_spin) { TP_YIELD(); spins = 0; }
            }
        }
        TP_BARRIER();
    } else {
        for (i = 0; i < tp_n; i++) WaitForSingleObject(tp_done[i], INFINITE);
    }
#else
    lo = 0;
    for (i = 0; i < tp_n; i++) {
        long hi = lo + per; if (hi > n) hi = n;
        tp_jobs[i].fn = fn; tp_jobs[i].arg = arg;
        tp_jobs[i].lo = lo; tp_jobs[i].hi = hi;
        lo = hi;
    }
    /* Publish the batch. Workers that are spinning see this without
     * ever entering the kernel; workers that already blocked need the
     * broadcast, so take the lock only to cover that case. */
    pthread_mutex_lock(&tp_mx);
    tp_left = tp_n;
    tp_sgen++;
    pthread_cond_broadcast(&tp_cv_go);
    pthread_mutex_unlock(&tp_mx);

    if (lo < n) fn(arg, lo, n);

    /* Join by spinning on each worker's done stamp. */
    if (tp_spin > 0) {
        /* Every worker owns a core, so poll: the stamp usually lands
         * within a few microseconds and a futex round trip costs far
         * more than that. */
        for (i = 0; i < tp_n; i++) {
            long spins = 0;
            for (;;) {
                TP_BARRIER();
                if (tp_sdone[i] == tp_sgen) break;
                if (++spins > tp_spin) { TP_YIELD(); spins = 0; }
            }
        }
        TP_BARRIER();
    } else {
        /* Oversubscribed: sleep instead of stealing CPU from the
         * workers we are waiting on. */
        pthread_mutex_lock(&tp_mx);
        while (tp_left > 0) pthread_cond_wait(&tp_cv_done, &tp_mx);
        pthread_mutex_unlock(&tp_mx);
    }
#endif
}

void tp_stop(void) {
    int i;
    if (!tp_started) return;
    tp_quit = 1;
#if defined(_WIN32)
    TP_BARRIER();
    tp_sgen++;
    TP_BARRIER();
    for (i = 0; i < tp_n; i++) SetEvent(tp_go[i]);
    for (i = 0; i < tp_n; i++) {
        WaitForSingleObject(tp_th[i], 1000);
        CloseHandle(tp_th[i]); CloseHandle(tp_go[i]); CloseHandle(tp_done[i]);
    }
#else
    pthread_mutex_lock(&tp_mx);
    tp_sgen++;
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
