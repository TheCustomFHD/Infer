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

    /* Page-align the offset. Kept as a `long` page *index* rather than a
     * byte offset: with _FILE_OFFSET_BITS=64 on a 32-bit host off_t
     * becomes `long long`, and strict C90 (Sun Studio -Xc -xc99=none)
     * has no such type -- a cast to it is a syntax error, not a
     * warning. Multiplying the index by the page size inside the mmap()
     * call lets the compiler do the widening implicitly, using whatever
     * off_t happens to be, with no 64-bit type named in our source. */
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
#ifdef MADV_RANDOM
    if (m) madvise(m->base, m->maplen, MADV_RANDOM);
#else
    (void) m;
#endif
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
