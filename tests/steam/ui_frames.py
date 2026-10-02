#!/usr/bin/env python3
"""How smoothly the Steam client's own window (Chromium, steamwebhelper)
draws: frame times of a synthetic animation run inside it for a few seconds,
over Chromium's DevTools protocol (the client started with
--remote-debugging-port, e.g. through LXRT_EXEC_ARGS).

  tests/steam/ui_frames.py [PORT] [SECONDS]

It adds a layer of 300 moving, shadowed, rounded boxes to the client's main
window, measures requestAnimationFrame intervals while they move, removes
the layer, and prints one JSON line: frames, mean fps, p50/p95/p99 frame
time (ms), frames over 20 ms and over 33 ms. It reads nothing of the page
(no titles, URLs, text or pixels: the window shows the user's account).
Standard library only.
"""
import base64
import json
import os
import socket
import struct
import sys
import urllib.request

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

JS = r"""
(async () => {
  const layer = document.createElement('div');
  layer.style.cssText = 'position:fixed;inset:0;pointer-events:none;z-index:2147483647';
  const boxes = [];
  for (let i = 0; i < 300; i++) {
    const b = document.createElement('div');
    b.style.cssText = 'position:absolute;width:64px;height:64px;border-radius:12px;' +
      'background:linear-gradient(135deg,hsl(' + (i * 37 % 360) + ',70%,55%),hsl(' + (i * 53 % 360) + ',70%,35%));' +
      'box-shadow:0 8px 24px rgba(0,0,0,.45);opacity:.85;will-change:transform';
    layer.appendChild(b); boxes.push(b);
  }
  document.body.appendChild(layer);
  const W = innerWidth, H = innerHeight, t0 = performance.now(), times = [];
  await new Promise(res => {
    let last = t0;
    function f(t) {
      times.push(t - last); last = t;
      const s = (t - t0) / 1000;
      for (let i = 0; i < boxes.length; i++) {
        const x = (Math.sin(s * 1.3 + i) * 0.5 + 0.5) * (W - 64), y = (Math.cos(s * 0.9 + i * 1.7) * 0.5 + 0.5) * (H - 64);
        boxes[i].style.transform = 'translate(' + x + 'px,' + y + 'px) rotate(' + ((s * 90 + i * 7) % 360) + 'deg)';
      }
      if (t - t0 < SECONDS * 1000) requestAnimationFrame(f); else res();
    }
    requestAnimationFrame(f);
  });
  layer.remove();
  times.shift();
  const sorted = times.slice().sort((a, b) => a - b), q = p => sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))];
  const total = times.reduce((a, b) => a + b, 0);
  return JSON.stringify({frames: times.length, fps: +(1000 * times.length / total).toFixed(1),
    p50: +q(.5).toFixed(2), p95: +q(.95).toFixed(2), p99: +q(.99).toFixed(2),
    over20: times.filter(x => x > 20).length, over33: times.filter(x => x > 33.4).length,
    width: W, height: H});
})()
""".replace("SECONDS", str(SECONDS))


def ws_connect(url):
    rest = url.split("://", 1)[1]
    hostport, path = rest.split("/", 1)
    host, port = hostport.split(":")
    s = socket.create_connection((host, int(port)), timeout=60)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, hostport, key)).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += s.recv(1)
    if b" 101 " not in head.split(b"\r\n", 1)[0]:
        raise RuntimeError("websocket refused: %r" % head[:80])
    return s


def ws_send(s, text):
    data = text.encode()
    mask = os.urandom(4)
    n = len(data)
    hdr = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else
                            bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536 else
                            bytes([0x80 | 127]) + struct.pack(">Q", n))
    s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("websocket closed")
        buf += chunk
    return buf


def ws_recv(s):
    msg = b""
    while True:
        b0, b1 = recv_exact(s, 2)
        n = b1 & 0x7f
        if n == 126:
            n = struct.unpack(">H", recv_exact(s, 2))[0]
        elif n == 127:
            n = struct.unpack(">Q", recv_exact(s, 8))[0]
        msg += recv_exact(s, n)
        if b0 & 0x80:
            return msg.decode()


def evaluate(url, expression, timeout=60):
    s = ws_connect(url)
    s.settimeout(timeout)
    ws_send(s, json.dumps({"id": 1, "method": "Runtime.evaluate",
                           "params": {"expression": expression, "awaitPromise": True, "returnByValue": True}}))
    while True:
        reply = json.loads(ws_recv(s))
        if reply.get("id") == 1:
            s.close()
            return reply


def main():
    targets = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % PORT, timeout=10))
    # The client's main window: the largest visible page. Hidden pages (the
    # client keeps a dozen: menus, the shared JS context) never run
    # requestAnimationFrame. Only the size and visibility are read.
    pages = [t for t in targets if t.get("type") == "page" and t.get("webSocketDebuggerUrl")]
    main_page, best = None, 0
    for t in pages:
        try:
            v = evaluate(t["webSocketDebuggerUrl"],
                         "JSON.stringify([innerWidth * innerHeight, document.visibilityState])", 5)
            area, vis = json.loads(v["result"]["result"]["value"])
        except Exception:
            continue
        if vis == "visible" and area > best:
            main_page, best = t, area
    if not main_page:
        print(json.dumps({"error": "no visible page target"}))
        return 1
    reply = evaluate(main_page["webSocketDebuggerUrl"], JS)
    value = reply.get("result", {}).get("result", {}).get("value")
    print(value if value else json.dumps({"error": reply.get("result", {}).get("exceptionDetails", {}).get("text", "?")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
