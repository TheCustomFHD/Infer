/* mcp.c -- MCP client over Streamable HTTP. See mcp.h.
 *
 * Protocol shape:
 *   POST <path> HTTP/1.1
 *   Content-Type: application/json
 *   Accept: application/json, text/event-stream
 *   {"jsonrpc":"2.0","id":N,"method":"...","params":{...}}
 *
 * The response is either a plain JSON body or an SSE stream whose
 * `data:` lines carry the JSON. Both are handled.
 *
 * ANSI C (C89). Networking via net.h only.
 */

#include "mcp.h"
#include "agent.h"
#include "net.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

#define MCP_TIMEOUT 30000     /* ms */

typedef struct {
    char *name;
    char *desc;
    char *schema;             /* inputSchema as raw JSON */
} mcp_tool;

struct mcp_client {
    char      host[128];
    char      path[192];
    int       port;
    char      session[128];   /* Mcp-Session-Id, if the server issues one */
    char      server_name[96];
    mcp_tool *tools;
    int       n_tools;
    int       next_id;
};

/* ------------------------------------------------------------------ */
/* URL parsing                                                         */
/* ------------------------------------------------------------------ */

static int parse_url(const char *url, char *host, size_t hostlen,
                     int *port, char *path, size_t pathlen) {
    const char *p = url;
    const char *slash;
    const char *colon;
    size_t n;

    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) return -2;   /* no TLS */

    slash = strchr(p, '/');
    n = slash ? (size_t) (slash - p) : strlen(p);
    if (n >= hostlen) n = hostlen - 1;
    memcpy(host, p, n);
    host[n] = '\0';

    *port = 80;
    colon = strchr(host, ':');
    if (colon) {
        *port = atoi(colon + 1);
        *((char *) colon) = '\0';
        if (*port <= 0) *port = 80;
    }

    if (slash) {
        strncpy(path, slash, pathlen - 1);
        path[pathlen - 1] = '\0';
    } else {
        strcpy(path, "/");
    }
    return host[0] ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* One JSON-RPC round trip                                             */
/* ------------------------------------------------------------------ */

/* Extract the JSON body from an HTTP response, handling both a plain
 * body and an SSE stream (`data: {...}` lines). Returns a pointer into
 * `resp`, and sets *len. */
static const char *extract_json(const char *resp, size_t *len) {
    const char *body = strstr(resp, "\r\n\r\n");
    const char *d;

    if (body) body += 4;
    else {
        body = strstr(resp, "\n\n");
        body = body ? body + 2 : resp;
    }

    /* SSE: find the last "data:" line, which carries the response */
    d = strstr(body, "data:");
    if (d && strstr(resp, "text/event-stream")) {
        const char *best = NULL;
        while (d) {
            best = d;
            d = strstr(d + 5, "data:");
        }
        if (best) {
            const char *s = best + 5;
            const char *e;
            while (*s == ' ') s++;
            e = strchr(s, '\n');
            *len = e ? (size_t) (e - s) : strlen(s);
            while (*len > 0 && (s[*len - 1] == '\r')) (*len)--;
            return s;
        }
    }

    /* chunked transfer: skip the size line if present */
    if (strstr(resp, "Transfer-Encoding: chunked") ||
        strstr(resp, "transfer-encoding: chunked")) {
        const char *nl = strchr(body, '\n');
        if (nl && body[0] != '{') body = nl + 1;
    }

    *len = strlen(body);
    return body;
}

/* Send one JSON-RPC request; returns the parsed response or NULL.
 * `notify` sends a notification (no id, no response expected). */
static js_val *rpc(mcp_client *m, const char *method, const char *params,
                   int notify, char *errbuf, size_t errlen) {
    net_conn *c;
    strbuf req, body, resp;
    js_val *root = NULL;
    char hdr[512];

    sb_init(&body);
    if (notify) {
        sb_printf(&body, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\"", method);
    } else {
        sb_printf(&body, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"",
                  m->next_id++, method);
    }
    if (params && params[0]) {
        sb_puts(&body, ",\"params\":");
        sb_puts(&body, params);
    }
    sb_puts(&body, "}");

    c = net_connect(m->host, m->port, MCP_TIMEOUT);
    if (!c) {
        if (errbuf) sprintf(errbuf, "connect %.60s:%d failed: %.80s",
                            m->host, m->port, net_last_error());
        sb_free(&body);
        return NULL;
    }
    net_set_nodelay(c, 1);

    sb_init(&req);
    sb_printf(&req, "POST %s HTTP/1.1\r\n", m->path);
    sb_printf(&req, "Host: %s:%d\r\n", m->host, m->port);
    sb_puts(&req, "Content-Type: application/json\r\n");
    sb_puts(&req, "Accept: application/json, text/event-stream\r\n");
    sb_puts(&req, "MCP-Protocol-Version: 2025-06-18\r\n");
    if (m->session[0]) {
        sprintf(hdr, "Mcp-Session-Id: %s\r\n", m->session);
        sb_puts(&req, hdr);
    }
    sb_printf(&req, "Content-Length: %lu\r\n", (unsigned long) body.len);
    sb_puts(&req, "Connection: close\r\n\r\n");
    sb_add(&req, body.data, body.len);

    if (net_write_all(c, req.data, req.len) != NET_OK) {
        if (errbuf) sprintf(errbuf, "send failed: %.80s", net_last_error());
        net_close(c);
        sb_free(&req); sb_free(&body);
        return NULL;
    }
    sb_free(&req);
    sb_free(&body);

    sb_init(&resp);
    for (;;) {
        char buf[2048];
        int n = net_read(c, buf, sizeof(buf) - 1, MCP_TIMEOUT);
        if (n == NET_CLOSED) break;
        if (n <= 0) {
            if (resp.len == 0) {
                if (errbuf) sprintf(errbuf, "no response from server");
                net_close(c);
                sb_free(&resp);
                return NULL;
            }
            break;
        }
        sb_add(&resp, buf, (size_t) n);
        if (resp.len > 4 * 1024 * 1024) break;
    }
    net_close(c);

    /* capture a session id if the server issued one */
    {
        const char *s = strstr(resp.data, "Mcp-Session-Id:");
        if (!s) s = strstr(resp.data, "mcp-session-id:");
        if (s) {
            const char *v = strchr(s, ':') + 1;
            const char *e;
            size_t n;
            while (*v == ' ') v++;
            e = strpbrk(v, "\r\n");
            n = e ? (size_t) (e - v) : strlen(v);
            if (n >= sizeof(m->session)) n = sizeof(m->session) - 1;
            memcpy(m->session, v, n);
            m->session[n] = '\0';
        }
    }

    if (notify) { sb_free(&resp); return NULL; }

    {
        size_t jlen;
        const char *j = extract_json(resp.data, &jlen);
        char *tmp = (char *) xmalloc(jlen + 1);
        memcpy(tmp, j, jlen);
        tmp[jlen] = '\0';
        root = js_parse(tmp);
        if (!root && errbuf) {
            sprintf(errbuf, "bad JSON response (%.60s)", tmp);
        }
        free(tmp);
    }
    sb_free(&resp);
    return root;
}

/* ------------------------------------------------------------------ */
/* Connect and discover                                                */
/* ------------------------------------------------------------------ */

/* Re-serialise a parsed JSON value (used to keep inputSchema verbatim). */
static void js_dump(const js_val *v, strbuf *b) {
    int i;
    char tmp[64];
    if (!v) { sb_puts(b, "null"); return; }
    switch (v->type) {
        case JS_NULL: sb_puts(b, "null"); break;
        case JS_BOOL: sb_puts(b, v->num != 0 ? "true" : "false"); break;
        case JS_NUM:
            if (v->num == (double) (long) v->num) sprintf(tmp, "%ld", (long) v->num);
            else sprintf(tmp, "%g", v->num);
            sb_puts(b, tmp);
            break;
        case JS_STR:
            sb_puts(b, "\"");
            sb_json_escape(b, v->str ? v->str : "", v->str ? strlen(v->str) : 0);
            sb_puts(b, "\"");
            break;
        case JS_ARR:
            sb_puts(b, "[");
            for (i = 0; i < v->n; i++) { if (i) sb_puts(b, ","); js_dump(v->items[i], b); }
            sb_puts(b, "]");
            break;
        default:
            sb_puts(b, "{");
            for (i = 0; i < v->n; i++) {
                if (i) sb_puts(b, ",");
                sb_puts(b, "\"");
                sb_json_escape(b, v->keys[i], strlen(v->keys[i]));
                sb_puts(b, "\":");
                js_dump(v->items[i], b);
            }
            sb_puts(b, "}");
            break;
    }
}

mcp_client *mcp_connect(const char *url, char *errbuf, size_t errlen) {
    mcp_client *m;
    js_val *r;
    int pr;

    if (errbuf && errlen) errbuf[0] = '\0';

    m = (mcp_client *) xcalloc(1, sizeof(mcp_client));
    m->next_id = 1;
    strcpy(m->server_name, "mcp");

    pr = parse_url(url, m->host, sizeof(m->host), &m->port,
                   m->path, sizeof(m->path));
    if (pr == -2) {
        if (errbuf) strcpy(errbuf, "https:// is not supported (no TLS in this build); use http://");
        free(m);
        return NULL;
    }
    if (pr != 0) {
        if (errbuf) sprintf(errbuf, "cannot parse URL '%.80s'", url);
        free(m);
        return NULL;
    }

    if (net_init() != NET_OK) {
        if (errbuf) strcpy(errbuf, "network init failed");
        free(m);
        return NULL;
    }

    /* --- initialize --- */
    r = rpc(m, "initialize",
            "{\"protocolVersion\":\"2025-06-18\","
            "\"capabilities\":{},"
            "\"clientInfo\":{\"name\":\"infer\",\"version\":\"" INFER_VERSION "\"}}",
            0, errbuf, errlen);
    if (!r) { free(m); return NULL; }
    {
        js_val *res = js_get(r, "result");
        js_val *si = res ? js_get(res, "serverInfo") : NULL;
        const char *nm = si ? js_str(si, "name", NULL) : NULL;
        if (nm) {
            strncpy(m->server_name, nm, sizeof(m->server_name) - 1);
            m->server_name[sizeof(m->server_name) - 1] = '\0';
        }
        if (js_get(r, "error")) {
            js_val *e = js_get(r, "error");
            if (errbuf) sprintf(errbuf, "initialize failed: %.90s",
                                js_str(e, "message", "unknown"));
            js_free(r);
            free(m);
            return NULL;
        }
    }
    js_free(r);

    /* --- notifications/initialized --- */
    rpc(m, "notifications/initialized", "{}", 1, NULL, 0);

    /* --- tools/list --- */
    r = rpc(m, "tools/list", "{}", 0, errbuf, errlen);
    if (!r) { free(m); return NULL; }
    {
        js_val *res = js_get(r, "result");
        js_val *arr = res ? js_get(res, "tools") : NULL;
        int i;
        if (arr && arr->type == JS_ARR && arr->n > 0) {
            m->tools = (mcp_tool *) xcalloc((size_t) arr->n, sizeof(mcp_tool));
            for (i = 0; i < arr->n; i++) {
                js_val *t = arr->items[i];
                const char *nm = js_str(t, "name", NULL);
                js_val *sch;
                if (!nm) continue;
                m->tools[m->n_tools].name = xstrdup(nm);
                m->tools[m->n_tools].desc =
                    xstrdup(js_str(t, "description", ""));
                sch = js_get(t, "inputSchema");
                if (!sch) sch = js_get(t, "input_schema");
                if (sch) {
                    strbuf b;
                    sb_init(&b);
                    js_dump(sch, &b);
                    m->tools[m->n_tools].schema = xstrdup(b.data);
                    sb_free(&b);
                } else {
                    m->tools[m->n_tools].schema =
                        xstrdup("{\"type\":\"object\",\"properties\":{}}");
                }
                m->n_tools++;
            }
        }
    }
    js_free(r);
    return m;
}

void mcp_free(mcp_client *m) {
    int i;
    if (!m) return;
    for (i = 0; i < m->n_tools; i++) {
        free(m->tools[i].name);
        free(m->tools[i].desc);
        free(m->tools[i].schema);
    }
    if (m->tools) free(m->tools);
    free(m);
}

int         mcp_tool_count(const mcp_client *m) { return m ? m->n_tools : 0; }
const char *mcp_server_name(const mcp_client *m) { return m ? m->server_name : "?"; }

const char *mcp_tool_name(const mcp_client *m, int i) {
    if (!m || i < 0 || i >= m->n_tools) return "";
    return m->tools[i].name;
}
const char *mcp_tool_desc(const mcp_client *m, int i) {
    if (!m || i < 0 || i >= m->n_tools) return "";
    return m->tools[i].desc;
}

int mcp_has_tool(const mcp_client *m, const char *name) {
    int i;
    if (!m) return 0;
    for (i = 0; i < m->n_tools; i++) {
        if (strcmp(m->tools[i].name, name) == 0) return 1;
    }
    return 0;
}

jj_val *mcp_tools_as_jinja(const mcp_client *m) {
    jj_val *l = jj_list();
    int i;
    if (!m) return l;
    for (i = 0; i < m->n_tools; i++) {
        jj_val *t = jj_dict();
        jj_val *fn = jj_dict();
        js_val *sch = js_parse(m->tools[i].schema);

        jj_dict_set(fn, "name", jj_str(m->tools[i].name));
        jj_dict_set(fn, "description", jj_str(m->tools[i].desc));
        jj_dict_set(fn, "parameters", sch ? agent_js_to_jj(sch) : jj_dict());
        js_free(sch);

        jj_dict_set(t, "type", jj_str("function"));
        jj_dict_set(t, "function", fn);
        jj_list_add(l, t);
    }
    return l;
}

/* ------------------------------------------------------------------ */
/* Tool invocation                                                     */
/* ------------------------------------------------------------------ */

int mcp_call(mcp_client *m, const char *name, const char *args_json,
             strbuf *out) {
    strbuf params;
    js_val *r;
    char err[256];
    int rc = -1;

    sb_init(out);
    if (!m) { sb_puts(out, "no MCP server"); return -1; }

    sb_init(&params);
    sb_puts(&params, "{\"name\":\"");
    sb_json_escape(&params, name, strlen(name));
    sb_puts(&params, "\",\"arguments\":");
    sb_puts(&params, (args_json && args_json[0]) ? args_json : "{}");
    sb_puts(&params, "}");

    r = rpc(m, "tools/call", params.data, 0, err, sizeof(err));
    sb_free(&params);

    if (!r) { sb_puts(out, err[0] ? err : "tool call failed"); return -1; }

    {
        js_val *e = js_get(r, "error");
        js_val *res = js_get(r, "result");
        if (e) {
            sb_puts(out, "error: ");
            sb_puts(out, js_str(e, "message", "unknown"));
        } else if (res) {
            js_val *content = js_get(res, "content");
            if (content && content->type == JS_ARR) {
                int i;
                for (i = 0; i < content->n; i++) {
                    const char *txt = js_str(content->items[i], "text", NULL);
                    if (txt) {
                        if (out->len) sb_puts(out, "\n");
                        sb_puts(out, txt);
                    }
                }
                rc = 0;
            } else {
                strbuf d;
                sb_init(&d);
                js_dump(res, &d);
                sb_add(out, d.data, d.len);
                sb_free(&d);
                rc = 0;
            }
            if (out->len == 0) sb_puts(out, "(empty result)");
        } else {
            sb_puts(out, "malformed response");
        }
    }
    js_free(r);
    return rc;
}
