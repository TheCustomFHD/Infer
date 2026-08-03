# `src/sys.h` + backends — file mapping and time

| file | platform |
|---|---|
| `sys.h` | the contract |
| `sys_posix.c` | `mmap`, `gettimeofday` |
| `sys_win32.c` | `CreateFileMapping`, `QueryPerformanceCounter` |

Same philosophy as [net-h.md](net-h.md): no file above this header
includes an OS header.

## Mapping

```c
sys_map *sys_map_file(const char *path, double offset, double length);
const unsigned char *sys_map_data(sys_map *m);
void sys_map_advise_random(sys_map *m);
void sys_map_close(sys_map *m);
```

`offset` is a `double` so it can exceed `LONG_MAX` on a 32-bit host.

**Why this matters:** a Q4_K_M Qwen3.5-0.8B has ~500 MB of tensor data.
Mapped read-only, weights are demand-paged and clean pages can be
evicted under pressure without ever touching swap. The 0.8B model idles
at **~44 MB RSS** and startup is immediate — there is no load-time copy.

Returning `NULL` is a valid answer; `gguf.c` falls back to `malloc` +
`fread`.

Windows note: view offsets must be a multiple of the **allocation
granularity** (64 KB on x86), not merely the page size. `sys_win32.c`
rounds down and adds the remainder back as a skew.

## Time

```c
double sys_time_sec(void);
```

C89's `clock()` measures CPU time with up to 10 ms granularity and wraps
after ~36 minutes at 32-bit `CLOCKS_PER_SEC=1000000` — both fatal for
profiling a machine that spends 30 s per token. POSIX uses
`gettimeofday()`; Win32 uses `QueryPerformanceCounter`, which is the only
sub-millisecond timer on XP, with a `GetTickCount` fallback.

## See also

- [gguf-c.md](gguf-c.md) · [prof-c.md](prof-c.md)
