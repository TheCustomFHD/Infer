# templates

Example Jinja chat templates for `--template`.

| file | source |
|---|---|
| `qwen-fixed-v21.3.jinja` | [froggeric/Qwen-Fixed-Chat-Templates](https://huggingface.co/froggeric/Qwen-Fixed-Chat-Templates) |

```sh
infer chat model.gguf --template templates/qwen-fixed-v21.3.jinja
```

This template is part of the verification set: rendering it with `infer`
and with Python's Jinja2 produces byte-identical output, for plain chat
and tool-enabled prompts, thinking on and off.

See [../docs/TEMPLATES.md](../docs/TEMPLATES.md).
