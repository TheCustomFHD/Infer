# `src/main.c` — the launcher

**~400 lines.** Dispatch and the one-shot `run` loop. Contains no
inference and no networking logic; it loads a model and hands over to one
of four sub-commands.

Argument parsing now lives in [`opts.c`](opts-c.md), so that every mode
shares one definition of every toggle, and prompt/tool handling lives in
[`agent.c`](agent-c.md). What remains here is dispatch.

Keeping this separate is why the engine and the server can be reused or
replaced independently.

---

## What it does

```
infer serve <model.gguf>   ->  server_run()      (server.c)
infer chat  <model.gguf>   ->  chat_run()        (chat.c)
infer run   <model.gguf>   ->  cmd_run()         (local, in this file)
                              via agent.c, so run supports --system,
                              --think, --template, --mcp and --raw
infer info  <model.gguf>   ->  cmd_info()        (local, in this file)
```

`infer model.gguf` with no sub-command is treated as `serve`.
`infer --backend list` works without a model, so you can probe a CPU
before copying 500 MB onto it.

## Flow

1. `opts_parse()` fills one `infer_opts`, validating each option against
   the mode it was used in.
2. If `--template` was given, `load_text_file()` reads it; the text
   overrides the GGUF template for whichever mode runs.
3. `qwen35_load()` maps the model.
4. If `--log-perf`/`--log-file`/a profiling build, open the log and emit
   the banner.
5. Dispatch.
6. `prof_report()`, free everything, return.

## Notable details

**Help text is printed in several `printf` calls.** C90 only guarantees
509 characters per string literal, and some strict compilers enforce it.
This was a real warning (`-Woverlength-strings`) before it was split.

**`cmd_run()` is deliberately dumb.** It wraps the prompt in a minimal
ChatML frame unless `--raw`, ingests, and greedily streams. It does *not*
use the Jinja engine, so it stays useful for testing a model whose
template is broken or missing.

**`cmd_info()`** dumps every GGUF key/value, abbreviating the huge
tokeniser arrays to `<N entries>` rather than printing 248,320 tokens.

## Options it owns

| flag | consumed by |
|---|---|
| `--host --port --api-key --alias` | `serve` |
| `--system --think --mcp` | `chat` |
| `-p --raw` | `run` |
| `--template` | `chat`, `serve` |
| `-c -n -t --top-p --top-k --repeat --seed` | all generation |
| `--backend --log-perf --log-file -v` | global |

## Gotchas

* The `NEXT()` macro advances `i` as a side effect. It is only ever used
  once per branch, guarded by `has_next`.
* `n_ctx` is clamped to the model's trained context after loading, so
  `-c 999999` silently becomes the real maximum rather than allocating a
  hopeless KV cache.
* `--seed 0` means "derive from the clock", matching the documented
  default.

## See also

- [server-c.md](server-c.md) · [chat-c.md](chat-c.md) ·
  [qwen35-c.md](qwen35-c.md)
- [../USER-GUIDE.md](../../USER-GUIDE.md) for what these flags do in practice
