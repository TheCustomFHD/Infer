# `src/opts.c` / `src/opts.h` — the option table

**~530 lines total.** Every command-line toggle `infer` understands, declared
exactly once.

Three things read that single declaration:

1. **parsing** — `opts_parse()`
2. **the help text** — `opts_usage()`, generated from the table
3. **applicability** — an option used in a mode that cannot act on it is
   reported as an error instead of being silently dropped

Because all three come from the same rows, they cannot drift apart. The
old arrangement — a hand-written `usage()` string, a separate `if/else`
chain, and each mode reading only the variables it cared about — allowed
exactly that drift, and it is what this file exists to prevent.

---

## Why it exists

Before this, `--system`, `--think` and `--mcp` were parsed for every
mode but only *read* by `chat`. Passing them to `run` or `serve` did
nothing at all, with no message. The help text described them under a
"chat options" heading, which documented the limitation without
justifying it — none of the three is inherently interactive.

The rule now:

> **An option is general unless the mode genuinely cannot use it.**

Only five are restricted, and each for a concrete reason:

| option | modes | why |
|---|---|---|
| `--host`, `--port`, `--api-key`, `--alias` | `serve` | nothing else opens a socket |
| `-p`, `--prompt` | `chat`, `run` | `serve` takes its prompt from the request body |

Everything else — `--system`, `--think`, `--raw`,
`--template`, `--mcp`, `--tool-rounds`, `--quiet-tools` and every
sampling knob — applies to `serve`, `chat` and `run` alike.

---

## The table

```c
typedef struct {
    int         id;       /* O_* enum                        */
    const char *lng;      /* --name                          */
    const char *shrt;     /* -x, or NULL                     */
    const char *arg;      /* argument name, NULL for a flag  */
    int         optarg;   /* 1: the argument may be omitted  */
    int         modes;    /* bit set: MODE_SERVE|MODE_CHAT|… */
    int         group;    /* which help section              */
    const char *help;
} opt_def;
```

`modes` is a bit set, so `MODE_GEN` (`serve|chat|run`) is the common
case and a restriction is visible at a glance:

```c
{ O_SYSTEM, "--system", NULL, "<text>", MODE_GEN,   G_PROMPT, "…" },
{ O_PORT,   "--port",   NULL, "<n>",    MODE_SERVE, G_SERVER, "…" },
```

Adding an option means adding one row and one `case` in the switch. The
help text and the validation follow automatically.

### Optional arguments (`--think`)

`--think` was originally a pair, `--think` and `--no-think`. Two flags
for one boolean means both can be given at once, and the result then
depends on argument order — an ambiguity with no correct answer.

They are now one option with an optional value: `--think` means on,
`--think off` means off. The parser consumes the following word only
when it actually reads as a value, so `--think --raw` sets thinking and
leaves `--raw` alone:

```c
if (i + 1 < argc && on_off(argv[i + 1]) >= 0) {
    val = argv[++i];
} else if (i + 1 < argc && argv[i + 1][0] != '-' && !find_opt(argv[i + 1])) {
    inf_log("'%s' expects on or off, not '%s'", d->lng, argv[i + 1]);
    return -1;
}
```

The third branch matters: without it `--think banana` reported "unknown
option 'banana'", which blames the wrong word.

---

## The applicability check

```c
if ((d->modes & o->mode) == 0) {
    inf_log("'%s' has no effect in %s mode (it applies to: %s)",
            d->lng, opts_mode_name(o->mode), mt);
    return -1;
}
```

```
$ infer run model.gguf --port 9090
'--port' has no effect in run mode (it applies to: serve)
```

This is the part that matters. A silently ignored flag is worse than a
rejected one: the command appears to work and quietly does something
other than what was asked.

Combinations that are legal but pointless warn rather than fail, since
the intent is still unambiguous:

```
$ infer run model.gguf --raw --template t.jinja
warning: --raw disables the chat template, so --template is unused
```

---

## `infer_opts`

One struct carries the parsed result to every mode. `server_config` and
`chat_run()` both take a `const infer_opts *`, so a new option reaches
them without changing any signature.

## C90 notes

`opts_usage()` prints in many small calls: C90 guarantees only 509
characters per string literal. Continuation lines in `help` are embedded
newlines padded to the 25-column description gutter.

---

## See also

- [main-c.md](main-c.md) — dispatch, and the `run` loop
- [agent-c.md](agent-c.md) — what the shared options actually do
- [../../USER-GUIDE.md](../../USER-GUIDE.md) — the options from a user's side
- [../ARCHITECTURE.md](../ARCHITECTURE.md) — where this sits
