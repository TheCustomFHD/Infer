/* t_jinja.c -- render the chat template from a GGUF file and check the
 * result against what the model expects.
 *
 *   ./build/t_jinja model.gguf
 */

#include "../src/infer.h"
#include "../src/jinja.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

static void show(const char *label, const char *s) {
    printf("--- %s ---\n", label);
    fputs(s, stdout);
    printf("\n[end]\n\n");
}

static void check(const char *what, int cond) {
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

/* small standalone template tests */
static void unit(const char *tmpl, const char *want) {
    jj_val *ctx = jj_dict();
    strbuf out;
    char err[256];
    int rc;

    jj_dict_set(ctx, "name", jj_str("world"));
    jj_dict_set(ctx, "n", jj_num(3));
    jj_dict_set(ctx, "yes", jj_bool(1));
    {
        jj_val *l = jj_list();
        jj_list_add(l, jj_str("a"));
        jj_list_add(l, jj_str("b"));
        jj_list_add(l, jj_str("c"));
        jj_dict_set(ctx, "items", l);
    }
    {
        jj_val *d = jj_dict();
        jj_dict_set(d, "role", jj_str("user"));
        jj_dict_set(d, "content", jj_str("  hi  "));
        jj_dict_set(ctx, "m", d);
    }

    rc = jj_render(tmpl, ctx, &out, err, sizeof(err));
    if (rc != 0) {
        printf("  %-40s ERROR: %s\n", tmpl, err);
        fails++;
    } else {
        int ok = strcmp(out.data, want) == 0;
        printf("  %-40s -> %-18s %s\n", tmpl,
               out.data, ok ? "ok" : "FAIL");
        if (!ok) { printf("      wanted: [%s]\n", want); fails++; }
    }
    sb_free(&out);
    jj_free(ctx);
}

int main(int argc, char **argv) {
    gguf_file g;
    const char *tmpl;
    jj_val *ctx, *msgs, *m;
    strbuf out;
    char err[256];

    printf("=== expression / statement unit tests ===\n");
    unit("{{ name }}", "world");
    unit("{{ 1 + 2 }}", "3");
    unit("{{ 'a' ~ 'b' }}", "ab");
    unit("{{ n * 1 if false else 9 }}", "9");
    unit("{%- if yes -%}Y{%- else -%}N{%- endif -%}", "Y");
    unit("{%- if not yes -%}Y{%- else -%}N{%- endif -%}", "N");
    unit("{%- for i in items -%}{{ i }}{%- endfor -%}", "abc");
    unit("{%- for i in items -%}{{ loop.index0 }}{%- endfor -%}", "012");
    unit("{%- for i in items[::-1] -%}{{ i }}{%- endfor -%}", "cba");
    unit("{%- set x = 5 -%}{{ x }}", "5");
    unit("{{ m.role }}", "user");
    unit("{{ m['role'] }}", "user");
    unit("{{ m.content | trim }}", "hi");
    unit("{{ items | length }}", "3");
    unit("{{ 'x' in 'axb' }}", "True");
    unit("{{ 'q' in items }}", "False");
    unit("{{ m is mapping }}", "True");
    unit("{{ name is string }}", "True");
    unit("{{ missing is defined }}", "False");
    unit("{%- set ns = namespace(v=1) -%}{%- set ns.v = 7 -%}{{ ns.v }}", "7");
    unit("{%- macro f(a) -%}[{{ a }}]{%- endmacro -%}{{ f('z') }}", "[z]");
    unit("{{ 'a,b' .split(',') | length }}", "2");
    unit("{{ 'hello'.startswith('he') }}", "True");
    /* |tojson sorts keys, matching Jinja2/json.dumps(sort_keys=True) */
    unit("{{ m | tojson }}", "{\"content\": \"  hi  \", \"role\": \"user\"}");

    if (argc < 2) {
        printf("\n(no model given; skipping the real chat template)\n");
        return fails ? 1 : 0;
    }

    if (gguf_open(&g, argv[1])) { printf("cannot open model\n"); return 1; }
    tmpl = gguf_str(&g, "tokenizer.chat_template");
    if (!tmpl) { printf("model has no chat template\n"); return 1; }
    printf("\n=== real chat template (%lu bytes) ===\n",
           (unsigned long) strlen(tmpl));

    /* two-turn conversation with a system prompt */
    ctx = jj_dict();
    msgs = jj_list();

    m = jj_dict();
    jj_dict_set(m, "role", jj_str("system"));
    jj_dict_set(m, "content", jj_str("You are terse."));
    jj_list_add(msgs, m);

    m = jj_dict();
    jj_dict_set(m, "role", jj_str("user"));
    jj_dict_set(m, "content", jj_str("Hi"));
    jj_list_add(msgs, m);

    m = jj_dict();
    jj_dict_set(m, "role", jj_str("assistant"));
    jj_dict_set(m, "content", jj_str("Hello."));
    jj_list_add(msgs, m);

    m = jj_dict();
    jj_dict_set(m, "role", jj_str("user"));
    jj_dict_set(m, "content", jj_str("Bye"));
    jj_list_add(msgs, m);

    jj_dict_set(ctx, "messages", msgs);
    jj_dict_set(ctx, "add_generation_prompt", jj_bool(1));
    jj_dict_set(ctx, "enable_thinking", jj_bool(0));

    if (jj_render(tmpl, ctx, &out, err, sizeof(err)) != 0) {
        printf("RENDER ERROR: %s\n", err);
        return 1;
    }
    show("rendered", out.data);

    check("starts with <|im_start|>system",
          strncmp(out.data, "<|im_start|>system\n", 19) == 0);
    check("system content present", strstr(out.data, "You are terse.") != NULL);
    check("user turn 1 present",
          strstr(out.data, "<|im_start|>user\nHi<|im_end|>") != NULL);
    check("assistant turn present",
          strstr(out.data, "<|im_start|>assistant\nHello.<|im_end|>") != NULL);
    check("user turn 2 present",
          strstr(out.data, "<|im_start|>user\nBye<|im_end|>") != NULL);
    check("ends with assistant generation prompt",
          strstr(out.data, "<|im_start|>assistant\n") != NULL);
    check("thinking disabled -> empty think block",
          strstr(out.data, "<think>\n\n</think>") != NULL);
    check("no unrendered {{", strstr(out.data, "{{") == NULL);
    check("no unrendered {%", strstr(out.data, "{%") == NULL);

    sb_free(&out);
    jj_free(ctx);
    gguf_close(&g);

    printf("\n%s\n", fails ? "FAILURES" : "all jinja tests passed");
    return fails ? 1 : 0;
}
