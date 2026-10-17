#!/usr/bin/env python3
"""A scripted Model Context Protocol server over stdio, for the MCP tests (tests/test_integration.py and the unit tests'
end-to-end checks). It prints a line that is not JSON-RPC first (a banner), asks the client a ping before it answers a
call, pages tools/list in two pages, answers one call with isError, and has a tool whose name has a dot and is longer
than 64 characters."""
import json
import sys

LONG = "tool.with.dots_" + "x" * 60
TOOLS_PAGE1 = [
    {"name": "echo", "description": "Echo the message back.",
     "inputSchema": {"type": "object", "properties": {"message": {"type": "string"}}, "required": ["message"]}},
    {"name": LONG, "description": "A long name.", "inputSchema": {"type": "object", "properties": {}}},
]
TOOLS_PAGE2 = [
    {"name": "shout", "description": "Always fails.", "inputSchema": {"type": "object", "properties": {}}},
]


def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def main():
    sys.stdout.write("fake-mcp: starting (this line is not JSON-RPC)\n")
    sys.stdout.flush()
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        msg = json.loads(raw)
        method = msg.get("method")
        mid = msg.get("id")
        if method == "notifications/initialized" or mid is None and method:
            continue   # notifications: nothing to answer
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": mid, "result": {"protocolVersion": "2024-11-05",
                  "capabilities": {"tools": {}}, "serverInfo": {"name": "fake", "version": "1"}}})
        elif method == "tools/list":
            cursor = (msg.get("params") or {}).get("cursor")
            if cursor == "p2":
                send({"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS_PAGE2}})
            else:
                send({"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS_PAGE1, "nextCursor": "p2"}})
        elif method == "tools/call":
            params = msg.get("params") or {}
            name = params.get("name")
            args = params.get("arguments") or {}
            if name == "echo":
                # ask the client something first: it must answer, and this reply must still come
                send({"jsonrpc": "2.0", "id": "srv-ping-1", "method": "ping"})
                sys.stdin.readline()
                send({"jsonrpc": "2.0", "id": mid, "result": {"content": [
                    {"type": "text", "text": "echo: " + str(args.get("message", ""))}]}})
            elif name == "shout":
                send({"jsonrpc": "2.0", "id": mid, "result": {"isError": True, "content": [
                    {"type": "text", "text": "shout failed on purpose"}]}})
            elif name == LONG:
                send({"jsonrpc": "2.0", "id": mid, "result": {"content": [{"type": "text", "text": "long ok"}]}})
            else:
                send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "no such tool: " + str(name)}})
        elif mid is not None:
            send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "unknown method " + str(method)}})


if __name__ == "__main__":
    main()
