#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Lucas Holt. All rights reserved.
"""Tiny HTTP server used by tests/18_xmlhttp.vbs (mscript test suite).

Usage: httpserver.py [port]   (default 18099, binds 127.0.0.1)
Endpoints: /ok /xml /missing /redir /latin1 /chunked  (GET),  /echo (POST)
"""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _send(self, code, body, ctype="text/plain; charset=utf-8", extra=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/ok":
            self._send(200, "hello-body", extra={"X-Custom": "yes"})
        elif self.path == "/xml":
            self._send(200, "<root><a>1</a><b>two</b></root>", "text/xml")
        elif self.path == "/missing":
            self._send(404, "nope")
        elif self.path == "/redir":
            self.send_response(302)
            self.send_header("Location", "/ok")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/redir-cross-origin":
            self.send_response(302)
            self.send_header("Location", "http://localhost:%d/auth" % self.server.server_port)
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/redir-to-auth":
            self.send_response(302)
            self.send_header("Location", "/auth")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/auth":
            self._send(200, "auth=%s cookie=%s" % (
                self.headers.get("Authorization") or "-",
                self.headers.get("Cookie") or "-"))
        elif self.path == "/latin1":
            self._send(200, "caf\xe9".encode("iso-8859-1"), "text/plain; charset=iso-8859-1")
        elif self.path == "/chunked":
            body = b"chunked-body"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"%x\r\n%s\r\n0\r\n\r\n" % (len(body), body))
        else:
            self._send(404, "unknown")

    def do_POST(self):
        if self.path == "/echo":
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n).decode("utf-8", "replace")
            out = "ct=%s x-test=%s ua=%s body=%s" % (
                self.headers.get("Content-Type") or "-",
                self.headers.get("X-Test") or "-",
                self.headers.get("User-Agent") or "-",
                body)
            self._send(200, out)
        else:
            self._send(404, "unknown")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18099
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
