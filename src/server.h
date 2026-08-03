/* server.h -- HTTP / OpenAI-compatible API layer.
 * ANSI C (C89). Uses net.h only; no OS networking headers. */

#ifndef INFER_SERVER_H
#define INFER_SERVER_H

#include "infer.h"
#include "opts.h"

typedef struct {
    /* Everything configurable comes from the shared option table, so a
     * flag added there is available to the server without touching this
     * struct. Only the two values the server derives itself live here. */
    const infer_opts *opts;
    const char *model_alias;   /* name reported by /v1/models */
    const char *chat_template; /* Jinja source, or NULL when --raw */
} server_config;

/* Run the accept loop until a fatal error or SIGINT. Returns 0 on a
 * clean shutdown. */
int server_run(qwen35_model *model, const server_config *cfg);

/* Ask the running server to stop at the next opportunity (signal-safe). */
void server_request_stop(void);

#endif
