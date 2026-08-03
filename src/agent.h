/* agent.h -- prompt construction and tool calling, shared by all modes.
 *
 * `chat`, `run` and `serve` all need the same three things: turn a list
 * of messages into a prompt, notice when the model has asked to call a
 * tool, and run that call. Those used to live inside chat.c, which is
 * why --system, --think and --mcp only worked in chat. They live here
 * now, so every mode gets identical behaviour from identical code.
 *
 * ANSI C (C89).
 */

#ifndef INFER_AGENT_H
#define INFER_AGENT_H

#include "infer.h"
#include "jinja.h"
#include "json.h"
#include "mcp.h"

typedef struct {
    const char *tmpl;        /* Jinja source; NULL or unused when raw   */
    int         raw;         /* bypass the template entirely            */
    int         thinking;    /* enable_thinking                          */
    mcp_client *mcp;         /* optional, may be NULL                   */
    int         tool_rounds; /* max tool calls per reply                */
    int         show_tools;  /* print calls and results                 */
} agent_config;

/* Build a messages list. Helpers used by every mode. */
jj_val *agent_messages(void);
void    agent_add_msg(jj_val *messages, const char *role, const char *content);
/* Insert or replace the system message at position 0. */
void    agent_set_system(jj_val **messages, const char *text);
/* True when messages already begins with a system (or developer) turn. */
int     agent_has_system(const jj_val *messages);

/* Render `messages` into a prompt.
 *
 * With cfg->raw the template is skipped and the message contents are
 * concatenated with blank lines between them -- what a base model or a
 * benchmark harness expects. Otherwise the Jinja template is rendered
 * with messages/add_generation_prompt/enable_thinking/tools bound.
 *
 * Returns 0 on success, non-zero with a reason in errbuf. */
int agent_render(const agent_config *cfg, const jj_val *messages,
                 strbuf *out, char *errbuf, size_t errlen);

/* Parse the first tool call in `text`. Returns 1 when one was found and
 * fills `name` (free()) and `args_json` (sb_free()). Accepts both the
 * <tool_call><function=..> XML form and a bare <function=..> block,
 * which small models emit surprisingly often. */
int agent_parse_tool_call(const char *text, char **name, strbuf *args_json);

/* Run one tool call through MCP, writing the textual result to `out`
 * (caller sb_frees). Returns 0 on success. Handles the unknown-tool
 * case by reporting it back to the model rather than failing. */
int agent_run_tool(const agent_config *cfg, const char *name,
                   const char *args_json, strbuf *out);

/* Connect to an MCP server and print what it offers. Returns NULL on
 * failure, having already explained why -- callers continue without
 * tools rather than aborting. */
mcp_client *agent_connect_mcp(const char *url, int announce);

/* Convert a parsed JSON value into the equivalent Jinja value. Used to
 * hand whole OpenAI messages -- tool_calls, structured content and all
 * -- to the template unchanged, and to embed MCP tool schemas. */
jj_val *agent_js_to_jj(const js_val *v);

#endif
