# Chat templates

`infer` renders real Jinja2 chat templates — the one baked into the GGUF,
or any `.jinja` file you point it at.

---

## Using a custom template

```sh
infer chat  model.gguf --template mytemplate.jinja
infer serve model.gguf --template mytemplate.jinja
```

The file replaces `tokenizer.chat_template` from the GGUF entirely.
CRLF line endings are normalised to LF on load.

### Example: community-fixed Qwen templates

[froggeric/Qwen-Fixed-Chat-Templates](https://huggingface.co/froggeric/Qwen-Fixed-Chat-Templates)
maintains corrected Qwen templates — 16 kB, considerably more elaborate
than the built-in one.

```sh
curl -L -o qwen-fixed.jinja \
  https://huggingface.co/froggeric/Qwen-Fixed-Chat-Templates/resolve/main/chat_template.jinja

infer chat model.gguf --template qwen-fixed.jinja
```

This template is part of the test set: rendering it with `infer` and with
Python's Jinja2 produces **byte-identical** output, for plain chat and
for tool-enabled prompts, with thinking on and off.

---

## Variables `infer` provides

| variable | type | set by |
|---|---|---|
| `messages` | list of `{role, content}` | the conversation |
| `add_generation_prompt` | bool | always `true` when generating |
| `enable_thinking` | bool | `--think`, `/think on\|off`, or the API field |
| `tools` | list of OpenAI-shaped tool dicts | present only when MCP is connected |

Roles used: `system`, `user`, `assistant`, `tool`.

---

## Supported Jinja

**Statements** `if` / `elif` / `else`, `for`, `set` (inline and block),
`macro` with defaults and keyword arguments, comments, and `-`
whitespace control on every tag.

**Loop object** `loop.index`, `index0`, `first`, `last`, `length`,
`revindex`, `revindex0`, `previtem`, `nextitem`.

**Operators** `and or not == != < > <= >= + - * / % ~ in`, `not in`,
ternary `x if c else y`.

**Filters** `trim tojson string length count first last list safe
default upper lower join int abs`.

**Tests** `is string / mapping / sequence / iterable / defined /
undefined / none / number / boolean / true / false`, each negatable with
`is not`.

**Methods** `.split() .startswith() .endswith() .strip() .lstrip()
.rstrip() .items() .keys()`.

**Indexing and slicing** `obj.attr`, `obj['key']`, `seq[n]`, `seq[::-1]`,
`seq[a:b]` with negative and omitted bounds.

**Other** `namespace(...)` with attribute assignment,
`raise_exception('...')`, `for k, v in d.items()`.

---

## Not supported

`{% include %}`, `{% extends %}`, `{% block %}`, `{% filter %}`,
`{% call %}`, `{% raw %}`, custom tests, `|selectattr`, `|map`,
`|groupby`, `|batch`.

None of these appear in any chat template examined. An unknown tag is
skipped rather than fatal, so a template using one renders the
surrounding text and quietly omits that construct — **check with
`/prompt`** if output looks wrong.

---

## Debugging a template

```
> /prompt
---8<---
<|im_start|>system
You are terse.<|im_end|>
<|im_start|>user
Hi<|im_end|>
<|im_start|>assistant
<think>

</think>

---8<---
```

`/prompt` shows exactly what the model is fed. If it looks wrong, the
template is wrong — not the engine.

To be certain, compare against Python:

```python
from jinja2 import Template
def raise_exception(m): raise RuntimeError(m)
t = Template(open('mytemplate.jinja').read())
t.globals['raise_exception'] = raise_exception
print(t.render(messages=[{"role":"user","content":"Hi"}],
               add_generation_prompt=True, enable_thinking=False))
```

Then diff against `/prompt` output. Any difference is a bug worth
reporting.

---

## Two details worth knowing

**`{{-` trims literal template text only** — never the output of an
expression, and never past the start of the current loop iteration.
Getting this wrong (as an earlier version did) silently deletes the
newline after every message.

**`|tojson` sorts object keys**, matching Jinja2's
`json.dumps(sort_keys=True)`. This matters for tool schemas: without it
the prompt differs byte-for-byte from every other runtime.

---

## See also

- [files/jinja-c.md](files/jinja-c.md) — how the engine works
- [../CHAT.md](../CHAT.md) · [../USER-GUIDE.md](../USER-GUIDE.md)
