/* tpool.h -- row-parallel execution for the matvec kernels.
 *
 * See tpool.c. The contract that matters to callers:
 *
 *   - tp_parallel_for(n, fn, arg) calls fn(arg, lo, hi) on disjoint
 *     sub-ranges covering [0, n), and returns when all are done.
 *   - The caller thread runs one of the slices itself, so with one
 *     thread this is exactly `fn(arg, 0, n)` with no synchronisation.
 *   - Results are BIT-IDENTICAL to the serial path: each index is
 *     computed by the same code in the same order, only on a different
 *     core. No partial sums cross threads.
 *
 * Without INFER_HAVE_THREADS every entry point is a stub and the whole
 * facility compiles away, which is what the i486 and Solaris/Studio
 * builds use.
 */

#ifndef INFER_TPOOL_H
#define INFER_TPOOL_H

/* Work function: process rows [lo, hi). */
typedef void (*tp_fn)(void *arg, long lo, long hi);

/* Create the pool. nthreads <= 0 means "one per logical CPU".
 * Returns the total number of participating threads (including the
 * caller). Safe to call more than once; only the first takes effect. */
int  tp_start(int nthreads);

/* Total participating threads, 1 if the pool is off or unavailable. */
int  tp_threads(void);

/* Logical CPUs the OS reports, 1 if unknown. */
int  tp_cpu_count(void);

/* Split [0,n) across the pool and run fn on each slice. */
void tp_parallel_for(long n, tp_fn fn, void *arg);

/* Join and release the workers. */
void tp_stop(void);

#endif /* INFER_TPOOL_H */
