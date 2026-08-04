# Conventions

House style. Most of it follows from one constraint: **this must compile
as ANSI C (C89) on a 1993 compiler and run on a 486.**

---

## The language is C89, not C99

```c
/* correct */
int f(void) {
    int i;
    char buf[64];

    for (i = 0; i < 10; i++) { ... }
    return 0;
}
```

Not allowed:

| construct | why |
|---|---|
| `// comment` | C99 |
| `for (int i = 0; ...)` | declaration in a `for` init |
| declaration after a statement | C89 wants them at the top of a block |
| `long long`, `int64_t` | C90 has neither — and see below, this bites hard |
| variable-length arrays | C99 |
| designated initialisers `{ .x = 1 }` | C99 |
| `inline`, `restrict`, `_Bool` | C99 |
| trailing comma in an enum | C99 |
| `snprintf` | C99; use `sprintf` with a bounded buffer, or `sb_printf` |

`src/backend_mmx.c` is the **only** exception. It uses GNU inline asm,
is built with `-std=gnu89`, and is x86-only. Do not put anything else in
there and do not copy its style elsewhere.

Check yourself:

```sh
gcc -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter \
    -Werror=long-long -c src/yourfile.c -o /tmp/x.o
```

Zero warnings is the standard, not an aspiration.

---

## Never name a 64-bit integer type

This has broken real builds twice, both times on Solaris.

Under `-Xc` (strict C90) Sun Studio undefines `_LONGLONG_TYPE`, and
`<sys/types.h>` falls back to:

```c
typedef union { double _d; int32_t _l[2]; } longlong_t;
```

With `_FILE_OFFSET_BITS=64` on a 32-bit build, `off_t` *becomes that
union*. You cannot cast to it, assign to it, or pass anything to
`mmap()`.

So:

* Large file offsets are carried as `double`, typedef'd `gg_off`. Exact
  for integers below 2^53, far beyond any model that fits in a 32-bit
  address space.
* Where a system call needs an offset, compute it as a `long` page index
  and let the compiler widen implicitly. See `sys_map_file()` in
  `src/sys_posix.c`.
* Solaris builds are 64-bit (`-m64`), where `off_t` is already a plain
  `long`.

---

## Reading model data

The GGUF file is little-endian by definition, the blob is only
byte-aligned, and the host may be big-endian. Therefore:

**Never** cast a byte pointer to a wider type to read file data:

```c
float x = *(const float *) p;        /* WRONG: alignment + byte order */
```

**Always** go through the accessors:

```c
float  x = inf_rd_f32p(p);           /* little-endian f32   */
double d = inf_rd_f64p(p);           /* little-endian f64   */
float  h = rd_f16p(p);               /* little-endian fp16  */
inf_rd_f32v(src, dst, n);            /* a run of f32        */
```

On a little-endian host these compile to a plain load — the portability
costs nothing measurable (0.35% end-to-end, i.e. noise).

Getting this wrong does not crash. It produces a program that loads the
model, prints correct metadata, runs at full speed, and emits garbage.

---

## Memory

Use the checked wrappers, which abort on failure so callers do not need
to test every allocation:

```c
xmalloc  xcalloc  xrealloc  xstrdup
```

Growable strings are `strbuf`:

```c
strbuf b;
sb_init(&b);
sb_puts(&b, "hello");
sb_printf(&b, " %d", 42);
sb_free(&b);
```

Ownership rules that are easy to get wrong:

* `jj_list_add` and `jj_dict_set` **take ownership** of the value.
* `jj_dict_get` returns a **borrowed** pointer — `jj_clone` it to keep it.
* Tensor data is never copied; `gg_tensor.data` points into the mmap.

---

## Naming

| kind | style | example |
|---|---|---|
| public function | `module_verb` | `qwen35_decode`, `tok_encode` |
| static helper | lowercase | `page_index`, `trim_inplace` |
| type | lowercase `_t`-free | `qwen35_ctx`, `strbuf`, `jj_val` |
| macro / constant | uppercase | `INFER_BIG_ENDIAN`, `QK_K` |
| struct member | short, lowercase | `n_embd`, `arr_n` |

Prefixes: `qwen35_` engine, `tok_` tokeniser, `bk_` backend, `sb_`
strbuf, `jj_` Jinja values, `js_` JSON values, `gguf_`/`gg_` container,
`net_`/`sys_` platform, `agent_` shared prompt/tool layer, `inf_` misc.

---

## Comments

Explain **why**, not what. The what is in the code.

Good:

```c
/* -march=i486 rather than -march=geode: with geode, GCC emits CMOV in
 * ordinary code (xmalloc, gguf_open, main), which faults on a 486.
 * Measured cost of the baseline: 0.6%, in favour of the baseline. */
```

Not useful:

```c
/* set i to 0 */
i = 0;
```

When a decision was made because of a measurement, put the number in the
comment. When something was tried and rejected, say so — half the value
of this codebase's comments is stopping the next person from redoing a
failed experiment.

---

## Adding a command-line option

One row in the table in `src/opts.c`:

```c
{ O_MYOPT, "--my-opt", NULL, "<value>", 0, MODE_GEN, G_PROMPT,
  "what it does" },
```

and one `case` in the switch. Parsing, `--help` and the
"this option does nothing in that mode" check all follow automatically.

The rule the table encodes: **an option is general unless the mode
genuinely cannot use it.** Only `--host`, `--port`, `--api-key`,
`--alias` (nothing else opens a socket) and `-p/--prompt` (`serve` gets
its prompt from the request body) are restricted.

---

## Tests

Add to `tests/`, wire into `Makefile`'s `test` target, and make it end
with a line saying it passed.

A test earns its place by catching something that actually broke:

| test | catches |
|---|---|
| `t_align` | MMX constants losing 8-byte alignment — a hard fault on a Geode |
| `t_endian` | byte-order and type-width mistakes |
| `t_agent` | chat/run/serve drifting apart; tool-call formats |
| `t_backend` | a kernel disagreeing with `ref` |

---

## Before you commit

```sh
make clean && make all          # zero warnings
make test && ./build/t_align && ./build/t_agent && ./build/t_endian
make checkversion
```

Update `CHANGELOG.md` for anything user-visible. Update `COMPILE.md` if
you touched a build flag — and **run the command you wrote** before
claiming it produces the output you documented.
