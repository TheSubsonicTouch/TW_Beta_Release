"""Local service emulator for Triad Wars (client build 1.6.0.0).

The client talks to https://<Environment>/<service>/api/v1/<method>, where
<Environment> comes from UserOptions.dat. With Environment=localhost this
server answers those requests so the game can run without the original
backend. Standard library only.

    python server.py [--port 443] [--host 127.0.0.1]
"""
import argparse
import base64
import datetime
import hashlib
import importlib
import json
import os
import socket
import ssl
import struct
import sys
import threading
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import handlers  # noqa: E402
import state  # noqa: E402

_reload_lock = threading.Lock()
_mtimes = {}


def maybe_reload():
    """Pick up edits to handlers.py without restarting (state.json is kept as-is on disk)."""
    with _reload_lock:
        changed = False
        for mod in (state, handlers):
            try:
                m = os.path.getmtime(mod.__file__)
            except OSError:
                continue
            if _mtimes.setdefault(mod.__file__, m) != m:
                _mtimes[mod.__file__] = m
                changed = True
        if changed:
            try:
                importlib.reload(state)
                importlib.reload(handlers)
                log("** reloaded handlers")
            except Exception:
                log("!! reload failed: " + traceback.format_exc())


LOG_DIR = os.path.join(HERE, "logs")
SESSION_ID = "s%3Alocal-triadwars-session"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

_log_lock = threading.Lock()
_log_file = None
_started_at = ""


def _mtime(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return 0


def log(msg):
    line = "%s %s" % (datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3], msg)
    with _log_lock:
        print(line, flush=True)
        if _log_file:
            _log_file.write(line + "\n")
            _log_file.flush()


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        # The client drops keep-alive connections abruptly; that is not an error.
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionError, ssl.SSLError, TimeoutError)):
            return
        log("!! handler error from %s: %r" % (client_address, exc))
        log(traceback.format_exc())


class Server6(Server):
    """The same server on ::, also accepting IPv4 connections.

    The client resolves "localhost", and Windows answers ::1 at least as often as
    127.0.0.1. An IPv4-only listener refuses those attempts, and a client that cannot
    reach the backend at startup just exits - it looks exactly like a crash on launch.
    IPV6_V6ONLY=0 makes one socket answer both families.
    """
    address_family = socket.AF_INET6

    def server_bind(self):
        try:
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        except OSError:
            pass
        super().server_bind()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "TriadWarsLocal/0.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, status, body=b"", ctype="application/json; charset=utf-8", headers=()):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _handle(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        url = urlsplit(self.path)
        if self.headers.get("Upgrade", "").lower() == "websocket":
            return self._websocket(url)

        segs = [s for s in url.path.split("/") if s]
        service, parts = "", segs
        if len(segs) >= 3 and segs[1] == "api":
            service, parts = segs[0], segs[3:]

        log("%s %s%s" % (self.command, self.path,
                         ("  body=" + body[:2000].decode("utf-8", "replace")) if body else ""))
        log("    headers: " + "; ".join("%s=%s" % kv for kv in self.headers.items()
                                          if kv[0].lower() not in ("host", "accept")))

        # session endpoints, regardless of which service prefix they sit under
        tail = "/".join(parts)
        if tail == "__local/info":
            # Used by launch.py: says *which* build is answering, so a server left over
            # from an older zip can be replaced instead of being played against.
            return self._send(200, {
                "pid": os.getpid(),
                "started": _started_at,
                "port": self.server.server_address[1],
                "python": sys.version.split()[0],
                "log": os.path.basename(getattr(_log_file, "name", "")),
                "code": {"server": _mtime(os.path.join(HERE, "server.py")),
                         "state": _mtime(getattr(state, "__file__", "")),
                         "handlers": _mtime(getattr(handlers, "__file__", ""))},
                "features": {"jobs": state.FEATURE_JOBS,
                             "challenges": state.FEATURE_CHALLENGES,
                             "exports": state.FEATURE_EXPORTS,
                             "production": state.FEATURE_PRODUCTION},
            })
        if tail == "acceptcredentials":
            return self._send(200, {}, headers=[
                ("Set-Cookie", "connect.sid=%s; Path=/; HttpOnly" % SESSION_ID)])
        if tail == "heartbeat":
            return self._send(200, {})
        if tail.startswith("logout"):
            return self._send(200, {})

        maybe_reload()
        fn = handlers.dispatch(service, parts)
        if fn is None:
            log("    !! UNHANDLED %s %s/%s" % (self.command, service, tail))
            return self._send(200, {"mReturnCode": 0, "mMessage": ""})
        # A handler that raises used to drop the connection, and the client can treat
        # that as a fatal error in the middle of a screen transition. Answer with the
        # neutral reply the client already handles for endpoints we do not implement.
        try:
            result = fn(self, self.command, parts, body, parse_qs(url.query))
        except Exception:
            log("    !! handler error for %s/%s" % (service, tail))
            log(traceback.format_exc())
            return self._send(200, {"mReturnCode": 0, "mMessage": ""})
        try:
            log("    -> %s" % json.dumps(result)[:300])
        except (TypeError, ValueError):
            log("    -> <reply that cannot be serialised>")
            return self._send(200, {"mReturnCode": 0, "mMessage": ""})
        status = 200
        if isinstance(result, tuple):
            status, result = result
        return self._send(status, result)

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = _handle

    # -- WebSocket (messaging channel) ------------------------------------
    def _websocket(self, url):
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        log("WEBSOCKET %s key=%s" % (self.path, key))
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True
        try:
            while True:
                hdr = self.rfile.read(2)
                if len(hdr) < 2:
                    break
                opcode = hdr[0] & 0x0F
                masked = hdr[1] & 0x80
                n = hdr[1] & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self.rfile.read(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self.rfile.read(8))[0]
                mask = self.rfile.read(4) if masked else b"\0\0\0\0"
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(self.rfile.read(n)))
                log("    ws frame op=%d len=%d %r" % (opcode, n, payload[:300]))
                if opcode == 8:
                    self.wfile.write(b"\x88\x00")
                    break
                if opcode == 9:
                    self.wfile.write(bytes([0x8A, len(payload)]) + payload)
        except (ConnectionError, OSError):
            pass
        log("WEBSOCKET closed")


def main():
    global _log_file, _started_at
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="auto",
                    help="'auto' listens on IPv4 and IPv6; or give an address to bind")
    ap.add_argument("--port", type=int, default=443)
    args = ap.parse_args()
    _started_at = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    os.makedirs(LOG_DIR, exist_ok=True)
    _log_file = open(os.path.join(
        LOG_DIR, datetime.datetime.now().strftime("server-%Y%m%d-%H%M%S.log")), "w", encoding="utf-8")

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    # The 2015 client ships OpenSSL 1.0.1j, which negotiates TLS 1.2.
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    try:
        ctx.set_ciphers("DEFAULT:@SECLEVEL=0")
    except ssl.SSLError:
        pass
    ctx.load_cert_chain(os.path.join(HERE, "cert.pem"), os.path.join(HERE, "key.pem"))

    if args.host == "auto":
        try:
            httpd = Server6(("::", args.port), Handler)
            where = "::"
        except OSError as e:
            log("!! could not listen on :: (%s); falling back to IPv4 only" % e)
            httpd = Server(("127.0.0.1", args.port), Handler)
            where = "127.0.0.1"
    else:
        family = Server6 if ":" in args.host else Server
        httpd = family((args.host, args.port), Handler)
        where = args.host
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    shown = "[%s]" % where if ":" in where else where
    log("Triad Wars local server listening on https://%s:%d (pid %d)"
        % (shown, args.port, os.getpid()))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
