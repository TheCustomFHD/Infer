/* chat.h -- interactive multi-turn CLI chat.
 * ANSI C (C89). */

#ifndef INFER_CHAT_H
#define INFER_CHAT_H

#include "infer.h"

/* chat is configured entirely from the shared option table; see
 * opts.h. Every field here has an identically named command-line flag
 * that also works in serve and run. */
#include "opts.h"

int chat_run(qwen35_model *m, const infer_opts *o, const char *tmpl);

#endif
