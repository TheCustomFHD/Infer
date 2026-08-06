/* opts.c -- the single command-line option table.
 *
 * See opts.h for why this exists. The short version: options used to be
 * parsed in one place and documented in another, and each mode quietly
 * ignored the ones it did not read. Now there is one table; parsing,
 * help and the "that flag does nothing here" check all read from it, so
 * the three can no longer disagree.
 *
 * ANSI C (C89).
 */

#include "opts.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_PORT    8080
#define DEFAULT_CTX     4096
#define DEFAULT_PREDICT 512

/* Option identifiers. */
enum {
    O_HOST, O_PORT, O_APIKEY, O_ALIAS,
    O_CTX, O_PREDICT, O_TEMP, O_TOPP, O_TOPK, O_REPEAT, O_SEED,
    O_PROMPT, O_SYSTEM, O_THINK, O_RAW, O_TEMPLATE,
    O_MCP, O_TOOLROUNDS, O_QUIETTOOLS,
    O_BACKEND, O_KERNEL, O_THREADS, O_LOGPERF, O_LOGSTAGES, O_LOGFILE,
    O_VERBOSE, O_HELP, O_VERSION,
    O_COUNT
};

/* Help groups, printed in this order. */
enum { G_SERVER, G_GEN, G_PROMPT, G_TOOLS, G_PERF, G_OTHER, G_COUNT };

static const char *group_title[G_COUNT] = {
    "Server options (serve)",
    "Generation options (serve, chat, run)",
    "Prompting options (serve, chat, run)",
    "Tool options (serve, chat, run)",
    "Diagnostics (all modes)",
    "Other"
};

typedef struct {
    int         id;
    const char *lng;      /* --name            */
    const char *shrt;     /* -x, or NULL       */
    const char *arg;      /* argument name, or NULL for a flag */
    int         optarg;   /* 1: the argument may be omitted    */
    int         modes;    /* where it applies  */
    int         group;
    const char *help;
} opt_def;

/* Parse an on/off word. Returns 1, 0, or -1 when it is neither, which
 * is how an optional-argument option tells "--think off" (a value) from
 * "--think" followed by the next option. */
static int on_off(const char *s) {
    if (!s) return -1;
    if (!strcmp(s,"on")  || !strcmp(s,"1") || !strcmp(s,"yes") ||
        !strcmp(s,"true")) return 1;
    if (!strcmp(s,"off") || !strcmp(s,"0") || !strcmp(s,"no")  ||
        !strcmp(s,"false")) return 0;
    return -1;
}

/* The table. Note how few entries are restricted: only the four that
 * genuinely have no meaning outside a listening socket. */
static const opt_def opts[] = {
{ O_HOST, "--host", NULL, "<addr>", 0, MODE_SERVE, G_SERVER,
  "bind address (default 127.0.0.1)" },
{ O_PORT, "--port", NULL, "<n>", 0, MODE_SERVE, G_SERVER,
  "listen port (default 8080)" },
{ O_APIKEY, "--api-key", NULL, "<key>", 0, MODE_SERVE, G_SERVER,
  "require this bearer token (default: no auth)" },
{ O_ALIAS, "--alias", NULL, "<name>", 0, MODE_SERVE, G_SERVER,
  "model name reported by the API (default: from the file)" },

{ O_CTX, "--ctx", "-c", "<n>", 0, MODE_GEN,   G_GEN,
  "context window (default 4096)" },
{ O_PREDICT, "--predict", "-n", "<n>", 0, MODE_GEN,   G_GEN,
  "max tokens to generate (default 512)" },
{ O_TEMP, "--temp", "-t", "<f>", 0, MODE_GEN,   G_GEN,
  "temperature (default 0.7)" },
{ O_TOPP, "--top-p", NULL, "<f>", 0, MODE_GEN,   G_GEN,
  "nucleus sampling (default 0.8)" },
{ O_TOPK, "--top-k", NULL, "<n>", 0, MODE_GEN,   G_GEN,
  "top-k sampling (default 40)" },
{ O_REPEAT, "--repeat", NULL, "<f>", 0, MODE_GEN,   G_GEN,
  "repetition penalty (default 1.05)" },
{ O_SEED, "--seed", NULL, "<n>", 0, MODE_GEN,   G_GEN,
  "RNG seed (default: from the clock)" },

/* -p is the one prompting option that serve cannot use: an HTTP server
 * takes its prompt from the request, so a command-line one would have
 * nowhere to go. Restricting it here makes `serve -p ...` an error
 * rather than a silently discarded argument. */
{ O_PROMPT, "--prompt", "-p", "<text>", 0, MODE_CHAT | MODE_RUN, G_PROMPT,
  "run: the prompt (default: read stdin).\n"
  "                          chat: answer this first, then continue\n"
  "                          interactively." },
{ O_SYSTEM, "--system", NULL, "<text>", 0, MODE_GEN,   G_PROMPT,
  "system prompt. In serve it is the default for requests\n"
  "                          that carry no system message of their own." },
/* One switch, not two. `--think` alone means on; `--think off` is the
 * explicit form. Having both --think and --no-think let a user write
 * them together and left the result decided by argument order, which is
 * exactly the kind of silent ambiguity this table exists to prevent. */
{ O_THINK, "--think", NULL, "[on|off]", 1, MODE_GEN, G_PROMPT,
  "emit the model's <think> block. `--think` alone means on;\n"
  "                          `--think off` disables it (the default)." },
{ O_RAW, "--raw", NULL, NULL, 0, MODE_GEN,   G_PROMPT,
  "feed the prompt to the model verbatim, no chat template.\n"
  "                          In serve, /v1/chat/completions concatenates\n"
  "                          message contents instead of rendering Jinja --\n"
  "                          for clients such as llama-bench that dislike\n"
  "                          templates." },
{ O_TEMPLATE, "--template", NULL, "<file>", 0, MODE_GEN,   G_PROMPT,
  "render this Jinja file instead of the one in the GGUF" },

{ O_MCP, "--mcp", NULL, "<url>", 0, MODE_GEN,   G_TOOLS,
  "MCP server over HTTP, e.g. http://127.0.0.1:3000/mcp\n"
  "                          (Streamable HTTP; stdio is not supported).\n"
  "                          serve runs the tool loop itself unless the\n"
  "                          request carries its own \"tools\" array." },
{ O_TOOLROUNDS, "--tool-rounds", NULL, "<n>", 0, MODE_GEN,   G_TOOLS,
  "max tool calls per reply (default 4)" },
{ O_QUIETTOOLS, "--quiet-tools", NULL, NULL, 0, MODE_GEN,   G_TOOLS,
  "do not print tool calls and results" },

{ O_BACKEND, "--backend", NULL, "<n>", 0, MODE_ALL,   G_PERF,
#if defined(INFER_HAVE_VIS)
  "matvec kernel: auto, vis, i8, ref -- or `list`" },
#else
  "matvec kernel: auto, mmx, i8, ref -- or `list`" },
#endif
{ O_LOGPERF, "--log-perf", NULL, NULL, 0, MODE_GEN,   G_PERF,
  "log TTFT, tokens/second and seconds/token per request,\n"
  "                          and a session total when chat exits" },
{ O_LOGSTAGES, "--log-stages", NULL, NULL, 0, MODE_GEN, G_PERF,
  "print the per-stage profile at exit. Only a -DINFER_PROFILE\n"
  "                          build can do this; other builds say so and\n"
  "                          carry no profiling code at all." },
{ O_LOGFILE, "--log-file", NULL, "<file>", 0, MODE_ALL,   G_PERF,
  "write the log to <file> instead of stderr" },

{ O_KERNEL, "--kernel", NULL, "<f=b>", 0, MODE_ALL,   G_PERF,
  "per-format kernel: q4k=mmx, or `bench` to measure, or `list`" },

{ O_THREADS, "--threads", "-T", "<n>", 0, MODE_ALL,   G_PERF,
  "worker threads for matvec (default 1; 0 = one per core)" },

{ O_VERBOSE, "--verbose", "-v", NULL, 0, MODE_ALL,   G_OTHER,
  "log progress and each request" },
{ O_HELP, "--help", "-h", NULL, 0, MODE_ALL,   G_OTHER,
  "this message" },
{ O_VERSION, "--version", NULL, NULL, 0, MODE_ALL,   G_OTHER,
  "print the version and exit" }
};

#define N_OPTS ((int) (sizeof(opts) / sizeof(opts[0])))

const char *opts_mode_name(int mode) {
    switch (mode) {
        case MODE_SERVE: return "serve";
        case MODE_CHAT:  return "chat";
        case MODE_RUN:   return "run";
        case MODE_INFO:  return "info";
        default:         return "?";
    }
}

/* Which modes accept this option, as text for the diagnostic. */
static void modes_text(int modes, char *buf, size_t n) {
    buf[0] = '\0';
    if (modes == MODE_ALL) { strncpy(buf, "all modes", n - 1); buf[n-1] = 0; return; }
    if (modes & MODE_SERVE) strncat(buf, "serve, ", n - strlen(buf) - 1);
    if (modes & MODE_CHAT)  strncat(buf, "chat, ",  n - strlen(buf) - 1);
    if (modes & MODE_RUN)   strncat(buf, "run, ",   n - strlen(buf) - 1);
    if (modes & MODE_INFO)  strncat(buf, "info, ",  n - strlen(buf) - 1);
    { size_t l = strlen(buf); if (l >= 2) buf[l - 2] = '\0'; }
}

void opts_default(infer_opts *o) {
    memset(o, 0, sizeof(*o));
    o->mode        = MODE_SERVE;
    o->host        = "127.0.0.1";
    o->port        = DEFAULT_PORT;
    o->n_ctx       = DEFAULT_CTX;
    o->n_predict   = DEFAULT_PREDICT;
    o->tool_rounds = 4;
    o->show_tools  = 1;
    o->thinking    = 0;
    /* Single-threaded by default. Threading is opt-in because it is a
     * throughput trade, not a free win: on a 2-core box it bought
     * nothing over a saturated memory bus, and it turns a wrong
     * --threads guess into a support problem. Ask for it explicitly
     * with --threads/-T. */
    o->threads     = 1;
    sampler_default(&o->sp);
}

/* ------------------------------------------------------------------ */
/* Help                                                               */
/* ------------------------------------------------------------------ */

/* C90 guarantees only 509 characters per string literal, so the help is
 * emitted in many small calls rather than one big one. */
void opts_usage(void) {
    int g, i;

    printf("infer %s -- GGUF inference server for the Qwen3.5 "
           "architecture\n\n", INFER_VERSION);

    printf("Usage:\n"
"  infer serve <model.gguf> [options]   OpenAI-compatible HTTP server\n"
"  infer chat  <model.gguf> [options]   interactive multi-turn chat\n"
"  infer run   <model.gguf> [options]   generate once on the console\n"
"  infer info  <model.gguf>             show model metadata and exit\n\n");

    printf("Options are shared between modes wherever they can be: the\n"
           "same --system, --think, --raw, --template and --mcp work in\n"
           "serve, chat and run alike. Using one where it has no meaning\n"
           "is reported, never silently ignored.\n\n");

    for (g = 0; g < G_COUNT; g++) {
        printf("%s:\n", group_title[g]);
        for (i = 0; i < N_OPTS; i++) {
            char lhs[48];
            if (opts[i].group != g) continue;
            if (opts[i].shrt) {
                sprintf(lhs, "%s, %s", opts[i].shrt, opts[i].lng);
            } else {
                sprintf(lhs, "    %s", opts[i].lng);
            }
            if (opts[i].arg) {
                strcat(lhs, " ");
                strncat(lhs, opts[i].arg, sizeof(lhs) - strlen(lhs) - 1);
            }
            printf("  %-23s %s\n", lhs, opts[i].help);
        }
        printf("\n");
    }

    printf("Examples:\n"
"  infer serve model.gguf --host 0.0.0.0 --api-key secret\n"
"  infer serve model.gguf --system \"You are terse.\" --think\n"
"  infer serve model.gguf --raw                 (for llama-bench etc.)\n"
"  infer chat  model.gguf --mcp http://127.0.0.1:3000/mcp\n"
"  infer chat  model.gguf -p \"hello\" --think\n"
"  infer run   model.gguf -p \"Write a haiku about DOS\" --system \"Be brief.\"\n"
"  infer run   model.gguf -p \"once upon a time\" --raw\n\n");

    printf("The server speaks the OpenAI API, so existing clients work "
           "unchanged:\n"
"  curl http://127.0.0.1:8080/v1/chat/completions \\\n"
"    -H 'Content-Type: application/json' \\\n"
"    -d '{\"model\":\"local\",\"messages\":"
"[{\"role\":\"user\",\"content\":\"hi\"}]}'\n");
}

/* ------------------------------------------------------------------ */
/* Parsing                                                            */
/* ------------------------------------------------------------------ */

static const opt_def *find_opt(const char *a) {
    int i;
    for (i = 0; i < N_OPTS; i++) {
        if (strcmp(a, opts[i].lng) == 0) return &opts[i];
        if (opts[i].shrt && strcmp(a, opts[i].shrt) == 0) return &opts[i];
    }
    return NULL;
}

int opts_parse(infer_opts *o, int argc, char **argv) {
    const char *cmd;
    int i;

    opts_default(o);

    if (argc < 2) { opts_usage(); return -1; }

    cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "help") == 0) {
        opts_usage();
        return 1;
    }
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        printf("infer %s\n", INFER_VERSION);
        return 1;
    }
    /* `infer --backend list` is useful with no model loaded. */
    if (strcmp(cmd, "--backend") == 0) {
        if (argc >= 3 && strcmp(argv[2], "list") == 0) {
            o->backend = "list";
            o->mode = MODE_INFO;
            o->model_path = NULL;
            return 0;
        }
        inf_log("usage: infer --backend list");
        return -1;
    }
    /* `infer --kernel bench|list` likewise: both are about this
     * machine, not about a model, and forcing a model path on them
     * would make the benchmark useless before you have one. */
    if (strcmp(cmd, "--kernel") == 0) {
        if (argc >= 3) {
            o->kernel = argv[2];
            o->mode = MODE_INFO;
            o->model_path = NULL;
            /* -v after the argument still enables the table */
            if (argc >= 4 && (strcmp(argv[3], "-v") == 0 ||
                              strcmp(argv[3], "--verbose") == 0)) {
                inf_verbose = 1;
            }
            return 0;
        }
        inf_log("usage: infer --kernel bench|list|<format>=<backend>");
        return -1;
    }

    if      (strcmp(cmd, "serve") == 0) { o->mode = MODE_SERVE; i = 3; }
    else if (strcmp(cmd, "chat")  == 0) { o->mode = MODE_CHAT;  i = 3; }
    else if (strcmp(cmd, "run")   == 0) { o->mode = MODE_RUN;   i = 3; }
    else if (strcmp(cmd, "info")  == 0) { o->mode = MODE_INFO;  i = 3; }
    else if (strstr(cmd, ".gguf") != NULL) {
        /* `infer model.gguf` is shorthand for `infer serve model.gguf`. */
        o->mode = MODE_SERVE;
        o->model_path = cmd;
        i = 2;
    } else {
        inf_log("unknown command '%s' (expected serve, chat, run or info)", cmd);
        opts_usage();
        return -1;
    }

    if (!o->model_path) {
        if (argc < 3) { inf_log("missing model path"); opts_usage(); return -1; }
        o->model_path = argv[2];
    }

    for (; i < argc; i++) {
        const char *a = argv[i];
        const opt_def *d = find_opt(a);
        const char *val = NULL;

        if (!d) {
            inf_log("unknown option '%s' (try --help)", a);
            return -1;
        }
        if (d->arg && !d->optarg) {
            if (i + 1 >= argc) {
                inf_log("option '%s' needs a value", a);
                return -1;
            }
            val = argv[++i];
        } else if (d->arg && d->optarg) {
            /* Consume the next word only when it is actually a value.
             * `--think off` takes it; `--think --raw` does not. A word
             * that is neither on/off nor an option is a typo, and
             * reporting it as an unknown option would be misleading. */
            if (i + 1 < argc && on_off(argv[i + 1]) >= 0) {
                val = argv[++i];
            } else if (i + 1 < argc && argv[i + 1][0] != '-' &&
                       !find_opt(argv[i + 1])) {
                inf_log("'%s' expects on or off, not '%s'",
                        d->lng, argv[i + 1]);
                return -1;
            }
        }

        /* The point of the table: complain instead of ignoring. */
        if ((d->modes & o->mode) == 0) {
            char mt[64];
            modes_text(d->modes, mt, sizeof(mt));
            inf_log("'%s' has no effect in %s mode (it applies to: %s)",
                    d->lng, opts_mode_name(o->mode), mt);
            return -1;
        }

        switch (d->id) {
            case O_HOST:     o->host = val; break;
            case O_PORT:     o->port = atoi(val); break;
            case O_APIKEY:   o->api_key = val; break;
            case O_ALIAS:    o->alias = val; break;
            case O_CTX:      o->n_ctx = atoi(val); break;
            case O_PREDICT:  o->n_predict = atoi(val); break;
            case O_TEMP:     o->sp.temperature = (float) atof(val); break;
            case O_TOPP:     o->sp.top_p = (float) atof(val); break;
            case O_TOPK:     o->sp.top_k = atoi(val); break;
            case O_REPEAT:   o->sp.repeat_penalty = (float) atof(val); break;
            case O_SEED:     o->sp.seed = (unsigned long) atol(val); break;
            case O_PROMPT:   o->prompt = val; break;
            case O_SYSTEM:   o->system = val; break;
            case O_THINK:
                /* no value = on; a value = whatever it says */
                o->thinking = val ? on_off(val) : 1;
                break;
            case O_RAW:      o->raw = 1; break;
            case O_TEMPLATE: o->template_path = val; break;
            case O_MCP:      o->mcp_url = val; break;
            case O_TOOLROUNDS: o->tool_rounds = atoi(val); break;
            case O_QUIETTOOLS: o->show_tools = 0; break;
            case O_BACKEND:  o->backend = val; break;
            case O_KERNEL:   o->kernel = val; break;
            case O_THREADS:  o->threads = atoi(val); break;
            case O_LOGPERF:  o->log_perf = 1; break;
            case O_LOGSTAGES: o->log_stages = 1; break;
            case O_LOGFILE:  o->log_file = val; break;
            case O_VERBOSE:  inf_verbose = 1; break;
            case O_HELP:     opts_usage(); return 1;
            case O_VERSION:  printf("infer %s\n", INFER_VERSION); return 1;
            default: break;
        }
    }

    /* Combinations that are accepted but pointless: warn, do not fail. */
    if (o->raw && o->template_path) {
        inf_log("warning: --raw disables the chat template, so --template "
                "is unused");
    }
    if (o->raw && o->mcp_url) {
        inf_log("warning: --raw disables the chat template, so the model "
                "will not be told about the MCP tools");
    }
    if (o->raw && o->system && o->mode != MODE_SERVE) {
        inf_log("warning: --raw prepends --system verbatim, with no "
                "template markers");
    }

    if (o->n_ctx < 32) o->n_ctx = 32;
    if (o->n_predict < 1) o->n_predict = 1;
    if (o->tool_rounds < 0) o->tool_rounds = 0;
    if (o->sp.seed == 0) o->sp.seed = (unsigned long) time(NULL);

    return 0;
}
