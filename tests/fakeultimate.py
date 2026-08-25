#!/usr/bin/env python3
"""Fake Ultimate REST server for integration tests.

Serves the subset of the Ultimate's /v1 API that c64uv talks to and appends
one line per request to a log file so tests can assert on what was called:

    METHOD PATH?QUERY [pw=<X-Password>] [body=<len>]

Usage: fakeultimate.py <bind-ip> <port> <logfile>
"""
import http.server
import json
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):  # silence stderr chatter
        pass

    def _log(self, body=b""):
        line = f"{self.command} {self.path}"
        pw = self.headers.get("X-Password")
        if pw:
            line += f" pw={pw}"
        if body:
            line += f" body={len(body)}"
        with open(sys.argv[3], "a") as f:
            f.write(line + "\n")

    def _reply(self, code, payload=b"", ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _json(self, obj, code=200):
        self._reply(code, json.dumps(obj).encode())

    def do_GET(self):
        self._log()
        if self.path == "/v1/info":
            self._json({"product": "Ultimate 64", "firmware_version": "3.12",
                        "fpga_version": "11F", "hostname": "fakeultimate",
                        "unique_id": "F00F00", "errors": []})
        elif self.path == "/v1/machine:input":
            self._json({"keyboard": {"inputs": []},
                        "joysticks": [{"port": 1, "inputs": []},
                                      {"port": 2, "inputs": []}],
                        "errors": []})
        else:
            self._json({"errors": ["Unknown API Call"]}, 404)

    def do_PUT(self):
        n = int(self.headers.get("Content-Length") or 0)
        self._log(self.rfile.read(n) if n else b"")
        self._json({"errors": []})

    do_POST = do_PUT


if __name__ == "__main__":
    http.server.HTTPServer((sys.argv[1], int(sys.argv[2])),
                           Handler).serve_forever()
