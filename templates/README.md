# Chat templates

`infer` renders the Jinja chat template stored inside the GGUF by
default. `--template <file>` replaces it with one from disk:

```sh
infer chat model.gguf --template my-template.jinja
```

This directory ships **no templates**. Third-party templates are the
work of their authors and are theirs to distribute; download them
yourself from the source below.

## Recommended: the community-maintained Qwen templates

froggeric maintains corrected Qwen chat templates that fix several
issues in the ones shipped inside official GGUFs:

<https://huggingface.co/froggeric/Qwen-Fixed-Chat-Templates>

```sh
curl -L -o qwen-fixed.jinja \
  "https://huggingface.co/froggeric/Qwen-Fixed-Chat-Templates/resolve/main/chat_template.jinja"

infer chat model.gguf --template qwen-fixed.jinja
```

That file (v21.3, ~16 kB, 328 lines) is the largest template `infer`
has been tested against. Rendering it with `infer` and with Python's
`jinja2` produces **byte-identical** output for plain chat, for
tool-enabled prompts, and with thinking on and off — see
[../docs/TEMPLATES.md](../docs/TEMPLATES.md).

## Writing your own

`infer` implements a subset of Jinja2 sufficient for chat templates.
The supported syntax, the two whitespace-trimming rules that matter,
and how to verify a template against Python are documented in
[../docs/TEMPLATES.md](../docs/TEMPLATES.md).

Check what a template actually produces with the `/prompt` command
inside `infer chat`.
