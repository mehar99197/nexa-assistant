#!/usr/bin/env python3
"""Shows the web remote's page with a pretend Nexa behind it, in any browser, without a PC-phone pair.

Serves remote/web/ (and the icon) on http://127.0.0.1:PORT with the server's real security headers,
accepts any six-digit code at /api/pair, and answers the WebSocket at /ws the way Nexa does: a welcome,
a short conversation, and the core's state walking through all nine states every few seconds. A typed
request comes back as an echo. Open http://127.0.0.1:8765/#pair=123456 and the page pairs itself.

    scripts/preview-web-remote.py [--port 8765] [--once]

--once plays the conversation and the tour of states once, then holds Idle (for screenshots).
Nothing here is Nexa's code: it is a stand-in for RemoteServer, for looking at the page only.
"""
import argparse
import base64
import hashlib
import json
import os
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
WEB = os.path.join(ROOT, "remote", "web")
ICON = os.path.join(ROOT, "packaging", "org.nexa.Assistant.svg")
TYPES = {
    "index.html": "text/html; charset=utf-8",
    "app.js": "text/javascript; charset=utf-8",
    "worklet.js": "text/javascript; charset=utf-8",
    "sw.js": "text/javascript; charset=utf-8",
    "style.css": "text/css; charset=utf-8",
    "manifest.json": "application/manifest+json",
    "icon.svg": "image/svg+xml",
}
CSP = ("default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; media-src 'self' blob:; "
       "connect-src 'self'; worker-src 'self'; manifest-src 'self'; base-uri 'none'; form-action 'none'; "
       "frame-ancestors 'none'")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MODES = ["idle", "wake", "listen", "stt", "think", "act", "speak", "fast", "offline"]
ONCE = False


def frame(payload: bytes, opcode: int = 1) -> bytes:
    head = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack(">H", n)
    else:
        head += bytes([127]) + struct.pack(">Q", n)
    return head + payload


def read_frame(rfile):
    """One frame from the browser (always masked): (opcode, payload) or None when closed."""
    head = rfile.read(2)
    if len(head) < 2:
        return None
    opcode = head[0] & 0x0F
    length = head[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", rfile.read(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", rfile.read(8))[0]
    mask = rfile.read(4) if head[1] & 0x80 else b""
    data = rfile.read(length)
    if mask:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return opcode, data


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet
        return

    def send_headers(self, status, ctype, length):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        self.send_header("Content-Security-Policy", CSP)
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?")[0].split("#")[0]
        if path == "/ws":
            self.websocket()
            return
        name = "index.html" if path == "/" else path.lstrip("/")
        if name not in TYPES:
            self.send_headers(404, "text/plain", 0)
            return
        file = ICON if name == "icon.svg" else os.path.join(WEB, name)
        with open(file, "rb") as f:
            body = f.read()
        self.send_headers(200, TYPES[name], len(body))
        self.wfile.write(body)

    def do_POST(self):
        if self.path != "/api/pair":
            self.send_headers(404, "text/plain", 0)
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            body = {}
        code = str(body.get("code", ""))
        ok = len(code) == 6 and code.isdigit()
        reply = json.dumps({"token": "preview-token"} if ok else {"error": "The code has six digits."}).encode()
        self.send_headers(200 if ok else 400, "application/json", len(reply))
        self.wfile.write(reply)

    # ---- the WebSocket: what RemoteServer would say

    def websocket(self):
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        lock = threading.Lock()
        alive = True

        def send(message: dict):
            with lock:
                try:
                    self.wfile.write(frame(json.dumps(message).encode()))
                    self.wfile.flush()
                except OSError:
                    pass

        def conversation():
            pause = 0.0 if ONCE else 0.8     # --once: everything at once, for a screenshot
            time.sleep(pause)
            send({"t": "accepted", "text": "open the text editor and tell me the volume", "source": "phone-voice"})
            send({"t": "state", "mode": "think", "nodes": []})
            time.sleep(pause)
            send({"t": "state", "mode": "act", "nodes": ["pc", "tools"]})
            send({"t": "step", "text": "open_app — Opened Text Editor · 6 ms", "ok": True})
            time.sleep(pause)
            send({"t": "step", "text": "get_volume — Volume 100% · 18 ms", "ok": True})
            send({"t": "state", "mode": "think", "nodes": []})
            for piece in ["Text Editor is open. ", "The volume is at 100%."]:
                time.sleep(pause / 2)
                send({"t": "delta", "text": piece})
            send({"t": "finished", "reason": "stop"})
            send({"t": "state", "mode": "idle", "nodes": []})
            while alive:
                for mode in MODES:
                    if not alive:
                        return
                    time.sleep(3.0)
                    nodes = ["pc", "tools"] if mode in ("act", "fast") else []
                    send({"t": "state", "mode": mode, "nodes": nodes})
                if ONCE:
                    send({"t": "state", "mode": "idle", "nodes": []})
                    return

        tour = None
        try:
            while True:
                got = read_frame(self.rfile)
                if got is None:
                    break
                opcode, data = got
                if opcode == 8:
                    break
                if opcode == 9:
                    with lock:
                        self.wfile.write(frame(data, 10))
                    continue
                if opcode == 2:
                    send({"t": "transcript", "text": "what time is it"})
                    send({"t": "accepted", "text": "what time is it", "source": "phone-voice"})
                    send({"t": "delta", "text": "It is " + time.strftime("%H:%M") + "."})
                    send({"t": "finished", "reason": "stop"})
                    continue
                try:
                    message = json.loads(data.decode())
                except ValueError:
                    continue
                kind = message.get("t")
                if kind == "hello":
                    send({"t": "welcome", "version": 1, "device": "Preview phone", "voice": True, "busy": False,
                          "mode": "idle"})
                    tour = threading.Thread(target=conversation, daemon=True)
                    tour.start()
                elif kind == "ping":
                    send({"t": "pong"})
                elif kind == "text":
                    send({"t": "accepted", "text": message.get("text", ""), "source": "phone"})
                    send({"t": "state", "mode": "think", "nodes": []})
                    send({"t": "busy", "busy": True})
                    time.sleep(0.6)
                    send({"t": "delta", "text": "You said: " + message.get("text", "")})
                    send({"t": "finished", "reason": "stop"})
                    send({"t": "busy", "busy": False})
                    send({"t": "state", "mode": "idle", "nodes": []})
                elif kind == "cancel":
                    send({"t": "finished", "reason": "cancelled"})
                    send({"t": "busy", "busy": False})
        finally:
            alive = False


def main():
    global ONCE
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--once", action="store_true", help="play the conversation and the tour once, then hold Idle")
    args = parser.parse_args()
    ONCE = args.once
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    print(f"Open http://127.0.0.1:{args.port}/#pair=123456 in a browser (Ctrl+C stops this).", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
