/* agent.c -- prompt construction and tool calling, shared by all modes.
 *
 * Lifted almost verbatim out of chat.c so that serve and run can use
 * the same code. The tool-call parser in particular had been tuned
 * against real Qwen3.5 output (it tolerates missing <tool_call> tags and
 * duplicated parameters); duplicating that anywhere else would have
 * meant two subtly different parsers.
 *
 * ANSI C (C89).
 */

#include "agent.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Messages                                                            */
/* ------------------------------------------------------------------ */

jj_val *agent_messages(void) {
    return jj_list();
}

void agent_add_msg(jj_val *messages, const char *role, const char *content) {
    jj_val *m = jj_dict();
    jj_dict_set(m, "role", jj_str(role));
    jj_dict_set(m, "content", jj_str(content ? content : ""));
    jj_list_add(messages, m);
}

int agent_has_system(const jj_val *messages) {
    jj_val *first, *role;
    if (!messages || messages->n == 0) return 0;
    first = messages->items[0];
    role = jj_dict_get(first, "role");
    if (!role || role->type != JJ_STR) return 0;
    return strcmp(role->str, "system") == 0 ||
           strcmp(role->str, "developer") == 0;
}

void agent_set_system(jj_val **pmessages, const char *text) {
    jj_val *messages = *pmessages;
    jj_val *nl, *m;
    int i;

    if (agent_has_system(messages)) {
        jj_dict_set(messages->items[0], "content", jj_str(text));
        return;
    }

    nl = jj_list();
    m = jj_dict();
    jj_dict_set(m, "role", jj_str("system"));
    jj_dict_set(m, "content", jj_str(text));
    jj_list_add(nl, m);
    for (i = 0; i < messages->n; i++) {
        jj_list_add(nl, jj_clone(messages->items[i]));
    }
    jj_free(messages);
    *pmessages = nl;
}

jj_val *agent_js_to_jj(const js_val *v) {
    jj_val *r;
    int i;
    if (!v) return jj_none();
    switch (v->type) {
        case JS_NULL: return jj_none();
        case JS_BOOL: return jj_bool(v->num != 0);
        case JS_NUM:  return jj_num(v->num);
        case JS_STR:  return jj_str(v->str);
        case JS_ARR:
            r = jj_list();
            for (i = 0; i < v->n; i++) jj_list_add(r, agent_js_to_jj(v->items[i]));
            return r;
        default:
            r = jj_dict();
            for (i = 0; i < v->n; i++)
                jj_dict_set(r, v->keys[i], agent_js_to_jj(v->items[i]));
            return r;
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

/* --raw: no template at all. Message contents are joined with blank
 * lines, which is what a base model expects and what benchmark
 * harnesses that refuse Jinja (llama-bench and friends) want to see.
 * Roles are dropped deliberately -- inventing "User:" prefixes here
 * would be a template by another name. */
static void render_raw(const jj_val *messages, strbuf *out) {
    int i;
    sb_init(out);
    for (i = 0; i < messages->n; i++) {
        jj_val *c = jj_dict_get(messages->items[i], "content");
        if (!c || c->type != JJ_STR || !c->str[0]) continue;
        if (out->len) sb_puts(out, "\n\n");
        sb_puts(out, c->str);
    }
}

int agent_render(const agent_config *cfg, const jj_val *messages,
                 strbuf *out, char *errbuf, size_t errlen) {
    jj_val *ctx;
    int rc;

    if (cfg->raw) {
        render_raw(messages, out);
        return 0;
    }

    if (!cfg->tmpl) {
        if (errbuf && errlen) {
            strncpy(errbuf, "model has no chat template; pass --template "
                            "or use --raw", errlen - 1);
            errbuf[errlen - 1] = '\0';
        }
        return -1;
    }

    ctx = jj_dict();
    jj_dict_set(ctx, "messages", jj_clone((jj_val *) messages));
    jj_dict_set(ctx, "add_generation_prompt", jj_bool(1));
    jj_dict_set(ctx, "enable_thinking", jj_bool(cfg->thinking));
    if (cfg->mcp && mcp_tool_count(cfg->mcp) > 0) {
        jj_dict_set(ctx, "tools", mcp_tools_as_jinja(cfg->mcp));
    }

    rc = jj_render(cfg->tmpl, ctx, out, errbuf, errlen);
    jj_free(ctx);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Tool-call extraction                                                */
/*                                                                     */
/* Qwen3.5 emits:                                                      */
/*   <tool_call>                                                       */
/*   <function=NAME>                                                   */
/*   <parameter=KEY>                                                   */
/*   value                                                             */
/*   </parameter>                                                      */
/*   </function>                                                       */
/*   </tool_call>                                                      */
/* ------------------------------------------------------------------ */

static char *between(const char *s, const char *a, const char *b,
                     const char **after) {
    const char *p = strstr(s, a);
    const char *q;
    char *r;
    size_t n;
    if (!p) return NULL;
    p += strlen(a);
    q = strstr(p, b);
    if (!q) return NULL;
    n = (size_t) (q - p);
    r = (char *) xmalloc(n + 1);
    memcpy(r, p, n);
    r[n] = '\0';
    if (after) *after = q + strlen(b);
    return r;
}

static void trim_inplace(char *s) {
    size_t n = strlen(s), a = 0;
    while (n > a && isspace((unsigned char) s[n - 1])) n--;
    s[n] = '\0';
    while (s[a] && isspace((unsigned char) s[a])) a++;
    if (a) memmove(s, s + a, strlen(s + a) + 1);
}

/* Serialise a parsed JSON value back to compact JSON. Used to hand a
 * tool call's "arguments" object to mcp_call unchanged. */
static void js_dump(const js_val *v, strbuf *out) {
    int i;
    if (!v) { sb_puts(out, "null"); return; }
    switch (v->type) {
        case JS_NULL: sb_puts(out, "null"); break;
        case JS_BOOL: sb_puts(out, v->num != 0 ? "true" : "false"); break;
        case JS_NUM: {
            char nb[64];
            if (v->num == (double) (long) v->num) sprintf(nb, "%ld", (long) v->num);
            else sprintf(nb, "%g", v->num);
            sb_puts(out, nb);
            break;
        }
        case JS_STR:
            sb_puts(out, "\"");
            sb_json_escape(out, v->str, strlen(v->str));
            sb_puts(out, "\"");
            break;
        case JS_ARR:
            sb_puts(out, "[");
            for (i = 0; i < v->n; i++) {
                if (i) sb_puts(out, ",");
                js_dump(v->items[i], out);
            }
            sb_puts(out, "]");
            break;
        default:
            sb_puts(out, "{");
            for (i = 0; i < v->n; i++) {
                if (i) sb_puts(out, ",");
                sb_puts(out, "\"");
                sb_json_escape(out, v->keys[i], strlen(v->keys[i]));
                sb_puts(out, "\":");
                js_dump(v->items[i], out);
            }
            sb_puts(out, "}");
            break;
    }
}

/* The other shape Qwen3.5 emits, selected by the template's
 * tool_call_format='json':
 *
 *   <tool_call>
 *   {"name": "f", "arguments": {"x": 1}}
 *   </tool_call>
 *
 * Also accepts the OpenAI nesting, {"function": {"name": ..., ...}}.
 * Missing this form is not cosmetic: the call still parsed as XML-with-
 * no-parameters, so the tool ran with an empty argument object. */
static int parse_json_tool_call(const char *blk, char **name,
                                strbuf *args_json) {
    const char *open = strchr(blk, '{');
    js_val *j, *fn, *nm, *args;

    if (!open) return 0;
    j = js_parse(open);
    if (!j || j->type != JS_OBJ) { js_free(j); return 0; }

    fn = js_get(j, "function");
    if (!fn || fn->type != JS_OBJ) fn = j;

    nm = js_get(fn, "name");
    if (!nm || nm->type != JS_STR) { js_free(j); return 0; }

    *name = xstrdup(nm->str);
    sb_init(args_json);

    args = js_get(fn, "arguments");
    if (!args) args = js_get(fn, "parameters");
    if (args && args->type == JS_STR) {
        /* some runtimes double-encode the arguments object */
        js_val *inner = js_parse(args->str);
        if (inner) { js_dump(inner, args_json); js_free(inner); }
        else sb_puts(args_json, "{}");
    } else if (args) {
        js_dump(args, args_json);
    } else {
        sb_puts(args_json, "{}");
    }

    js_free(j);
    return 1;
}

int agent_parse_tool_call(const char *text, char **name, strbuf *args_json) {
    const char *blk_after = NULL;
    char *blk;
    const char *p;
    int first = 1;
    int owned = 1;

    if (!text) return 0;
    blk = between(text, "<tool_call>", "</tool_call>", &blk_after);

    /* Small models frequently emit the <function=...> block without the
     * surrounding <tool_call> tags, or leave the closing tag off when
     * generation is truncated. Accept those too -- the function tag is
     * the part that actually identifies a call. */
    if (!blk) {
        const char *f = strstr(text, "<function=");
        if (!f) {
            /* No XML anywhere. Try the JSON form, tags or not. */
            const char *tc = strstr(text, "<tool_call>");
            return parse_json_tool_call(tc ? tc : text, name, args_json);
        }
        blk = (char *) f;
        owned = 0;
    }

    /* A <tool_call> block that carries JSON rather than <function=..>. */
    if (owned && !strstr(blk, "<function=")) {
        int rc = parse_json_tool_call(blk, name, args_json);
        free(blk);
        return rc;
    }

    *name = between(blk, "<function=", ">", &p);
    if (!*name) { if (owned) free(blk); return 0; }
    trim_inplace(*name);

    sb_init(args_json);
    sb_puts(args_json, "{");

    for (;;) {
        const char *after;
        char *key = between(p, "<parameter=", ">", &after);
        char *val;
        if (!key) break;
        trim_inplace(key);
        val = between(after, "", "</parameter>", &p);
        if (!val) { free(key); break; }
        trim_inplace(val);

        /* A truncated or self-correcting model can emit the same
         * parameter twice; JSON objects must not have duplicate keys, so
         * later values replace earlier ones. */
        {
            strbuf probe;
            sb_init(&probe);
            sb_puts(&probe, "\"");
            sb_json_escape(&probe, key, strlen(key));
            sb_puts(&probe, "\":");
            if (strstr(args_json->data, probe.data)) {
                char *cut = strstr(args_json->data, probe.data);
                size_t at = (size_t) (cut - args_json->data);
                if (at > 0 && args_json->data[at-1] == ',') at--;
                args_json->len = at;
                args_json->data[at] = '\0';
                if (at == 1) first = 1;
            }
            sb_free(&probe);
        }
        if (!first) sb_puts(args_json, ",");
        first = 0;
        sb_puts(args_json, "\"");
        sb_json_escape(args_json, key, strlen(key));
        sb_puts(args_json, "\":");
        /* keep JSON literals literal, quote everything else */
        if ((val[0] == '{' || val[0] == '[' ||
             strcmp(val, "true") == 0 || strcmp(val, "false") == 0 ||
             strcmp(val, "null") == 0) ) {
            sb_puts(args_json, val);
        } else {
            char *end;
            double d = strtod(val, &end);
            if (val[0] && *end == '\0') {
                char nb[64];
                if (d == (double) (long) d) sprintf(nb, "%ld", (long) d);
                else sprintf(nb, "%g", d);
                sb_puts(args_json, nb);
            } else {
                sb_puts(args_json, "\"");
                sb_json_escape(args_json, val, strlen(val));
                sb_puts(args_json, "\"");
            }
        }
        free(key);
        free(val);
    }

    sb_puts(args_json, "}");
    if (owned) free(blk);
    (void) blk_after;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Invocation                                                          */
/* ------------------------------------------------------------------ */

int agent_run_tool(const agent_config *cfg, const char *name,
                   const char *args_json, strbuf *out) {
    int rc;

    if (!cfg->mcp) {
        sb_init(out);
        sb_puts(out, "error: no MCP server is configured");
        return -1;
    }
    if (!mcp_has_tool(cfg->mcp, name)) {
        sb_init(out);
        sb_printf(out, "error: no such tool '%.60s'", name);
        return -1;
    }
    rc = mcp_call(cfg->mcp, name, args_json, out);
    return rc;
}

mcp_client *agent_connect_mcp(const char *url, int announce) {
    char err[256];
    mcp_client *m;

    if (!url || !url[0]) return NULL;
    if (announce) printf("connecting to MCP server %s ...\n", url);

    m = mcp_connect(url, err, sizeof(err));
    if (!m) {
        if (announce) {
            printf("  failed: %s\n", err);
            printf("  continuing without tools\n");
        } else {
            inf_log("MCP: %s -- continuing without tools", err);
        }
        return NULL;
    }

    if (announce) {
        int i;
        printf("  %s: %d tool%s\n", mcp_server_name(m), mcp_tool_count(m),
               mcp_tool_count(m) == 1 ? "" : "s");
        for (i = 0; i < mcp_tool_count(m); i++) {
            printf("    %-24s %.60s\n", mcp_tool_name(m, i),
                   mcp_tool_desc(m, i));
        }
    } else {
        inf_log("MCP %s: %d tool(s) from %s", url, mcp_tool_count(m),
                mcp_server_name(m));
    }
    return m;
}
