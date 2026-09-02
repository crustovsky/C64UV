#!/usr/bin/env python3
"""Fake Ultimate REST server for integration tests.

Serves the subset of the Ultimate's /v1 API that c64uv talks to and appends
one line per request to a log file so tests can assert on what was called:

    METHOD PATH?QUERY [pw=<X-Password>] [body=<len>]

With a fourth argument it also serves the firmware's DMA socket protocol on
that TCP port (c64uv reads C64U_DMA_PORT), logging every frame as

    DMA cmd=FFxx len=<payload length>      (AUTHENTICATE logs pw=<password>)

Usage: fakeultimate.py <bind-ip> <port> <logfile> [dma-port]
"""
import http.server
import json
import socket
import sys
import threading

LEN24_CMDS = (0xFF0A, 0xFF0B, 0xFF0D)  # MOUNT_IMG, RUN_IMG, RUN_CRT


def _log_line(line):
    with open(sys.argv[3], "a") as f:
        f.write(line + "\n")


def _recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def _dma_client(conn):
    with conn:
        while True:
            hdr = _recv_exact(conn, 4)
            if not hdr:
                return
            cmd = hdr[0] | hdr[1] << 8
            n = hdr[2] | hdr[3] << 8
            if cmd in LEN24_CMDS:
                extra = _recv_exact(conn, 1)
                if extra is None:
                    return
                n |= extra[0] << 16
            payload = _recv_exact(conn, n) if n else b""
            if payload is None:
                return
            if cmd == 0xFF1F:  # AUTHENTICATE: the firmware answers one byte
                _log_line(f"DMA cmd=FF1F pw={payload.decode(errors='replace')}")
                conn.sendall(b"\x01")
            else:
                _log_line(f"DMA cmd={cmd:04X} len={n}")


def _dma_server(ip, port):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((ip, port))
    srv.listen()
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=_dma_client, args=(conn,), daemon=True).start()


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
        elif self.path == "/v1/configs/C64%20and%20Cartridge%20Settings/Cartridge":
            self._json({"C64 and Cartridge Settings": {
                        "Cartridge": {"current": "Retro Replay",
                                      "presets": ["", "Retro Replay"],
                                      "default": ""}}, "errors": []})
        elif self.path.startswith("/v1/machine:readmem"):
            self._reply(200, b"\x00", "application/octet-stream")
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
    if len(sys.argv) > 4:
        threading.Thread(target=_dma_server,
                         args=(sys.argv[1], int(sys.argv[4])),
                         daemon=True).start()
    http.server.HTTPServer((sys.argv[1], int(sys.argv[2])),
                           Handler).serve_forever()
