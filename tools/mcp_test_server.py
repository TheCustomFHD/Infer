#!/usr/bin/env python3
"""A minimal MCP server over Streamable HTTP, for testing `infer chat --mcp`.

Implements just enough of the spec: initialize, notifications/initialized,
tools/list and tools/call. Two toy tools.

    python3 mcp_server.py [port]
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

TOOLS = [
    {
        "name": "get_weather",
        "description": "Get the current weather for a city",
        "inputSchema": {
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "City name"}
            },
            "required": ["city"],
        },
    },
    {
        "name": "add",
        "description": "Add two numbers",
        "inputSchema": {
            "type": "object",
            "properties": {
                "a": {"type": "number"},
                "b": {"type": "number"},
            },
            "required": ["a", "b"],
        },
    },
]


def call_tool(name, args):
    if name == "get_weather":
        city = args.get("city", "?")
        return "Weather in %s: 14 C, light rain, wind 12 km/h." % city
    if name == "add":
        try:
            return "The sum is %g." % (float(args.get("a", 0)) + float(args.get("b", 0)))
        except (TypeError, ValueError):
            return "error: arguments must be numbers"
    return "error: unknown tool %s" % name


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *a):
        sys.stderr.write("  [mcp] " + (fmt % a) + "\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n).decode("utf8")
        try:
            req = json.loads(raw)
        except ValueError:
            self.send_response(400)
            self.end_headers()
            return

        method = req.get("method", "")
        rid = req.get("id")

        if method == "initialize":
            result = {
                "protocolVersion": "2025-06-18",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "test-mcp", "version": "1.0"},
            }
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            p = req.get("params", {})
            text = call_tool(p.get("name", ""), p.get("arguments", {}) or {})
            result = {"content": [{"type": "text", "text": text}]}
        else:
            # notification: no response body
            if rid is None:
                self.send_response(202)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            result = {}

        body = json.dumps({"jsonrpc": "2.0", "id": rid, "result": result}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Mcp-Session-Id", "test-session-1")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
    print("MCP test server on http://127.0.0.1:%d/mcp" % port)
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
