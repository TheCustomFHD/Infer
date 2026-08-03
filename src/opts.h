/* opts.h -- one command-line option table shared by every mode.
 *
 * Every toggle `infer` understands is declared exactly once, in the
 * table in opts.c, together with the set of modes it applies to. That
 * table drives three things at once:
 *
 *   - parsing            (opts_parse)
 *   - the help text       (opts_usage) -- so help can never drift
 *   - applicability       (a flag used in a mode that ignores it is
 *                          reported instead of silently dropped)
 *
 * The rule the table encodes is "general, not per mode": an option is
 * restricted only when it is genuinely meaningless elsewhere. --host
 * and --port are server-only because nothing else listens on a socket;
 * --system, --think, --mcp, --template, --raw, --prompt and every
 * sampling knob apply everywhere they can possibly apply.
 *
 * ANSI C (C89).
 */

#ifndef INFER_OPTS_H
#define INFER_OPTS_H

#include "infer.h"

/* Modes, as a bit set so an option can list several. */
#define MODE_SERVE  1
#define MODE_CHAT   2
#define MODE_RUN    4
#define MODE_INFO   8

#define MODE_GEN    (MODE_SERVE | MODE_CHAT | MODE_RUN)
#define MODE_ALL    (MODE_GEN | MODE_INFO)

typedef struct {
    int         mode;            /* MODE_* -- which sub-command      */
    const char *model_path;

    /* server */
    const char *host;
    int         port;
    const char *api_key;
    const char *alias;

    /* generation */
    int         n_ctx;
    int         n_predict;
    sampler_params sp;

    /* prompting -- all of these work in serve, chat and run */
    const char *prompt;          /* -p: run input, or chat's first turn */
    const char *system;
    const char *mcp_url;
    const char *template_path;
    int         thinking;
    int         raw;
    int         tool_rounds;
    int         show_tools;

    /* diagnostics */
    const char *backend;
    const char *log_file;
    int         log_perf;
    int         log_stages;      /* --log-stages, needs a profile build */

    /* filled in by main after parsing, not by the table */
    char       *template_text;
} infer_opts;

void opts_default(infer_opts *o);

/* Parse argv. Returns 0 to continue, 1 to exit with success (help or
 * version was printed), -1 on a usage error (already reported). */
int  opts_parse(infer_opts *o, int argc, char **argv);

/* The full help text, generated from the table. */
void opts_usage(void);

/* Human-readable mode name, e.g. "run". */
const char *opts_mode_name(int mode);

#endif
