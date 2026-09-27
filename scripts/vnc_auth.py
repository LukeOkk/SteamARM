"""VNC security handshake shared by scripts/vnc_snapshot.py and vnc_click.py:
None when offered, else classic VncAuth with the password from
$STEAMARM_VNC_PASSWORD or ~/SteamARM-roots/vncpasswd.txt (DES via openssl)."""
import os, struct, subprocess


def _recvn(s, n):
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            raise EOFError
        b += d
    return b


def _password():
    pw = os.environ.get("STEAMARM_VNC_PASSWORD")
    if not pw:
        p = os.path.expanduser(os.environ.get("STEAMARM_STATE", "~/SteamARM-roots") + "/vncpasswd.txt")
        with open(p) as f:
            pw = f.read().strip()
    return pw


def handshake(s):
    _recvn(s, 12)
    s.sendall(b"RFB 003.008\n")
    n = _recvn(s, 1)[0]
    types = _recvn(s, n)
    if 1 in types:
        s.sendall(b"\x01")
    elif 2 in types:
        s.sendall(b"\x02")
        challenge = _recvn(s, 16)
        key = bytes(int(f"{c:08b}"[::-1], 2) for c in _password().encode()[:8].ljust(8, b"\0"))
        resp = subprocess.run(["openssl", "enc", "-des-ecb", "-K", key.hex(), "-nopad", "-nosalt"],
                              input=challenge, capture_output=True, check=True).stdout
        s.sendall(resp[:16])
    else:
        raise SystemExit(f"unsupported VNC security types {list(types)}")
    if struct.unpack(">I", _recvn(s, 4))[0] != 0:
        raise SystemExit("VNC authentication failed")
