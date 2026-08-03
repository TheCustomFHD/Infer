# `src/jinja.c`, `jinja_eval.c`, `jinja.h`, `jinja_priv.h` — template engine

**~1550 lines total.** A Jinja2 subset interpreter, enough to render real
GGUF chat templates byte-for-byte identically to Python's Jinja2.

| file | role |
|---|---|
| `jinja.h` | public API and the `jj_val` value type |
| `jinja.c` | values: constructors, clone, truthiness, `tostr`, `tojson` |
| `jinja_priv.h` | scope, macro and context types shared internally |
| `jinja_eval.c` | expression parser and statement interpreter |

## Design

Recursive-descent expression parser over a template walked **linearly**.
Blocks are handled by scanning forward for the matching end tag — no AST
is built. A `{% for %}` body is therefore re-scanned once per iteration.

That is deliberate: chat templates are a few kilobytes and loop over a
handful of messages, so the cost is irrelevant, and it keeps the memory
footprint tiny on the target.

## Supported

**Statements** `if` / `elif` / `else`, `for` (with `loop.index`,
`index0`, `first`, `last`, `length`, `revindex`, `previtem`, `nextitem`),
`set` (inline and block), `macro` with defaults and keyword arguments,
comments.

**Expressions** `and or not == != < > <= >= + - * / % ~ in`, `not in`,
ternary `if/else`, `namespace()`, `raise_exception()`.

**Filters** `trim tojson string length count first last list safe
default upper lower join int abs`.

**Tests** `is string / mapping / sequence / iterable / defined /
undefined / none / number / boolean / true / false`, each with `not`.

**Methods** `.split() .startswith() .endswith() .strip() .lstrip()
.rstrip() .items() .keys()`.

**Indexing** `obj.attr`, `obj['key']`, `seq[n]`, `seq[::-1]`, and general
slices `seq[a:b]` with negative and omitted bounds.

**Unpacking** `for k, v in d.items()`.

## Two bugs that silently corrupt prompts

Both were found by byte-comparing against Python Jinja2, not by
inspection. If you modify this engine, re-run that comparison.

**1. `{{-` must trim only literal template text.** It trims trailing
whitespace from the output so far — but the output of an *expression* is
not template text, and a `{% for %}` body is re-executed per iteration,
so a leading `{{-` could reach back and eat output written by the
*previous* iteration. That deleted the newline after every chat message.

Fixed with two markers in the context: `lit_start` (where the current
literal run began) and `trim_floor` (where this execution began, i.e.
the start of the current iteration). Trimming may not go below either.

**2. `|tojson` must sort object keys.** Jinja2 uses
`json.dumps(sort_keys=True)`. Without it the tools prompt differs
byte-for-byte from every other runtime.

## Verification

`tests/t_jinja.c` runs 25 expression/statement cases plus the real
template from a model file. Beyond that, both the built-in Qwen3.5
template **and** the 16 kB community template from
`froggeric/Qwen-Fixed-Chat-Templates` were rendered and `cmp`'d against
Python Jinja2 — plain chat, tool-enabled, thinking on and off. All
byte-identical.

## Not supported

`{% include %}`, `{% extends %}`, `{% block %}`, `{% filter %}`,
`{% call %}`, custom tests, `|selectattr`, `|map`, `|groupby`,
`{% raw %}`. None appear in any chat template examined. A template using
them will render the surrounding text and skip the unknown tag rather
than fail — check with `/prompt`.

## See also

- [chat-c.md](chat-c.md) · [../TEMPLATES.md](../TEMPLATES.md)
