/* sys_posix.c -- mmap-based file mapping for sys.h (Linux / Unix).
 *
 * The only POSIX-specific file besides net_posix.c.
 * ANSI C (C89) plus the POSIX mmap API.
 */

#include "sys.h"

#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

struct sys_map {
    void   *base;      /* page-aligned mapping start */
    size_t  maplen;
    size_t  skew;      /* offset - page-aligned start */
};

sys_map *sys_map_file(const char *path, double offset, double length) {
    sys_map *m;
    void *base;
    long pagesz;
    double aligned;
    size_t skew, maplen;
    long   page_index;
    int fd;

    if (length <= 0.0) return NULL;

    /* On a 32-bit host without _FILE_OFFSET_BITS=64 the mapping window
     * must fit in size_t; refuse politely so the caller can fall back. */
    if (length > 2000.0 * 1024.0 * 1024.0) return NULL;

    pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;

    /* Page-align the offset, keeping a `long` page *index* rather than a
     * byte offset, and never naming a 64-bit type.
     *
     * This matters on Solaris with Sun Studio in strict C90 mode. -Xc
     * undefines _LONGLONG_TYPE, because C90 has no `long long`, and
     * <sys/types.h> then falls back to
     *
     *     typedef union { double _d; int32_t _l[2]; } longlong_t;
     *
     * With _FILE_OFFSET_BITS=64 on a 32-bit build off_t becomes that
     * union: it cannot be cast to, assigned from an integer, or used in
     * arithmetic, and passing anything to mmap()'s offset argument
     * fails to compile.
     *
     * The supported configuration is therefore a 64-bit build (-m64),
     * where off_t is already a plain 64-bit long and large-file support
     * is automatic. Passing `page_index * pagesz` -- long times long --
     * is then correct everywhere, and on 32-bit glibc it still widens
     * implicitly to whatever off_t is. */
    page_index = (long) (offset / (double) pagesz);
    aligned = (double) page_index * (double) pagesz;
    skew    = (size_t) (offset - aligned);
    maplen  = (size_t) (length + (double) skew);

    fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    base = mmap(NULL, maplen, PROT_READ, MAP_PRIVATE, fd,
                page_index * pagesz);

    /* The descriptor is not needed once the mapping exists. */
    close(fd);

    if (base == MAP_FAILED) return NULL;

    m = (sys_map *) malloc(sizeof(sys_map));
    if (!m) {
        munmap(base, maplen);
        return NULL;
    }
    m->base   = base;
    m->maplen = maplen;
    m->skew   = skew;
    return m;
}

const unsigned char *sys_map_data(sys_map *m) {
    return m ? (const unsigned char *) m->base + m->skew : NULL;
}

void sys_map_advise_random(sys_map *m) {
    /* Advise WILLNEED, and deliberately NOT SEQUENTIAL.
     *
     * The access pattern is sequential WITHIN one token and then
     * starts over from the beginning for the next token. Those two
     * facts pull in opposite directions, and which one you encode
     * matters enormously on a small machine.
     *
     * MADV_RANDOM was the original advice and was wrong: it switches
     * readahead off, so each fault fetched one page instead of a run.
     *
     * MADV_SEQUENTIAL replaced it and is ALSO wrong, but only on
     * Solaris, which is why it survived. The two kernels do not mean
     * the same thing by it:
     *
     *   Linux    widens readahead. Pages are NOT dropped behind the
     *            read pointer.
     *   Solaris  "pages will be accessed only once and can be freed
     *            behind the current access point" -- the hint is
     *            licence to DISCARD what we just read.
     *
     * On Solaris that turns every generated token into a full re-read
     * of the model from disk, because the pages the next token needs
     * are exactly the ones the hint just told the VM to throw away.
     * Reported from a real UltraSPARC as "VERY high disk usage", with
     * generation pinned near the disk's throughput rather than the
     * CPU's.
     *
     * MADV_WILLNEED alone gives the useful half -- start pulling the
     * mapping in -- with no licence to evict. Neither hint forces
     * residency, so a machine with less RAM than the model still
     * works; it just pages, which is the honest behaviour.
     *
     * The function keeps its name so callers do not change. */
#if defined(MADV_WILLNEED)
    if (m) madvise(m->base, m->maplen, MADV_WILLNEED);
#endif
    (void) m;
}


void sys_map_close(sys_map *m) {
    if (!m) return;
    munmap(m->base, m->maplen);
    free(m);
}

double sys_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec + (double) tv.tv_usec * 1e-6;
}
