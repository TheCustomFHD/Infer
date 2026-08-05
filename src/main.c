/* main.c -- the `infer` command-line launcher.
 *
 * Usage:
 *   infer serve model.gguf [options]     start the OpenAI-compatible server
 *   infer chat  model.gguf [options]     interactive multi-turn chat
 *   infer run   model.gguf [options]     one-shot generation on the console
 *   infer info  model.gguf               print model metadata and exit
 *
 * This file contains no inference and no networking logic: it parses
 * arguments (opts.c), loads the model through the engine API and hands
 * over to server_run(), chat_run() or the small console loop below.
 *
 * Option parsing lives in opts.c precisely so that every mode shares one
 * definition of every toggle -- see the comment at the top of opts.h.
 *
 * ANSI C (C89).
 */

#include "infer.h"
#include "opts.h"
#include "server.h"
#include "backend.h"
#include "tpool.h"
#include "prof.h"
#include "chat.h"
#include "agent.h"

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

static void on_signal(int sig) {
    (void) sig;
    server_request_stop();
    signal(sig, on_signal);
}

/* ------------------------------------------------------------------ */
/* info                                                               */
/* ------------------------------------------------------------------ */

static int cmd_info(const char *path) {
    gguf_file g;
    int i;

    if (gguf_open(&g, path) != 0) return 1;

    printf("file          : %s\n", path);
    printf("architecture  : %s\n", gguf_str(&g, "general.architecture"));
    printf("name          : %s\n", gguf_str(&g, "general.name"));
    printf("tensors       : %d\n", g.n_tensors);
    printf("metadata keys : %d\n", g.n_kv);
    printf("tensor data   : %.1f MB\n", g.blob_size / 1048576.0);
    printf("\n");

    for (i = 0; i < g.n_kv; i++) {
        gg_kv *kv = &g.kv[i];
        if (strncmp(kv->key, "tokenizer.ggml.tokens", 21) == 0 ||
            strncmp(kv->key, "tokenizer.ggml.merges", 21) == 0 ||
            strncmp(kv->key, "tokenizer.ggml.token_type", 25) == 0 ||
            strcmp(kv->key, "tokenizer.chat_template") == 0) {
            printf("%-46s <%ld entries>\n", kv->key, kv->arr_n);
            continue;
        }
        if (kv->type == GGUF_TYPE_STRING) {
            printf("%-46s %.60s\n", kv->key, kv->str ? kv->str : "");
        } else if (kv->type == GGUF_TYPE_ARRAY) {
            printf("%-46s <array of %ld>\n", kv->key, kv->arr_n);
        } else {
            printf("%-46s %g\n", kv->key, kv->num);
        }
    }

    gguf_close(&g);
    return 0;
}

/* ------------------------------------------------------------------ */
/* run                                                                */
/*                                                                    */
/* One-shot generation. It goes through exactly the same agent layer   */
/* as chat, so --system, --think, --template and --mcp behave here     */
/* identically; --raw turns all of it off and feeds the prompt to the  */
/* model verbatim.                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    qwen35_ctx *ctx;
    tokenizer  *tok;
    sampler    *smp;
    int        *toks;
    int         max_toks;
    const infer_opts *o;
} run_state;

/* Generate one reply. Streams to stdout unless `quiet`. Returns the
 * number of tokens produced, or -2 when the prompt does not fit. */
static int run_generate(run_state *r, const char *prompt, strbuf *reply,
                        perf_run *pr, int quiet) {
    int n, i, pos = 0, gen = 0;
    float *logits = NULL;
    int eos = tok_eos(r->tok);
    int im_end = tok_id(r->tok, "<|im_end|>");

    sb_init(reply);

    n = tok_encode(r->tok, prompt, 1, r->toks, r->max_toks);
    if (n >= r->o->n_ctx) return -2;

    qwen35_reset(r->ctx);
    sampler_reset(r->smp);

    for (i = 0; i < n; i++) {
        logits = qwen35_decode(r->ctx, r->toks[i], pos++, i == n - 1);
        sampler_accept(r->smp, r->toks[i]);
    }
    if (pr) perf_mark_prompt_done(pr, n);

    if (inf_verbose) inf_log("prompt: %d tokens", n);

    while (gen < r->o->n_predict && pos < r->o->n_ctx - 1) {
        int next = sampler_pick(r->smp, logits);
        const char *piece;
        int plen;

        if (next == eos || (im_end >= 0 && next == im_end)) break;
        if (pr) perf_mark_first_token(pr);

        piece = tok_piece(r->tok, next, &plen);
        if (!tok_is_control(r->tok, next)) {
            sb_add(reply, piece, (size_t) plen);
            if (!quiet) {
                fwrite(piece, 1, (size_t) plen, stdout);
                fflush(stdout);
            }
        }

        sampler_accept(r->smp, next);
        gen++;
        logits = qwen35_decode(r->ctx, next, pos++, 1);
    }
    return gen;
}

static int cmd_run(qwen35_model *m, const infer_opts *o, const char *tmpl) {
    run_state r;
    qwen35_params qp;
    agent_config ac;
    jj_val *messages;
    strbuf input, prompt, reply;
    char line[4096];
    char err[256];
    perf_run pr;
    int rounds, rc = 0, total_gen = 0;

    ac.tmpl        = tmpl;
    ac.raw         = o->raw;
    ac.thinking    = o->thinking;
    ac.tool_rounds = o->tool_rounds;
    ac.show_tools  = o->show_tools;
    ac.mcp         = agent_connect_mcp(o->mcp_url, 0);

    if (!ac.tmpl && !ac.raw) {
        inf_log("this model has no chat template; pass --template or --raw");
        if (ac.mcp) mcp_free(ac.mcp);
        return 1;
    }

    /* Where the prompt comes from: -p, else all of stdin. */
    sb_init(&input);
    if (o->prompt) {
        sb_puts(&input, o->prompt);
    } else {
        while (fgets(line, sizeof(line), stdin)) sb_puts(&input, line);
    }

    messages = agent_messages();
    if (o->system && o->system[0]) {
        agent_add_msg(messages, "system", o->system);
    }
    agent_add_msg(messages, "user", input.data);
    sb_free(&input);

    qp.n_ctx = o->n_ctx;
    qp.n_threads = 1;
    r.o = o;
    r.ctx = qwen35_ctx_create(m, &qp);
    r.tok = qwen35_tokenizer(m);
    r.smp = sampler_create(&o->sp, qwen35_n_vocab(m));
    r.max_toks = o->n_ctx;
    r.toks = (int *) xmalloc(sizeof(int) * (size_t) r.max_toks);

    perf_begin(&pr);
    total_gen = 0;

    /* The tool loop runs here too: a single-shot generation that calls a
     * tool is still a single-shot generation from the user's side. */
    for (rounds = 0; rounds <= o->tool_rounds; rounds++) {
        int gen;
        char *tname = NULL;
        strbuf targs;

        if (agent_render(&ac, messages, &prompt, err, sizeof(err)) != 0) {
            inf_log("template error: %s", err);
            rc = 1;
            break;
        }

        gen = run_generate(&r, prompt.data, &reply, rounds == 0 ? &pr : NULL, 0);
        sb_free(&prompt);
        if (gen > 0) total_gen += gen;

        if (gen == -2) {
            inf_log("prompt too long for the context window");
            sb_free(&reply);
            rc = 1;
            break;
        }

        if (ac.mcp && rounds < o->tool_rounds &&
            agent_parse_tool_call(reply.data, &tname, &targs)) {
            strbuf result;
            int ok;

            printf("\n");
            agent_add_msg(messages, "assistant", reply.data);
            if (ac.show_tools) printf("  [tool] %s %s\n", tname, targs.data);
            ok = agent_run_tool(&ac, tname, targs.data, &result);
            if (ac.show_tools) printf("  [ -> ] %.300s\n", result.data);
            else if (ok != 0) printf("  [tool error] %.200s\n", result.data);
            agent_add_msg(messages, "tool", result.data);

            sb_free(&result);
            sb_free(&targs);
            free(tname);
            sb_free(&reply);
            continue;
        }

        sb_free(&reply);
        break;
    }

    perf_end(&pr, total_gen);
    perf_report(&pr, "run");
    printf("\n");

    jj_free(messages);
    free(r.toks);
    sampler_free(r.smp);
    qwen35_ctx_free(r.ctx);
    if (ac.mcp) mcp_free(ac.mcp);
    return rc;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    infer_opts o;
    qwen35_model *m;
    const char *tmpl;
    char err[256];
    int rc;

    rc = opts_parse(&o, argc, argv);
    if (rc != 0) return rc < 0 ? 1 : 0;

    if (o.backend && strcmp(o.backend, "list") == 0) {
        bk_print_list(stdout);
        return 0;
    }
    if (o.backend) {
        int rcb = bk_select(o.backend);
        if (rcb == -1) {
            inf_log("unknown backend '%s' (try --backend list)", o.backend);
            return 1;
        }
        if (rcb == -2) {
            inf_log("backend '%s' is not supported by this CPU "
                    "(try --backend list)", o.backend);
            return 1;
        }
    }

    /* Start the worker pool before any model work. Rows of every
     * matvec are handed out across it; results stay bit-identical
     * because no partial sum ever crosses a thread. */
    {
        int nt = tp_start(o.threads);
        if (inf_verbose)
            inf_log("threads: %d (of %d logical CPUs)", nt, tp_cpu_count());
    }

    /* --kernel runs AFTER --backend, so an explicit backend is the
     * baseline a benchmark or a pin refines. `bench` measures on this
     * machine; everything else is a fixed choice. */
    if (o.kernel) {
        int rck = bk_kernel_arg(o.kernel);
        if (rck == 1) return 0;              /* `list` printed and done */
        if (rck == -1) {
            inf_log("bad --kernel '%s'; expected q4k|q5k|q6k|q8_0=<backend>, "
                    "or `bench`, `list`, `auto`", o.kernel);
            return 1;
        }
        if (rck == -2) {
            inf_log("--kernel '%s': that backend is not supported by this CPU "
                    "(try --backend list)", o.kernel);
            return 1;
        }
        /* `infer --kernel ...` with no model is a query about this
         * machine; show the result and stop. */
        if (!o.model_path) {
            bk_kernel_list(stdout);
            return 0;
        }
    }

    /* Report an unusable --log-stages before doing any work: loading a
     * 500 MB model only to say "that flag does nothing" wastes the
     * user's time, and if the path is wrong they never see it at all. */
    if (o.log_stages && !PROF_ENABLED) {
        inf_log("--log-stages needs a build with -DINFER_PROFILE "
                "(make <target> PROFILE=1); "
                "this build has no stage timers compiled in");
        o.log_stages = 0;
    }

    if (o.mode == MODE_INFO) return cmd_info(o.model_path);

    inf_log("infer %s -- loading %s", INFER_VERSION, o.model_path);
    m = qwen35_load(o.model_path, err, sizeof(err));
    if (!m) {
        inf_log("failed to load model: %s", err[0] ? err : "unknown error");
        return 1;
    }

    if (o.template_path) {
        char terr[256];
        o.template_text = load_text_file(o.template_path, terr, sizeof(terr));
        if (!o.template_text) {
            inf_log("template: %s", terr[0] ? terr : "cannot read file");
            qwen35_free(m);
            return 1;
        }
        inf_log("chat template: %s (%lu bytes)", o.template_path,
                (unsigned long) strlen(o.template_text));
    }
    tmpl = o.template_text ? o.template_text : qwen35_chat_template(m);

    inf_log("loaded '%s': %d vocab, trained context %d",
            qwen35_name(m), qwen35_n_vocab(m), qwen35_n_ctx_train(m));
    inf_log("compute backend: %s (%s)", bk_name(), bk_desc());

    /* Logging is opt-in, including in a -DINFER_PROFILE build.
     *
     * Until 1.10.0 this read `o.log_perf || PROF_ENABLED`, so
     * infer-profile.exe printed a report after every single request
     * whether or not it was asked to. A profiling build should make
     * measurement *possible*, not compulsory: the timers stay compiled
     * in, and --log-perf / --log-stages decide whether anything is
     * printed. */
    prof_stages_enabled = o.log_stages;

    if (o.log_perf || o.log_stages || o.log_file) {
        prof_perf_enabled = o.log_perf || o.log_stages;
        if (prof_open_log(o.log_file) != 0) {
            inf_log("warning: cannot write '%s', logging to stderr", o.log_file);
        }
        prof_log_banner(o.model_path, bk_name(), bk_cpu_vendor(),
                        bk_cpu_has_mmx(), bk_cpu_has_3dnow(),
                        bk_cpu_has_cmov(), o.n_ctx);
        if (o.log_file) inf_log("performance log: %s", o.log_file);
    }

    if (o.n_ctx > qwen35_n_ctx_train(m)) o.n_ctx = qwen35_n_ctx_train(m);

    if (o.mode == MODE_CHAT) {
        rc = chat_run(m, &o, tmpl);
    } else if (o.mode == MODE_RUN) {
        rc = cmd_run(m, &o, tmpl);
    } else {
        server_config cfg;

        signal(SIGINT, on_signal);
#ifdef SIGTERM
        signal(SIGTERM, on_signal);
#endif

        cfg.opts          = &o;
        cfg.model_alias   = o.alias ? o.alias : qwen35_name(m);
        cfg.chat_template = tmpl;

        rc = server_run(m, &cfg);
    }

    prof_report();
    prof_close_log();
    if (o.template_text) free(o.template_text);
    qwen35_free(m);
    return rc;
}
