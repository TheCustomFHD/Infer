/* sys.h -- platform-independent read-only file mapping.
 *
 * Same philosophy as net.h: no file above this header may include an OS
 * header. One implementation per platform:
 *
 *   sys_posix.c   -- Linux / Unix (mmap)
 *   sys_win32.c   -- Windows XP and later (CreateFileMapping)
 *
 * Mapping matters a great deal on the i486 target. A Q4_K_M Qwen3.5-0.8B
 * has ~500 MB of tensor data, far more than such a machine's RAM. Mapped
 * read-only, the weights are demand-paged from disk and clean pages can
 * be evicted under pressure without ever touching swap, so the resident
 * set stays close to the working set rather than the file size.
 *
 * ANSI C (C89).
 */

#ifndef INFER_SYS_H
#define INFER_SYS_H

#include <stddef.h>

typedef struct sys_map sys_map;

/* Map [offset, offset+length) of `path` read-only.
 * `offset` is a double so it can exceed LONG_MAX on 32-bit hosts; the
 * implementation rounds it down to a page boundary internally.
 * Returns NULL if mapping is unavailable -- the caller should then fall
 * back to reading the data into a malloc'd buffer. */
sys_map *sys_map_file(const char *path, double offset, double length);

/* Pointer to the first mapped byte (already adjusted for page rounding). */
const unsigned char *sys_map_data(sys_map *m);

void sys_map_close(sys_map *m);

/* Advise the OS that access will be mostly sequential / random.
 * A no-op where the platform has no equivalent. */
void sys_map_advise_random(sys_map *m);

/* ------------------------------------------------------------------ */
/* Monotonic wall clock, in seconds.                                   */
/*                                                                     */
/* C89 only guarantees clock(), which measures CPU time with a         */
/* resolution that can be as coarse as 10 ms and which wraps after ~36 */
/* minutes on a 32-bit CLOCKS_PER_SEC=1000000 host -- both fatal for   */
/* profiling a machine that spends 30 s per token. Each platform       */
/* backend provides something better:                                  */
/*                                                                     */
/*   POSIX  gettimeofday()          (microseconds)                     */
/*   Win32  QueryPerformanceCounter (sub-microsecond; XP-safe)         */
/*                                                                     */
/* The absolute epoch is unspecified; only differences are meaningful. */
double sys_time_sec(void);

#endif
