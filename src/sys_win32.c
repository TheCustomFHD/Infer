/* sys_win32.c -- file mapping for sys.h on Windows XP and later.
 *
 * Uses CreateFileMapping / MapViewOfFile, both present since Win32s.
 * The only Windows-specific file besides net_win32.c.
 *
 * ANSI C (C89) plus the Win32 API.
 */

#include "sys.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501     /* Windows XP */
#endif

#include <windows.h>
#include <stdlib.h>

struct sys_map {
    HANDLE file;
    HANDLE mapping;
    void  *base;
    size_t skew;
};

sys_map *sys_map_file(const char *path, double offset, double length) {
    sys_map *m;
    HANDLE file, mapping;
    void *base;
    SYSTEM_INFO si;
    DWORD gran;
    double aligned;
    size_t skew, viewlen;
    DWORD off_lo, off_hi;

    if (length <= 0.0) return NULL;
    if (length > 2000.0 * 1024.0 * 1024.0) return NULL;

    /* View offsets must be a multiple of the allocation granularity
     * (64 KB on x86), not merely the page size. */
    GetSystemInfo(&si);
    gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 65536;

    aligned = (double) ((long) (offset / (double) gran)) * (double) gran;
    skew    = (size_t) (offset - aligned);
    viewlen = (size_t) (length + (double) skew);

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                       NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;

    mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) {
        CloseHandle(file);
        return NULL;
    }

    off_hi = (DWORD) (aligned / 4294967296.0);
    off_lo = (DWORD) (aligned - (double) off_hi * 4294967296.0);

    base = MapViewOfFile(mapping, FILE_MAP_READ, off_hi, off_lo, viewlen);
    if (!base) {
        CloseHandle(mapping);
        CloseHandle(file);
        return NULL;
    }

    m = (sys_map *) malloc(sizeof(sys_map));
    if (!m) {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        CloseHandle(file);
        return NULL;
    }
    m->file    = file;
    m->mapping = mapping;
    m->base    = base;
    m->skew    = skew;
    return m;
}

const unsigned char *sys_map_data(sys_map *m) {
    return m ? (const unsigned char *) m->base + m->skew : NULL;
}

void sys_map_advise_random(sys_map *m) {
    /* Requested up front via FILE_FLAG_RANDOM_ACCESS. */
    (void) m;
}

void sys_map_close(sys_map *m) {
    if (!m) return;
    UnmapViewOfFile(m->base);
    CloseHandle(m->mapping);
    CloseHandle(m->file);
    free(m);
}

double sys_time_sec(void) {
    /* QueryPerformanceCounter has existed since NT 3.1 / Win95 and is
     * the only sub-millisecond timer available on XP (GetTickCount has
     * ~10-16 ms granularity). Falls back to GetTickCount if the machine
     * genuinely has no performance counter. */
    static int inited = 0;
    static double period = 0.0;
    LARGE_INTEGER v;

    if (!inited) {
        LARGE_INTEGER freq;
        inited = 1;
        if (QueryPerformanceFrequency(&freq) && freq.QuadPart != 0) {
            period = 1.0 / (double) freq.QuadPart;
        }
    }

    if (period != 0.0 && QueryPerformanceCounter(&v)) {
        return (double) v.QuadPart * period;
    }
    return (double) GetTickCount() * 1e-3;
}
