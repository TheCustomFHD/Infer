/* chat.c -- interactive multi-turn chat on the console.
 *
 * Keeps a growing message history, renders it through the model's own
 * Jinja chat template on every turn, and streams the reply. Optionally
 * exposes tools from an MCP server and runs the tool-call loop.
 *
 * Because the whole conversation is re-rendered and re-ingested each
 * turn, the model sees exactly the prompt its template defines -- no
 * hand-rolled approximation. On a slow machine that re-ingestion is the
 * dominant cost, which is why /forget exists.
 *
 * Prompt building and tool calling live in agent.c, shared with serve
 * and run; this file is the REPL and nothing else.
 *
 * ANSI C (C89).
 */

#include "chat.h"
#include "agent.h"
#include "jinja.h"
#include "mcp.h"
#include "backend.h"
#include "prof.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Conversation state                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    jj_val *messages;      /* JJ_LIST of {role, content}     */
    qwen35_model *model;
    qwen35_ctx   *ctx;
    tokenizer    *tok;
    sampler      *smp;
    const infer_opts *o;
    agent_config  ac;
    int          *toks;
    int           max_toks;
    perf_total    totals;      /* across the whole session */
} chat_state;

static int build_prompt(chat_state *s, strbuf *out, char *err, size_t errlen) {
    return agent_render(&s->ac, s->messages, out, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Generation                                                          */
/* ------------------------------------------------------------------ */

/* Generate one assistant reply, streaming it to stdout.
 * Returns the reply text in `reply` (caller sb_frees). */
static int generate_reply(chat_state *s, const char *prompt, strbuf *reply,
                          int quiet) {
    int n, i, pos = 0, gen = 0;
    float *logits = NULL;
    int eos = tok_eos(s->tok);
    int im_end = tok_id(s->tok, "<|im_end|>");
    strbuf pending;      /* holds bytes until they form whole UTF-8 */
    perf_run pr;

    sb_init(reply);
    sb_init(&pending);

    n = tok_encode(s->tok, prompt, 1, s->toks, s->max_toks);
    if (n >= s->o->n_ctx) {
        sb_free(&pending);
        return -2;                     /* context overflow */
    }

    perf_begin(&pr);
    qwen35_reset(s->ctx);
    sampler_reset(s->smp);

    for (i = 0; i < n; i++) {
        logits = qwen35_decode(s->ctx, s->toks[i], pos++, i == n - 1);
        sampler_accept(s->smp, s->toks[i]);
    }
    perf_mark_prompt_done(&pr, n);

    while (gen < s->o->n_predict && pos < s->o->n_ctx - 1) {
        int next = sampler_pick(s->smp, logits);
        const char *piece;
        int plen;

        if (next == eos || (im_end >= 0 && next == im_end)) break;

        perf_mark_first_token(&pr);

        piece = tok_piece(s->tok, next, &plen);
        if (!tok_is_control(s->tok, next)) {
            sb_add(reply, piece, (size_t) plen);
            if (!quiet) {
                size_t emit;
                sb_add(&pending, piece, (size_t) plen);
                /* only print whole UTF-8 characters */
                emit = pending.len;
                while (emit > 0) {
                    unsigned char c = (unsigned char) pending.data[emit - 1];
                    if ((c & 0xC0) != 0x80) {
                        size_t need = c < 0x80 ? 1 :
                                      (c & 0xE0) == 0xC0 ? 2 :
                                      (c & 0xF0) == 0xE0 ? 3 : 4;
                        if (emit - 1 + need > pending.len) emit--;
                        break;
                    }
                    emit--;
                }
                if (emit == 0 && pending.len > 4) emit = pending.len;
                if (emit > 0) {
                    fwrite(pending.data, 1, emit, stdout);
                    fflush(stdout);
                    memmove(pending.data, pending.data + emit, pending.len - emit);
                    pending.len -= emit;
                    pending.data[pending.len] = '\0';
                }
            }
        }

        sampler_accept(s->smp, next);
        gen++;
        logits = qwen35_decode(s->ctx, next, pos++, 1);
    }

    if (!quiet && pending.len) {
        fwrite(pending.data, 1, pending.len, stdout);
        fflush(stdout);
    }
    sb_free(&pending);

    perf_end(&pr, gen);
    perf_report(&pr, "chat turn");
    perf_total_add(&s->totals, &pr);
    return gen;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void print_help(void) {
    printf("\ncommands:\n");
    printf("  /help              this list\n");
    printf("  /new, /clear       start a fresh conversation\n");
    printf("  /forget            drop all but the system prompt\n");
    printf("  /history           show the conversation so far\n");
    printf("  /system <text>     set or replace the system prompt\n");
    printf("  /think on|off      toggle the model's thinking block\n");
    printf("  /raw on|off        bypass the chat template\n");
    printf("  /tools             list tools from the MCP server\n");
    printf("  /prompt            show the exact rendered prompt\n");
    printf("  /stats             tokens in the current conversation\n");
    printf("  /perf              speed of the session so far\n");
    printf("  /set               show the options in effect\n");
    printf("  /quit, /exit       leave\n\n");
}

/* Mirrors the command-line flags, so the REPL and the launcher expose
 * the same set of switches under the same names. */
static void print_settings(chat_state *s) {
    printf("\n  --ctx        %d\n", s->o->n_ctx);
    printf("  --predict    %d\n", s->o->n_predict);
    printf("  --temp       %.2f\n", (double) s->o->sp.temperature);
    printf("  --top-p      %.2f\n", (double) s->o->sp.top_p);
    printf("  --top-k      %d\n", s->o->sp.top_k);
    printf("  --repeat     %.2f\n", (double) s->o->sp.repeat_penalty);
    printf("  --seed       %lu\n", s->o->sp.seed);
    printf("  --think      %s\n", s->ac.thinking ? "on" : "off");
    printf("  --raw        %s\n", s->ac.raw ? "on" : "off");
    printf("  --template   %s\n", s->o->template_path ? s->o->template_path
                                                      : "(from the GGUF)");
    printf("  --mcp        %s\n", s->o->mcp_url ? s->o->mcp_url : "(none)");
    printf("  --log-perf   %s\n", prof_perf_enabled ? "on" : "off");
    printf("  backend      %s\n\n", bk_name());
}

static void reset_conv(chat_state *s) {
    jj_free(s->messages);
    s->messages = agent_messages();
    if (s->o->system && s->o->system[0]) {
        agent_add_msg(s->messages, "system", s->o->system);
    }
}

static void show_history(chat_state *s) {
    int i;
    printf("\n");
    for (i = 0; i < s->messages->n; i++) {
        jj_val *m = s->messages->items[i];
        jj_val *r = jj_dict_get(m, "role");
        jj_val *c = jj_dict_get(m, "content");
        printf("  [%d] %-9s %.200s\n", i,
               (r && r->str) ? r->str : "?",
               (c && c->str) ? c->str : "");
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* One turn                                                            */
/* ------------------------------------------------------------------ */

/* Runs the model, then the tool loop, until it produces a plain reply. */
static void take_turn(chat_state *s) {
    char err[256];
    int rounds;

    for (rounds = 0; rounds <= s->o->tool_rounds; rounds++) {
        strbuf prompt, reply;
        int gen;
        char *tname = NULL;
        strbuf targs;

        if (build_prompt(s, &prompt, err, sizeof(err)) != 0) {
            printf("template error: %s\n", err);
            return;
        }

        gen = generate_reply(s, prompt.data, &reply, 0);
        sb_free(&prompt);

        if (gen == -2) {
            printf("\n[context full -- use /forget to continue]\n");
            sb_free(&reply);
            return;
        }
        printf("\n");

        if (s->ac.mcp && rounds < s->o->tool_rounds &&
            agent_parse_tool_call(reply.data, &tname, &targs)) {
            strbuf result;
            int ok;

            agent_add_msg(s->messages, "assistant", reply.data);

            if (s->ac.show_tools) {
                printf("  [tool] %s %s\n", tname, targs.data);
            }
            ok = agent_run_tool(&s->ac, tname, targs.data, &result);
            if (s->ac.show_tools) {
                printf("  [ -> ] %.300s\n", result.data);
            } else if (ok != 0) {
                printf("  [tool error] %.200s\n", result.data);
            }

            agent_add_msg(s->messages, "tool", result.data);

            sb_free(&result);
            sb_free(&targs);
            free(tname);
            sb_free(&reply);
            continue;                 /* let the model use the result */
        }

        agent_add_msg(s->messages, "assistant", reply.data);
        sb_free(&reply);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int chat_run(qwen35_model *m, const infer_opts *o, const char *tmpl) {
    chat_state s;
    qwen35_params qp;
    char line[8192];

    memset(&s, 0, sizeof(s));
    s.model = m;
    s.o = o;
    s.tok = qwen35_tokenizer(m);

    s.ac.tmpl        = tmpl;
    s.ac.raw         = o->raw;
    s.ac.thinking    = o->thinking;
    s.ac.tool_rounds = o->tool_rounds;
    s.ac.show_tools  = o->show_tools;
    s.ac.mcp         = NULL;

    if (!s.ac.tmpl && !s.ac.raw) {
        inf_log("this model has no chat template; pass --template or --raw");
        return 1;
    }

    qp.n_ctx = o->n_ctx;
    qp.n_threads = 1;
    s.ctx = qwen35_ctx_create(m, &qp);
    s.smp = sampler_create(&o->sp, qwen35_n_vocab(m));
    s.max_toks = o->n_ctx;
    s.toks = (int *) xmalloc(sizeof(int) * (size_t) s.max_toks);
    s.messages = agent_messages();
    perf_total_init(&s.totals);

    if (o->system && o->system[0]) {
        agent_add_msg(s.messages, "system", o->system);
    }

    s.ac.mcp = agent_connect_mcp(o->mcp_url, 1);

    printf("\ninfer chat -- %s\n", qwen35_name(m));
    printf("context %d tokens, backend %s. /help for commands, /quit to exit.\n\n",
           o->n_ctx, bk_name());

    /* -p seeds the first turn, then we drop into the REPL as usual. */
    if (o->prompt && o->prompt[0]) {
        printf("> %s\n", o->prompt);
        agent_add_msg(s.messages, "user", o->prompt);
        take_turn(&s);
        printf("\n");
    }

    for (;;) {
        fputs("> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) { printf("\n"); break; }

        {
            size_t n = strlen(line);
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        }
        if (!line[0]) continue;

        /* ---- commands ---- */
        if (line[0] == '/') {
            char err[256];
            strbuf prompt;

            if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) break;
            if (!strcmp(line, "/help")) { print_help(); continue; }
            if (!strcmp(line, "/set"))  { print_settings(&s); continue; }
            if (!strcmp(line, "/new") || !strcmp(line, "/clear")) {
                reset_conv(&s);
                printf("conversation cleared\n");
                continue;
            }
            if (!strcmp(line, "/forget")) {
                reset_conv(&s);
                printf("history dropped\n");
                continue;
            }
            if (!strcmp(line, "/history")) { show_history(&s); continue; }
            if (!strncmp(line, "/system", 7)) {
                const char *t = line + 7;
                while (*t == ' ') t++;
                if (!*t) { printf("usage: /system <text>\n"); continue; }
                agent_set_system(&s.messages, t);
                printf("system prompt set\n");
                continue;
            }
            if (!strncmp(line, "/think", 6)) {
                const char *t = line + 6;
                while (*t == ' ') t++;
                if (!strcmp(t, "on")) s.ac.thinking = 1;
                else if (!strcmp(t, "off")) s.ac.thinking = 0;
                printf("thinking %s\n", s.ac.thinking ? "on" : "off");
                continue;
            }
            if (!strncmp(line, "/raw", 4)) {
                const char *t = line + 4;
                while (*t == ' ') t++;
                if (!strcmp(t, "on")) s.ac.raw = 1;
                else if (!strcmp(t, "off")) s.ac.raw = 0;
                if (s.ac.raw && !s.ac.tmpl) {
                    /* fine: raw needs no template */
                } else if (!s.ac.raw && !s.ac.tmpl) {
                    printf("no chat template available; staying raw\n");
                    s.ac.raw = 1;
                    continue;
                }
                printf("raw %s\n", s.ac.raw ? "on (no chat template)" : "off");
                continue;
            }
            if (!strcmp(line, "/tools")) {
                int i;
                if (!s.ac.mcp || mcp_tool_count(s.ac.mcp) == 0) {
                    printf("no MCP tools (start with --mcp http://host:port/path)\n");
                } else {
                    for (i = 0; i < mcp_tool_count(s.ac.mcp); i++) {
                        printf("  %-24s %s\n", mcp_tool_name(s.ac.mcp, i),
                               mcp_tool_desc(s.ac.mcp, i));
                    }
                }
                continue;
            }
            if (!strcmp(line, "/prompt")) {
                if (build_prompt(&s, &prompt, err, sizeof(err)) != 0) {
                    printf("template error: %s\n", err);
                } else {
                    printf("---8<---\n%s\n---8<---\n", prompt.data);
                    sb_free(&prompt);
                }
                continue;
            }
            if (!strcmp(line, "/perf")) {
                if (!prof_perf_enabled) {
                    printf("not measuring; restart with --log-perf\n");
                } else {
                    perf_total_report(&s.totals, "chat session so far");
                }
                continue;
            }
            if (!strcmp(line, "/stats")) {
                if (build_prompt(&s, &prompt, err, sizeof(err)) == 0) {
                    int n = tok_encode(s.tok, prompt.data, 1, s.toks, s.max_toks);
                    printf("%d messages, %d prompt tokens of %d context\n",
                           s.messages->n, n, o->n_ctx);
                    sb_free(&prompt);
                }
                continue;
            }
            printf("unknown command '%s' -- /help for the list\n", line);
            continue;
        }

        /* ---- a normal turn ---- */
        agent_add_msg(s.messages, "user", line);
        take_turn(&s);
        printf("\n");
    }

    printf("bye\n");

    /* Session totals, so a slow machine gets one honest number for the
     * whole conversation rather than only per-turn figures. */
    perf_total_report(&s.totals, "chat session");

    free(s.toks);
    jj_free(s.messages);
    sampler_free(s.smp);
    qwen35_ctx_free(s.ctx);
    if (s.ac.mcp) mcp_free(s.ac.mcp);
    return 0;
}
