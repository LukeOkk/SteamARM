#!/usr/bin/env python3
"""Minimal Chrome DevTools Protocol client for the Steam webhelper (no deps).

Enable it with an empty file <Steam>/.cef-enable-remote-debugging; the
webhelper then serves DevTools on 127.0.0.1:8080.

    scripts/cdp.py list
    scripts/cdp.py eval  <title-substring> '<js expression>'
    scripts/cdp.py console <title-substring> [seconds]   # stream console + exceptions
"""
import base64, json, os, socket, struct, sys, time, urllib.request

HOST, PORT = "127.0.0.1", 8080


def targets():
    with urllib.request.urlopen(f"http://{HOST}:{PORT}/json", timeout=5) as r:
        return json.load(r)


class WS:
    def __init__(self, url):
        path = url.split(f"{PORT}", 1)[1]
        self.s = socket.create_connection((HOST, PORT), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET {path} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
                        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(4096)
        if b" 101 " not in buf.split(b"\r\n")[0]:
            raise SystemExit("handshake failed: " + buf.decode(errors="replace"))
        self.rest = buf.split(b"\r\n\r\n", 1)[1]
        self.n = 0

    def _read(self, k):
        while len(self.rest) < k:
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.rest += d
        out, self.rest = self.rest[:k], self.rest[k:]
        return out

    def recv(self):
        msg = b""
        while True:
            b0, b1 = self._read(2)
            ln = b1 & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._read(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._read(8))[0]
            msg += self._read(ln)
            if b0 & 0x80:
                return json.loads(msg)

    def send(self, method, **params):
        self.n += 1
        data = json.dumps({"id": self.n, "method": method, "params": params}).encode()
        hdr = bytes([0x81])
        mask = os.urandom(4)
        if len(data) < 126:
            hdr += bytes([0x80 | len(data)])
        elif len(data) < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", len(data))
        else:
            hdr += bytes([0x80 | 127]) + struct.pack(">Q", len(data))
        self.s.sendall(hdr + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))
        return self.n

    def call(self, method, **params):
        i = self.send(method, **params)
        while True:
            m = self.recv()
            if m.get("id") == i:
                return m


def pick(sub):
    for t in targets():
        if sub in t.get("title", "") or sub in t.get("url", ""):
            return WS(t["webSocketDebuggerUrl"])
    raise SystemExit(f"no target matching {sub!r}")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "list":
        for t in targets():
            print(t.get("type"), "|", t.get("title"), "|", t.get("url", "")[:120])
    elif cmd == "eval":
        ws = pick(sys.argv[2])
        r = ws.call("Runtime.evaluate", expression=sys.argv[3], returnByValue=True, awaitPromise=True)
        print(json.dumps(r.get("result", r), indent=1)[:20000])
    elif cmd == "console":
        ws = pick(sys.argv[2])
        secs = float(sys.argv[3]) if len(sys.argv) > 3 else 30
        ws.s.settimeout(secs)
        ws.send("Runtime.enable")
        ws.send("Log.enable")
        end = time.time() + secs
        try:
            while time.time() < end:
                m = ws.recv()
                meth = m.get("method", "")
                p = m.get("params", {})
                if meth == "Runtime.consoleAPICalled":
                    args = " ".join(str(a.get("value", a.get("description", ""))) for a in p.get("args", []))
                    print(f"console.{p.get('type')}: {args}"[:600])
                elif meth == "Runtime.exceptionThrown":
                    d = p.get("exceptionDetails", {})
                    print("EXCEPTION:", d.get("text"), d.get("exception", {}).get("description", "")[:1500])
                elif meth == "Log.entryAdded":
                    e = p.get("entry", {})
                    print(f"log.{e.get('level')}: {e.get('text')} {e.get('url', '')}"[:600])
        except (socket.timeout, EOFError):
            pass


if __name__ == "__main__":
    main()
