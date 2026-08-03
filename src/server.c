/* server.c -- HTTP/1.1 server exposing an OpenAI-compatible API.
 *
 * Endpoints:
 *   GET  /health                  -> {"status":"ok"}
 *   GET  /v1/models               -> model list
 *   POST /v1/chat/completions     -> chat, optional SSE streaming
 *   POST /v1/completions          -> raw text completion, optional SSE
 *
 * The prompt for chat requests is rendered through agent.c, the same
 * code path `chat` and `run` use, so all three modes produce identical
 * prompts from identical inputs.
 *
 * Command-line defaults and per-request overrides compose in the usual
 * way: --system, --think and --raw set the default for every request,
 * and anything the request states explicitly wins. --mcp makes the
 * server run the tool loop itself, unless the request carries its own
 * "tools" array, in which case the client is driving and gets the tool
 * calls back verbatim.
 *
 * Requests are handled one at a time: the engine holds a single
 * recurrent state, and on the target hardware concurrency would only
 * make every client slower.
 *
 * All socket access goes through net.h, so this file is identical on
 * Linux and on Windows XP.
 *
 * ANSI C (C89).
 */

#include "server.h"
#include "net.h"
#include "json.h"
#include "prof.h"
#include "jinja.h"
#include "agent.h"
#include "mcp.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REQ_MAX      (4 * 1024 * 1024)   /* max request body */
#define READ_TIMEOUT 30000               /* ms */
#define MAX_TOKENS_IN 65536

static volatile int g_stop = 0;

void server_request_stop(void) {
    g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* HTTP request parsing                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char   method[8];
    char   path[256];
    char   auth[256];
    char  *body;
    size_t body_len;
} http_req;

static int hdr_int(const char *hdrs, const char *name) {
    const char *p = hdrs;
    size_t nlen = strlen(name);

    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) break;
        /* case-insensitive prefix compare */
        {
            size_t i;
            int match = 1;
            for (i = 0; i < nlen; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
                if (a != b) { match = 0; break; }
            }
            if (match && p[nlen] == ':') {
                return atoi(p + nlen + 1);
            }
        }
        p = eol + 2;
    }
    return -1;
}

static void hdr_str(const char *hdrs, const char *name, char *out, size_t olen) {
    const char *p = hdrs;
    size_t nlen = strlen(name);

    out[0] = '\0';
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t i;
        int match = 1;
        if (!eol) break;
        for (i = 0; i < nlen; i++) {
            char a = p[i], b = name[i];
            if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
            if (a != b) { match = 0; break; }
        }
        if (match && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            size_t n;
            while (*v == ' ' || *v == '\t') v++;
            n = (size_t) (eol - v);
            if (n >= olen) n = olen - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return;
        }
        p = eol + 2;
    }
}

/* Read a full request. Returns 0 on success. */
static int read_request(net_conn *c, http_req *r) {
    strbuf buf;
    const char *hdr_end;
    size_t hdr_len;
    int content_len;
    int rc = -1;

    memset(r, 0, sizeof(*r));
    sb_init(&buf);

    /* headers */
    for (;;) {
        char chunk[2048];
        int n;

        hdr_end = buf.len ? strstr(buf.data, "\r\n\r\n") : NULL;
        if (hdr_end) break;

        if (buf.len > 64 * 1024) goto out;   /* header flood */

        n = net_read(c, chunk, sizeof(chunk), READ_TIMEOUT);
        if (n <= 0) goto out;
        sb_add(&buf, chunk, (size_t) n);
    }

    hdr_len = (size_t) (hdr_end - buf.data) + 4;

    /* request line */
    {
        const char *sp1 = strchr(buf.data, ' ');
        const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        size_t n;
        if (!sp1 || !sp2) goto out;

        n = (size_t) (sp1 - buf.data);
        if (n >= sizeof(r->method)) n = sizeof(r->method) - 1;
        memcpy(r->method, buf.data, n);
        r->method[n] = '\0';

        n = (size_t) (sp2 - sp1 - 1);
        if (n >= sizeof(r->path)) n = sizeof(r->path) - 1;
        memcpy(r->path, sp1 + 1, n);
        r->path[n] = '\0';
    }

    hdr_str(buf.data, "Authorization", r->auth, sizeof(r->auth));

    content_len = hdr_int(buf.data, "Content-Length");
    if (content_len < 0) content_len = 0;
    if (content_len > REQ_MAX) goto out;

    /* body */
    {
        size_t have = buf.len - hdr_len;
        r->body = (char *) xmalloc((size_t) content_len + 1);
        if (have > (size_t) content_len) have = (size_t) content_len;
        memcpy(r->body, buf.data + hdr_len, have);

        while (have < (size_t) content_len) {
            int n = net_read(c, r->body + have,
                             (size_t) content_len - have, READ_TIMEOUT);
            if (n <= 0) { free(r->body); r->body = NULL; goto out; }
            have += (size_t) n;
        }
        r->body[content_len] = '\0';
        r->body_len = (size_t) content_len;
    }

    rc = 0;

out:
    sb_free(&buf);
    return rc;
}

static void free_request(http_req *r) {
    if (r->body) free(r->body);
    r->body = NULL;
}

/* ------------------------------------------------------------------ */
/* Responses                                                          */
/* ------------------------------------------------------------------ */

static const char *CORS =
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";

static void send_response(net_conn *c, int code, const char *status,
                          const char *ctype, const char *body) {
    strbuf out;
    sb_init(&out);
    sb_printf(&out, "HTTP/1.1 %d %s\r\n", code, status);
    sb_printf(&out, "Content-Type: %s\r\n", ctype);
    sb_printf(&out, "Content-Length: %lu\r\n",
              (unsigned long) strlen(body));
    sb_puts(&out, CORS);
    sb_puts(&out, "Connection: close\r\n\r\n");
    sb_puts(&out, body);
    net_write_all(c, out.data, out.len);
    sb_free(&out);
}

static void send_error(net_conn *c, int code, const char *status,
                       const char *type, const char *msg) {
    strbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"error\":{\"message\":\"");
    sb_json_escape(&b, msg, strlen(msg));
    sb_printf(&b, "\",\"type\":\"%s\",\"param\":null,\"code\":null}}", type);
    send_response(c, code, status, "application/json", b.data);
    sb_free(&b);
}

static int send_sse_head(net_conn *c) {
    strbuf out;
    int rc;
    sb_init(&out);
    sb_puts(&out, "HTTP/1.1 200 OK\r\n");
    sb_puts(&out, "Content-Type: text/event-stream\r\n");
    sb_puts(&out, "Cache-Control: no-cache\r\n");
    sb_puts(&out, CORS);
    sb_puts(&out, "Connection: close\r\n\r\n");
    rc = net_write_all(c, out.data, out.len);
    sb_free(&out);
    return rc;
}

static int send_sse_chunk(net_conn *c, const char *json) {
    strbuf out;
    int rc;
    sb_init(&out);
    sb_puts(&out, "data: ");
    sb_puts(&out, json);
    sb_puts(&out, "\n\n");
    rc = net_write_all(c, out.data, out.len);
    sb_free(&out);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Prompt construction                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    qwen35_model  *model;
    qwen35_ctx    *ctx;
    tokenizer     *tok;
    const server_config *cfg;
    const infer_opts *o;
    mcp_client    *mcp;      /* from --mcp, or NULL */
} engine;

/* Convert the OpenAI `messages` array into the jj_val form the chat
 * template expects.
 *
 * Every field is carried across, not just role and content: templates
 * read message.tool_calls, message.reasoning_content and structured
 * content arrays, and dropping them (as this did before) silently
 * degraded any conversation that used tools. */
static jj_val *messages_to_jinja(js_val *messages) {
    jj_val *l = jj_list();
    int i;

    for (i = 0; i < messages->n; i++) {
        js_val *msg = messages->items[i];
        jj_val *m = agent_js_to_jj(msg);
        if (m->type != JJ_DICT) {          /* malformed entry: skip */
            jj_free(m);
            continue;
        }
        if (!jj_dict_get(m, "role"))    jj_dict_set(m, "role", jj_str("user"));
        if (!jj_dict_get(m, "content")) jj_dict_set(m, "content", jj_str(""));
        jj_list_add(l, m);
    }
    return l;
}

/* Concatenate message contents for --raw. Mirrors agent.c's raw path,
 * but works on the JSON directly so structured content still flattens. */
static void raw_from_messages(js_val *messages, strbuf *out) {
    int i;
    sb_init(out);
    for (i = 0; i < messages->n; i++) {
        js_val *content = js_get(messages->items[i], "content");
        size_t before = out->len;
        if (out->len) sb_puts(out, "\n\n");
        if (content && content->type == JS_STR) {
            sb_puts(out, content->str);
        } else if (content && content->type == JS_ARR) {
            int k;
            for (k = 0; k < content->n; k++) {
                const char *t = js_str(content->items[k], "text", NULL);
                if (t) sb_puts(out, t);
            }
        }
        if (out->len == before + 2) {      /* nothing appended: undo */
            out->len = before;
            out->data[before] = '\0';
        }
    }
}

/* Build the jj message list for a chat request, applying the --system
 * default when the client sent no system turn of its own. */
static jj_val *chat_messages(engine *e, js_val *messages) {
    jj_val *msgs = messages_to_jinja(messages);
    const infer_opts *o = e->o;

    if (o->system && o->system[0] && !agent_has_system(msgs)) {
        agent_set_system(&msgs, o->system);
    }
    return msgs;
}

/* Render `msgs` for this request. Returns 0 on success.
 *
 * Precedence, applied here so it is visible in one place:
 *   raw      = request "raw" if present, else --raw
 *   thinking = request "enable_thinking"/"think" if present, else --think
 *   tools    = the request's own array if it sent one, else --mcp's
 */
static int render_chat(engine *e, js_val *req, const jj_val *msgs,
                       strbuf *out, char *err, size_t errlen) {
    const infer_opts *o = e->o;
    js_val *req_tools = js_get(req, "tools");
    agent_config ac;
    int client_tools = (req_tools && req_tools->type == JS_ARR);

    ac.tmpl        = e->cfg->chat_template;
    ac.raw         = 0;
    ac.thinking    = js_bool(req, "enable_thinking",
                             js_bool(req, "think", o->thinking));
    ac.tool_rounds = o->tool_rounds;
    ac.show_tools  = 0;
    /* When the client supplies tools it is driving the loop, so the
     * server's own MCP tools must not be advertised as well. */
    ac.mcp         = client_tools ? NULL : e->mcp;

    if (client_tools && ac.tmpl) {
        jj_val *ctx = jj_dict();
        int rc;
        jj_dict_set(ctx, "messages", jj_clone((jj_val *) msgs));
        jj_dict_set(ctx, "add_generation_prompt", jj_bool(1));
        jj_dict_set(ctx, "enable_thinking", jj_bool(ac.thinking));
        jj_dict_set(ctx, "tools", agent_js_to_jj(req_tools));
        rc = jj_render(ac.tmpl, ctx, out, err, errlen);
        jj_free(ctx);
        return rc;
    }

    return agent_render(&ac, msgs, out, err, errlen);
}

/* True when this request should have its tool calls executed by the
 * server rather than returned to the client. */
static int server_runs_tools(engine *e, js_val *req) {
    js_val *req_tools = js_get(req, "tools");
    if (!e->mcp || mcp_tool_count(e->mcp) == 0) return 0;
    if (req_tools && req_tools->type == JS_ARR) return 0;  /* client drives */
    if (js_bool(req, "raw", e->o->raw)) return 0;          /* no template */
    return 1;
}

/* ------------------------------------------------------------------ */
/* Generation                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    /* stop strings */
    char **stop;
    int    n_stop;
} stop_set;

static void stops_from(js_val *req, stop_set *ss) {
    js_val *v = js_get(req, "stop");
    ss->stop = NULL;
    ss->n_stop = 0;

    if (!v) return;
    if (v->type == JS_STR) {
        ss->stop = (char **) xmalloc(sizeof(char *));
        ss->stop[0] = xstrdup(v->str);
        ss->n_stop = 1;
    } else if (v->type == JS_ARR) {
        int i;
        ss->stop = (char **) xmalloc(sizeof(char *) * (size_t) (v->n ? v->n : 1));
        for (i = 0; i < v->n; i++) {
            if (v->items[i]->type == JS_STR) {
                ss->stop[ss->n_stop++] = xstrdup(v->items[i]->str);
            }
        }
    }
}

static void stops_free(stop_set *ss) {
    int i;
    for (i = 0; i < ss->n_stop; i++) free(ss->stop[i]);
    if (ss->stop) free(ss->stop);
}

/* Does `text` end with any stop string? Returns the length to keep. */
static int check_stop(const stop_set *ss, const char *text, size_t len,
                      size_t *keep) {
    int i;
    for (i = 0; i < ss->n_stop; i++) {
        size_t sl = strlen(ss->stop[i]);
        if (sl && len >= sl && memcmp(text + len - sl, ss->stop[i], sl) == 0) {
            *keep = len - sl;
            return 1;
        }
    }
    return 0;
}

/* Length of the longest prefix of buf[0..len) that is complete, valid
 * UTF-8.
 *
 * Byte-level BPE tokens are not aligned to character boundaries: a
 * single multi-byte character (an emoji, a CJK ideograph) is often
 * split across two or more tokens. Emitting such a fragment in an SSE
 * chunk would produce invalid UTF-8 inside a JSON string, which strict
 * clients -- including the official OpenAI SDK -- reject. We therefore
 * hold back any trailing partial sequence until its remaining bytes
 * arrive. */
static size_t utf8_complete_prefix(const char *buf, size_t len) {
    size_t i = 0;

    while (i < len) {
        unsigned char c = (unsigned char) buf[i];
        size_t need;

        if (c < 0x80)            need = 1;
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else                     need = 1;   /* stray byte: pass through */

        if (i + need > len) break;           /* truncated tail */
        i += need;
    }
    return i;
}

static void unique_id(char *out, const char *prefix) {
    static unsigned long counter = 0;
    counter++;
    sprintf(out, "%s-%lx%04lx", prefix,
            (unsigned long) time(NULL), counter & 0xFFFFUL);
}

/* Read sampling overrides out of the request body. */
static void apply_sampling(sampler_params *sp, js_val *req,
                           const infer_opts *o) {
    *sp = o->sp;
    sp->temperature    = (float) js_num(req, "temperature", sp->temperature);
    sp->top_p          = (float) js_num(req, "top_p", sp->top_p);
    sp->top_k          = (int)   js_num(req, "top_k", sp->top_k);
    sp->repeat_penalty = (float) js_num(req, "repeat_penalty",
                          js_num(req, "frequency_penalty", 0.0) != 0.0
                            ? 1.0 + js_num(req, "frequency_penalty", 0.0)
                            : sp->repeat_penalty);
    {
        js_val *s = js_get(req, "seed");
        if (s && s->type == JS_NUM) sp->seed = (unsigned long) s->num;
    }
    if (sp->temperature < 0.0f) sp->temperature = 0.0f;
    if (sp->top_p <= 0.0f || sp->top_p > 1.0f) sp->top_p = 1.0f;
}

/* The core generation loop, shared by chat and completions.
 *
 * `chat` selects the JSON envelope (chat.completion vs text_completion).
 * Returns 0 on success. On streaming requests the response has already
 * been written when this returns. */
/* `capture` non-NULL means: run the model but write nothing to the
 * socket, appending the reply text to the buffer instead. That is how
 * the server-side tool loop inspects a reply before deciding whether the
 * client should ever see it. */
static int generate(engine *e, net_conn *conn, js_val *req,
                    const char *prompt, int chat, int stream,
                    strbuf *capture) {
    tokenizer *tok = e->tok;
    int *toks;
    int n_prompt, n_max, n_ctx;
    int i, pos = 0, n_gen = 0;
    float *logits = NULL;
    sampler *smp;
    sampler_params sp;
    stop_set ss;
    strbuf text, piece_acc, json;
    char id[64];
    const char *finish = "stop";
    long created = (long) time(NULL);
    int rc = 0;
    perf_run pr;
    int eos_id = tok_eos(tok);
    int im_end = tok_id(tok, "<|im_end|>");

    if (capture) stream = 0;

    n_ctx = qwen35_n_ctx(e->ctx);
    toks = (int *) xmalloc(sizeof(int) * MAX_TOKENS_IN);
    n_prompt = tok_encode(tok, prompt, 1, toks, MAX_TOKENS_IN);

    if (n_prompt >= n_ctx) {
        free(toks);
        if (capture) {
            sb_puts(capture, "");
            return -1;
        }
        send_error(conn, 400, "Bad Request", "invalid_request_error",
                   "prompt is longer than the server context window");
        return -1;
    }

    n_max = (int) js_num(req, "max_tokens",
                         js_num(req, "max_completion_tokens",
                                e->o->n_predict));
    if (n_max <= 0) n_max = e->o->n_predict;
    if (n_prompt + n_max > n_ctx) n_max = n_ctx - n_prompt;

    apply_sampling(&sp, req, e->o);
    smp = sampler_create(&sp, qwen35_n_vocab(e->model));
    stops_from(req, &ss);

    unique_id(id, chat ? "chatcmpl" : "cmpl");
    sb_init(&text);
    sb_init(&piece_acc);
    sb_init(&json);

    /* ---- fresh sequence ---- */
    perf_begin(&pr);
    qwen35_reset(e->ctx);

    if (inf_verbose) {
        inf_log("generate: %d prompt tokens, up to %d new", n_prompt, n_max);
    }

    /* ---- prompt ingestion ---- */
    for (i = 0; i < n_prompt; i++) {
        logits = qwen35_decode(e->ctx, toks[i], pos++, i == n_prompt - 1);
        sampler_accept(smp, toks[i]);
    }
    perf_mark_prompt_done(&pr, n_prompt);

    /* ---- streaming preamble ---- */
    if (stream) {
        if (send_sse_head(conn) != NET_OK) { rc = -1; goto done; }
        if (chat) {
            sb_clear(&json);
            sb_printf(&json,
                "{\"id\":\"%s\",\"object\":\"chat.completion.chunk\","
                "\"created\":%ld,\"model\":\"%s\",\"choices\":"
                "[{\"index\":0,\"delta\":{\"role\":\"assistant\","
                "\"content\":\"\"},\"finish_reason\":null}]}",
                id, created, e->cfg->model_alias);
            if (send_sse_chunk(conn, json.data) != NET_OK) { rc = -1; goto done; }
        }
    }

    /* ---- decode loop ---- */
    while (n_gen < n_max) {
        int next;
        const char *piece;
        int plen;
        size_t keep;

        if (g_stop) { finish = "stop"; break; }

        next = sampler_pick(smp, logits);
        perf_mark_first_token(&pr);

        if (next == eos_id || (im_end >= 0 && next == im_end)) {
            finish = "stop";
            break;
        }

        piece = tok_piece(tok, next, &plen);

        /* Never emit control tokens as visible text. */
        if (!tok_is_control(tok, next)) {
            sb_add(&text, piece, (size_t) plen);
        }

        n_gen++;
        sampler_accept(smp, next);

        /* stop strings act on the decoded text */
        if (ss.n_stop && check_stop(&ss, text.data, text.len, &keep)) {
            text.len = keep;
            text.data[keep] = '\0';
            finish = "stop";
            break;
        }

        if (stream && plen > 0 && !tok_is_control(tok, next)) {
            /* Accumulate, then emit only whole UTF-8 characters. */
            size_t emit;
            sb_add(&piece_acc, piece, (size_t) plen);
            emit = utf8_complete_prefix(piece_acc.data, piece_acc.len);

            if (emit > 0) {
                sb_clear(&json);
                if (chat) {
                    sb_printf(&json,
                        "{\"id\":\"%s\",\"object\":\"chat.completion.chunk\","
                        "\"created\":%ld,\"model\":\"%s\",\"choices\":"
                        "[{\"index\":0,\"delta\":{\"content\":\"",
                        id, created, e->cfg->model_alias);
                    sb_json_escape(&json, piece_acc.data, emit);
                    sb_puts(&json, "\"},\"finish_reason\":null}]}");
                } else {
                    sb_printf(&json,
                        "{\"id\":\"%s\",\"object\":\"text_completion\","
                        "\"created\":%ld,\"model\":\"%s\",\"choices\":"
                        "[{\"index\":0,\"text\":\"",
                        id, created, e->cfg->model_alias);
                    sb_json_escape(&json, piece_acc.data, emit);
                    sb_puts(&json, "\",\"finish_reason\":null}]}");
                }
                if (send_sse_chunk(conn, json.data) != NET_OK) {
                    rc = -1;
                    goto done;
                }
                /* keep the incomplete tail for the next token */
                memmove(piece_acc.data, piece_acc.data + emit,
                        piece_acc.len - emit);
                piece_acc.len -= emit;
                piece_acc.data[piece_acc.len] = '\0';
            }
        }

        if (pos >= n_ctx - 1) { finish = "length"; break; }

        logits = qwen35_decode(e->ctx, next, pos++, 1);
    }

    if (n_gen >= n_max) finish = "length";

    perf_end(&pr, n_gen);
    perf_report(&pr, chat ? "chat.completion" : "text_completion");

    /* Drop a trailing partial UTF-8 sequence (possible when generation is
     * cut short mid-character by max_tokens or the context limit). */
    {
        size_t good = utf8_complete_prefix(text.data, text.len);
        text.len = good;
        text.data[good] = '\0';
    }

    /* ---- final response ---- */
    if (capture) {
        sb_add(capture, text.data, text.len);
        goto done;
    }

    if (stream) {
        sb_clear(&json);
        if (chat) {
            sb_printf(&json,
                "{\"id\":\"%s\",\"object\":\"chat.completion.chunk\","
                "\"created\":%ld,\"model\":\"%s\",\"choices\":"
                "[{\"index\":0,\"delta\":{},\"finish_reason\":\"%s\"}],"
                "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,"
                "\"total_tokens\":%d}}",
                id, created, e->cfg->model_alias, finish,
                n_prompt, n_gen, n_prompt + n_gen);
        } else {
            sb_printf(&json,
                "{\"id\":\"%s\",\"object\":\"text_completion\","
                "\"created\":%ld,\"model\":\"%s\",\"choices\":"
                "[{\"index\":0,\"text\":\"\",\"finish_reason\":\"%s\"}],"
                "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,"
                "\"total_tokens\":%d}}",
                id, created, e->cfg->model_alias, finish,
                n_prompt, n_gen, n_prompt + n_gen);
        }
        send_sse_chunk(conn, json.data);
        net_write_all(conn, "data: [DONE]\n\n", 14);
    } else {
        sb_clear(&json);
        if (chat) {
            sb_printf(&json,
                "{\"id\":\"%s\",\"object\":\"chat.completion\","
                "\"created\":%ld,\"model\":\"%s\",\"choices\":"
                "[{\"index\":0,\"message\":{\"role\":\"assistant\","
                "\"content\":\"",
                id, created, e->cfg->model_alias);
            sb_json_escape(&json, text.data, text.len);
            sb_printf(&json, "\"},\"logprobs\":null,\"finish_reason\":\"%s\"}],"
                "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,"
                "\"total_tokens\":%d}}",
                finish, n_prompt, n_gen, n_prompt + n_gen);
        } else {
            sb_printf(&json,
                "{\"id\":\"%s\",\"object\":\"text_completion\","
                "\"created\":%ld,\"model\":\"%s\",\"choices\":"
                "[{\"index\":0,\"text\":\"",
                id, created, e->cfg->model_alias);
            sb_json_escape(&json, text.data, text.len);
            sb_printf(&json, "\",\"logprobs\":null,\"finish_reason\":\"%s\"}],"
                "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,"
                "\"total_tokens\":%d}}",
                finish, n_prompt, n_gen, n_prompt + n_gen);
        }
        send_response(conn, 200, "OK", "application/json", json.data);
    }

done:
    sb_free(&text);
    sb_free(&piece_acc);
    sb_free(&json);
    stops_free(&ss);
    sampler_free(smp);
    free(toks);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Routing                                                            */
/* ------------------------------------------------------------------ */

static int auth_ok(const server_config *cfg, const http_req *r) {
    const char *p;
    const infer_opts *o = cfg->opts;
    if (!o->api_key || !o->api_key[0]) return 1;
    p = r->auth;
    if (strncmp(p, "Bearer ", 7) == 0) p += 7;
    return strcmp(p, o->api_key) == 0;
}

static void handle_models(net_conn *c, const server_config *cfg) {
    strbuf b;
    sb_init(&b);
    sb_printf(&b,
        "{\"object\":\"list\",\"data\":[{\"id\":\"%s\","
        "\"object\":\"model\",\"created\":%ld,\"owned_by\":\"infer\"}]}",
        cfg->model_alias, (long) time(NULL));
    send_response(c, 200, "OK", "application/json", b.data);
    sb_free(&b);
}

static void handle_request(engine *e, net_conn *c, http_req *r) {
    const server_config *cfg = e->cfg;

    if (strcmp(r->method, "OPTIONS") == 0) {
        send_response(c, 204, "No Content", "text/plain", "");
        return;
    }

    if (strcmp(r->method, "GET") == 0) {
        if (strcmp(r->path, "/health") == 0 ||
            strcmp(r->path, "/v1/health") == 0) {
            send_response(c, 200, "OK", "application/json",
                          "{\"status\":\"ok\"}");
            return;
        }
        if (strcmp(r->path, "/v1/models") == 0 ||
            strcmp(r->path, "/models") == 0) {
            if (!auth_ok(cfg, r)) {
                send_error(c, 401, "Unauthorized", "invalid_request_error",
                           "invalid API key");
                return;
            }
            handle_models(c, cfg);
            return;
        }
        send_error(c, 404, "Not Found", "invalid_request_error",
                   "unknown endpoint");
        return;
    }

    if (strcmp(r->method, "POST") != 0) {
        send_error(c, 405, "Method Not Allowed", "invalid_request_error",
                   "method not allowed");
        return;
    }

    if (!auth_ok(cfg, r)) {
        send_error(c, 401, "Unauthorized", "invalid_request_error",
                   "invalid API key");
        return;
    }

    {
        int is_chat = (strcmp(r->path, "/v1/chat/completions") == 0 ||
                       strcmp(r->path, "/chat/completions") == 0);
        int is_comp = (strcmp(r->path, "/v1/completions") == 0 ||
                       strcmp(r->path, "/completions") == 0);
        js_val *req;
        int stream;

        if (!is_chat && !is_comp) {
            send_error(c, 404, "Not Found", "invalid_request_error",
                       "unknown endpoint");
            return;
        }

        req = r->body ? js_parse(r->body) : NULL;
        if (!req || req->type != JS_OBJ) {
            js_free(req);
            send_error(c, 400, "Bad Request", "invalid_request_error",
                       "request body is not a JSON object");
            return;
        }

        stream = js_bool(req, "stream", 0);

        if (is_chat) {
            js_val *messages = js_get(req, "messages");
            strbuf prompt;
            jj_val *msgs;
            char terr[256];
            int raw = js_bool(req, "raw", e->o->raw);

            if (!messages || messages->type != JS_ARR || messages->n == 0) {
                js_free(req);
                send_error(c, 400, "Bad Request", "invalid_request_error",
                           "'messages' must be a non-empty array");
                return;
            }

            /* --raw: no template, no tools, just the text. This is the
             * path benchmark harnesses that dislike Jinja want. */
            if (raw) {
                raw_from_messages(messages, &prompt);
                generate(e, c, req, prompt.data, 1, stream, NULL);
                sb_free(&prompt);
                js_free(req);
                return;
            }

            msgs = chat_messages(e, messages);

            /* Server-side tool loop: generate into a buffer, run any
             * tool the model asks for, append the result and go round
             * again. The client sees only the final reply, so plain
             * OpenAI clients get MCP tools without knowing it. */
            if (server_runs_tools(e, req)) {
                int rounds;
                for (rounds = 0; rounds < e->o->tool_rounds; rounds++) {
                    strbuf reply, targs, result;
                    char *tname = NULL;

                    if (render_chat(e, req, msgs, &prompt,
                                    terr, sizeof(terr)) != 0) {
                        jj_free(msgs);
                        js_free(req);
                        send_error(c, 500, "Internal Server Error",
                                   "server_error", terr);
                        return;
                    }

                    sb_init(&reply);
                    generate(e, c, req, prompt.data, 1, 0, &reply);
                    sb_free(&prompt);

                    if (!agent_parse_tool_call(reply.data, &tname, &targs)) {
                        sb_free(&reply);
                        break;             /* a normal answer: emit it below */
                    }

                    if (inf_verbose) {
                        inf_log("tool call: %s %.200s", tname, targs.data);
                    }
                    agent_add_msg(msgs, "assistant", reply.data);
                    {
                        agent_config ac;
                        memset(&ac, 0, sizeof(ac));
                        ac.mcp = e->mcp;
                        agent_run_tool(&ac, tname, targs.data, &result);
                    }
                    if (inf_verbose) inf_log("tool result: %.200s", result.data);
                    agent_add_msg(msgs, "tool", result.data);

                    sb_free(&result);
                    sb_free(&targs);
                    free(tname);
                    sb_free(&reply);
                }
            }

            if (render_chat(e, req, msgs, &prompt, terr, sizeof(terr)) != 0) {
                jj_free(msgs);
                js_free(req);
                send_error(c, 500, "Internal Server Error",
                           "server_error", terr);
                return;
            }
            jj_free(msgs);

            generate(e, c, req, prompt.data, 1, stream, NULL);
            sb_free(&prompt);
        } else {
            js_val *p = js_get(req, "prompt");
            const char *text = NULL;
            if (p && p->type == JS_STR) {
                text = p->str;
            } else if (p && p->type == JS_ARR && p->n > 0 &&
                       p->items[0]->type == JS_STR) {
                text = p->items[0]->str;
            }
            if (!text) {
                js_free(req);
                send_error(c, 400, "Bad Request", "invalid_request_error",
                           "'prompt' must be a string");
                return;
            }
            generate(e, c, req, text, 0, stream, NULL);
        }

        js_free(req);
    }
}

/* ------------------------------------------------------------------ */
/* Accept loop                                                        */
/* ------------------------------------------------------------------ */

int server_run(qwen35_model *model, const server_config *cfg) {
    net_listener *l;
    engine e;
    qwen35_params qp;

    if (net_init() != NET_OK) {
        inf_log("network init failed: %s", net_last_error());
        return 1;
    }

    l = net_listen(cfg->opts->host, cfg->opts->port, 16);
    if (!l) {
        inf_log("cannot listen on %s:%d -- %s",
                cfg->opts->host ? cfg->opts->host : "0.0.0.0", cfg->opts->port,
                net_last_error());
        net_shutdown();
        return 1;
    }

    qp.n_ctx     = cfg->opts->n_ctx;
    qp.n_threads = 1;

    e.model = model;
    e.cfg   = cfg;
    e.o     = cfg->opts;
    e.tok   = qwen35_tokenizer(model);
    e.ctx   = qwen35_ctx_create(model, &qp);
    e.mcp   = agent_connect_mcp(cfg->opts->mcp_url, 0);

    inf_log("listening on http://%s:%d",
            (cfg->opts->host && cfg->opts->host[0]) ? cfg->opts->host
                                                   : "0.0.0.0", cfg->opts->port);
    inf_log("model '%s' ready (context %d tokens)",
            cfg->model_alias, cfg->opts->n_ctx);
    inf_log("endpoints: /v1/chat/completions  /v1/completions  "
            "/v1/models  /health");
    if (cfg->opts->raw) {
        inf_log("--raw: chat requests bypass the chat template");
    }
    if (cfg->opts->system && cfg->opts->system[0]) {
        inf_log("default system prompt: %.60s", cfg->opts->system);
    }
    if (cfg->opts->thinking) inf_log("thinking enabled by default");
    if (e.mcp && mcp_tool_count(e.mcp) > 0) {
        inf_log("running the tool loop server-side (%d tool(s)); a request "
                "with its own \"tools\" array takes over",
                mcp_tool_count(e.mcp));
    }

    while (!g_stop) {
        int status;
        net_conn *c = net_accept(l, 500, &status);
        http_req r;

        if (!c) {
            if (status == NET_TIMEOUT) continue;
            if (g_stop) break;
            inf_log("accept failed: %s", net_last_error());
            continue;
        }

        net_set_nodelay(c, 1);

        if (read_request(c, &r) == 0) {
            if (inf_verbose) {
                inf_log("%s %s from %s", r.method, r.path, net_peer(c));
            }
            handle_request(&e, c, &r);
        }
        free_request(&r);
        net_close(c);
    }

    inf_log("shutting down");
    if (e.mcp) mcp_free(e.mcp);
    qwen35_ctx_free(e.ctx);
    net_listener_close(l);
    net_shutdown();
    return 0;
}
