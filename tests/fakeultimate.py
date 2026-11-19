#!/usr/bin/env python3
"""Fake Ultimate REST server for integration tests.

Serves the subset of the Ultimate's /v1 API that c64uv talks to and appends
one line per request to a log file so tests can assert on what was called:

    METHOD PATH?QUERY [pw=<X-Password>] [body=<len>]

With a fourth argument it also serves the firmware's DMA socket protocol on
that TCP port (c64uv reads C64U_DMA_PORT), logging every frame as

    DMA cmd=FFxx len=<payload length>      (AUTHENTICATE logs pw=<password>)

With a fifth argument it also serves a minimal passive-mode FTP server on
that port (c64uv reads C64U_FTP_PORT), logging every upload as

    FTP STOR <path> len=<bytes>

Usage: fakeultimate.py <bind-ip> <port> <logfile> [dma-port [ftp-port]]
"""
import http.server
import json
import socket
import sys
import threading

SWAPPER_PATH = "/v1/configs/U64%20Specific%20Settings/Joystick%20Swapper"
# Settings a test can change with a PUT ...?value=, like on the machine.
# "Normal" (the machine's default) keeps the API's port numbers equal
# to the C64's, which is what most tests want to read in the log.
# "mouse": whether machine:input lists a mouse (1.2.1 does, the first
# firmware with the input API does not); PUT /test/mouse?value=0 clears it.
STATE = {"swapper": "Normal", "menu": False, "row": 0, "mouse": True}

MENU_ITEMS = ["Disk File Browser", "Assembly 64 Query", "Reset C64",
              "Power OFF"]


def _menu_screen():
    """The menu the way machine:menu_screen returns it: 40x25 character
    bytes (ASCII, frame shapes below 0x20, bit 7 on the selected row),
    then 40x25 colour bytes."""
    chars = bytearray(b" " * 1000)
    colours = bytearray(b"\x0f" * 1000)

    def put(row, col, text):
        chars[row * 40 + col:row * 40 + col + len(text)] = text

    put(0, 0, b"\x01" + b"\x02" * 38 + b"\x03")
    for row in range(1, 24):
        put(row, 0, b"\x04")
        put(row, 39, b"\x04")
    put(24, 0, b"\x05" + b"\x02" * 38 + b"\x06")
    put(1, 2, bytes(c | 0x80 for c in b"fakeultimate menu"))  # a heading
    for i, item in enumerate(MENU_ITEMS):
        put(3 + i, 2, item.encode())
        if i == STATE["row"]:
            for col in range(1, 39):
                chars[(3 + i) * 40 + col] |= 0x80
                colours[(3 + i) * 40 + col] = 0xE1
    return bytes(chars + colours)


def _menu_input(body):
    """Keys reach the menu while it is open, like on firmware 1.2+:
    cursor down/up moves the selection, RUN/STOP leaves the menu."""
    try:
        events = json.loads(body).get("events", [])
    except ValueError:
        return
    for ev in events:
        if not STATE["menu"] or ev.get("kind") != "keyboard" or \
                ev.get("transition") != "press":
            continue
        keys = ev.get("inputs", [])
        if "run_stop" in keys:
            STATE["menu"] = False
        elif "cursor_up_down" in keys:
            step = -1 if "right_shift" in keys else 1
            STATE["row"] = (STATE["row"] + step) % len(MENU_ITEMS)


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


def _dma_client(conn, _ip):
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


def _listen(ip, port):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((ip, port))
    srv.listen()
    return srv


def _serve(ip, port, handler):
    srv = _listen(ip, port)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handler, args=(conn, ip), daemon=True).start()


def _ftp_client(conn, ip):
    """Just enough FTP for a libcurl anonymous upload: login, CWD, passive
    data connection (EPSV or PASV), STOR."""
    cwd = "/"
    data_srv = None
    with conn, conn.makefile("rwb", buffering=0) as f:
        f.write(b"220 fakeultimate\r\n")
        while True:
            line = f.readline()
            if not line:
                return
            cmd, _, arg = line.decode(errors="replace").strip().partition(" ")
            cmd = cmd.upper()
            if cmd in ("USER", ):
                f.write(b"331 ok\r\n")
            elif cmd in ("PASS", ):
                f.write(b"230 ok\r\n")
            elif cmd == "PWD":
                f.write(f'257 "{cwd}"\r\n'.encode())
            elif cmd == "CWD":
                cwd = arg if arg.startswith("/") else cwd.rstrip("/") + "/" + arg
                f.write(b"250 ok\r\n")
            elif cmd in ("EPSV", "PASV"):
                data_srv = _listen(ip, 0)
                port = data_srv.getsockname()[1]
                if cmd == "EPSV":
                    f.write(f"229 ok (|||{port}|)\r\n".encode())
                else:
                    f.write(f"227 ok ({ip.replace('.', ',')},{port >> 8},"
                            f"{port & 255})\r\n".encode())
            elif cmd == "STOR":
                f.write(b"150 ok\r\n")
                data, _ = data_srv.accept()
                data_srv.close()
                n = 0
                chunk = data.recv(65536)
                while chunk:
                    n += len(chunk)
                    chunk = data.recv(65536)
                data.close()
                _log_line(f"FTP STOR {cwd.rstrip('/')}/{arg} len={n}")
                f.write(b"226 ok\r\n")
            elif cmd == "QUIT":
                f.write(b"221 bye\r\n")
                return
            else:
                f.write(b"200 ok\r\n")  # TYPE and friends


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
            if self.path == "/v1/machine:input":  # small JSON: log it whole
                line += " " + body.decode("ascii", "replace")
        with open(sys.argv[3], "a") as f:
            f.write(line + "\n")

    def _reply(self, code, payload=b"", ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _json(self, obj, code=200):
        # spaced like the firmware's own output ("key" : value)
        self._reply(code, json.dumps(obj, separators=(", ", " : ")).encode())

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
        elif self.path == SWAPPER_PATH:
            self._json({"U64 Specific Settings": {
                        "Joystick Swapper": {"current": STATE["swapper"],
                                             "values": ["Normal", "Swapped",
                                                        "WASD Port 2",
                                                        "WASD Port 1"],
                                             "default": "Normal"}},
                        "errors": []})
        elif self.path == "/v1/configs/User%20Interface%20Settings/Color%20Scheme":
            self._json({"User Interface Settings": {
                        "Color Scheme": {"current": "Commodore Blue",
                                         "values": ["Commodore Blue",
                                                    "Ultimate Black"],
                                         "default": "Commodore Blue"}},
                        "errors": []})
        elif self.path.startswith("/v1/machine:readmem?address=0400"):
            # a text screen: READY. on row 0 (screen codes), blanks elsewhere
            screen = bytearray(b"\x20" * 1000)
            screen[0:6] = bytes(ord(c) - 64 for c in "READY") + b"\x2e"
            self._reply(200, bytes(screen), "application/octet-stream")
        elif self.path.startswith("/v1/machine:readmem"):
            self._reply(200, b"\x00", "application/octet-stream")
        elif self.path == "/v1/machine:input":
            state = {"keyboard": {"inputs": []},
                     "joysticks": [{"port": 1, "inputs": []},
                                   {"port": 2, "inputs": []}]}
            if STATE["mouse"]:
                state["mouse"] = {"attached": False, "inputs": [],
                                  "pending": 0}
            state["errors"] = []
            self._json(state)
        elif self.path == "/v1/machine:menu_screen":
            if STATE["menu"]:
                self._reply(200, _menu_screen(), "application/octet-stream")
            else:
                self._json({"errors": ["Menu screen unavailable."]}, 404)
        else:
            self._json({"errors": ["Unknown API Call"]}, 404)

    def do_PUT(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        self._log(body)
        if self.path.startswith(SWAPPER_PATH + "?value="):
            STATE["swapper"] = self.path.split("=", 1)[1]
        elif self.path.startswith("/test/mouse?value="):
            STATE["mouse"] = self.path.endswith("1")
        elif self.path == "/v1/machine:menu_button":
            STATE["menu"] = not STATE["menu"]
            STATE["row"] = 0
        elif self.path == "/v1/machine:input":
            _menu_input(body)
        self._json({"errors": []})

    do_POST = do_PUT


if __name__ == "__main__":
    for i, handler in ((4, _dma_client), (5, _ftp_client)):
        if len(sys.argv) > i:
            threading.Thread(target=_serve,
                             args=(sys.argv[1], int(sys.argv[i]), handler),
                             daemon=True).start()
    http.server.HTTPServer((sys.argv[1], int(sys.argv[2])),
                           Handler).serve_forever()
