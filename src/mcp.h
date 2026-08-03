/* mcp.h -- Model Context Protocol client over Streamable HTTP.
 *
 * Connects to MCP servers reachable at an http:// URL, discovers their
 * tools, and calls them. Transport is HTTP POST of JSON-RPC 2.0, which
 * is the "Streamable HTTP" transport from the MCP specification.
 *
 * stdio transport is deliberately NOT implemented: it requires spawning
 * child processes and wiring pipes, which has no portable ANSI C form
 * and would need entirely separate POSIX and Win32 backends.
 *
 * All networking goes through net.h, so this works unchanged on
 * Windows XP.
 *
 * ANSI C (C89).
 */

#ifndef INFER_MCP_H
#define INFER_MCP_H

#include "infer.h"
#include "jinja.h"

typedef struct mcp_client mcp_client;

/* Connect to an MCP server and run the initialise handshake plus
 * tools/list. `url` looks like http://host:port/path .
 * Returns NULL on failure with a reason in errbuf. */
mcp_client *mcp_connect(const char *url, char *errbuf, size_t errlen);
void        mcp_free(mcp_client *m);

/* Number of tools the server offers, and their metadata. */
int         mcp_tool_count(const mcp_client *m);
const char *mcp_tool_name(const mcp_client *m, int i);
const char *mcp_tool_desc(const mcp_client *m, int i);

/* The tool list as a jj_val LIST of OpenAI-style
 * {"type":"function","function":{...}} dicts, for feeding to a chat
 * template's `tools` variable. Caller owns the result. */
jj_val *mcp_tools_as_jinja(const mcp_client *m);

/* Is `name` provided by this server? */
int mcp_has_tool(const mcp_client *m, const char *name);

/* Invoke a tool. `args_json` is a JSON object literal. On success
 * returns 0 and fills `out` (caller must sb_free) with the textual
 * result. On failure returns -1 and puts the reason in `out`. */
int mcp_call(mcp_client *m, const char *name, const char *args_json,
             strbuf *out);

/* Server-reported name, for logging. */
const char *mcp_server_name(const mcp_client *m);

#endif
