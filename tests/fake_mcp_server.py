#!/usr/bin/env python3
"""A pretend MCP server on stdio, for tests/McpClientTest.cc: initialize, tools/list in two pages,
tools/call for echo, add and fail, a ping of its own when FAKE_MCP_PING=1, and its version from
FAKE_MCP_VERSION so the test can see that the environment reached it."""
import json
import os
import sys

TOOLS = [
    {"name": "echo", "description": "Says it back",
     "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}},
    {"name": "add", "description": "Adds two numbers",
     "inputSchema": {"type": "object", "properties": {"a": {"type": "number"}, "b": {"type": "number"}}}},
    {"name": "fail", "description": "Always fails", "inputSchema": {"type": "object"}},
]


def send(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def reply(mid, result):
    send({"jsonrpc": "2.0", "id": mid, "result": result})


def error(mid, code, message):
    send({"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}})


for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    message = json.loads(line)
    method = message.get("method")
    mid = message.get("id")
    params = message.get("params") or {}
    if method == "initialize":
        reply(mid, {"protocolVersion": params.get("protocolVersion", "2025-06-18"),
                    "capabilities": {"tools": {"listChanged": True}},
                    "serverInfo": {"name": "fake-mcp", "version": os.environ.get("FAKE_MCP_VERSION", "1.0")}})
    elif method == "notifications/initialized":
        if os.environ.get("FAKE_MCP_PING") == "1":
            send({"jsonrpc": "2.0", "id": 999, "method": "ping"})
    elif method == "tools/list":
        if params.get("cursor") == "page2":
            reply(mid, {"tools": TOOLS[1:]})
        else:
            reply(mid, {"tools": TOOLS[:1], "nextCursor": "page2"})
    elif method == "tools/call":
        name = params.get("name")
        arguments = params.get("arguments") or {}
        if name == "echo":
            reply(mid, {"content": [{"type": "text", "text": "echo: " + str(arguments.get("text", ""))}]})
        elif name == "add":
            reply(mid, {"content": [{"type": "text", "text": str(arguments.get("a", 0) + arguments.get("b", 0))}]})
        elif name == "fail":
            reply(mid, {"isError": True, "content": [{"type": "text", "text": "boom"}]})
        elif name == "quit":
            sys.exit(3)
        else:
            error(mid, -32602, "Unknown tool: " + str(name))
    elif mid is not None and ("result" in message or "error" in message):
        pass    # the client answered our ping
    elif mid is not None:
        error(mid, -32601, "Method not found")
