#!/usr/bin/env python3
"""netd's DNS proxy for an Android root under lxrun, answered by the Mac's
resolver (docs/ANDROID_RUNTIME_ARCHITECTURE.md, "Network").

Every DNS lookup of an Android app goes to netd: bionic's getaddrinfo,
gethostbyname and gethostbyaddr, and the NDK/Java DnsResolver's raw queries
(libnetd_client's resnsend), are sent as text commands to the
/dev/socket/dnsproxyd socket (the strings are in the image's libc.so and
libnetd_client.so). netd cannot run on a Mac (scripts/android-boot.py, its
stand-in), so with no dnsproxyd every lookup failed at once. This serves that
socket from the host, with the replies laid out as netd's DnsProxyListener
and SocketClient write them: a 4-byte code ("222\\0" for a result, "401\\0" and
a length-prefixed error for a failure), then big-endian 32-bit fields, Linux
AF_* numbers and Linux sockaddr layouts. The network id in each command is
ignored: there is one network, the Mac's.

  scripts/android_dnsproxy.py <root>     serve <root>/dev/socket/dnsproxyd
  (scripts/android-boot.py starts it in-process: serve(root, log))
"""
import base64
import os
import socket
import struct
import sys
import threading

OK = b"222\0"                 # ResponseCode::DnsProxyQueryResult
FAILED = b"401\0"             # ResponseCode::DnsProxyOperationFailed
LINUX_AF_INET, LINUX_AF_INET6 = 2, 10
FROM_LINUX_AF = {0: socket.AF_UNSPEC, LINUX_AF_INET: socket.AF_INET, LINUX_AF_INET6: socket.AF_INET6}
TO_LINUX_AF = {socket.AF_INET: LINUX_AF_INET, socket.AF_INET6: LINUX_AF_INET6}
# bionic's AI_* bits that mean the same on Darwin (AI_NUMERICSERV differs:
# 0x400 on bionic/Linux, 0x1000 on Darwin).
AI_KEEP = socket.AI_PASSIVE | socket.AI_CANONNAME | socket.AI_NUMERICHOST
LINUX_AI_NUMERICSERV = 0x400
EAI_NODATA, EAI_SERVICE = 7, 9        # bionic's numbers are BSD's, as Darwin's
HOST_NOT_FOUND, NO_RECOVERY = 1, 3


def be32(v):
    return struct.pack(">i", v)


def len_and_data(b):
    return be32(len(b)) + b


def binary_msg(code, payload):
    # SocketClient::sendBinaryMsg: the code, then the length, then the bytes.
    return code + struct.pack(">I", len(payload)) + payload


def linux_sockaddr(family, sa):
    if family == socket.AF_INET:
        return struct.pack("<H", LINUX_AF_INET) + struct.pack(">H", sa[1]) + socket.inet_aton(sa[0]) + b"\0" * 8
    host = sa[0].split("%", 1)[0]
    return (struct.pack("<H", LINUX_AF_INET6) + struct.pack(">HI", sa[1], sa[2]) +
            socket.inet_pton(socket.AF_INET6, host) + struct.pack("<I", sa[3]))


def arg(s):
    return None if s == "^" else s


def getaddrinfo(args):
    host, serv = arg(args[0]), arg(args[1])
    flags, family, socktype, proto = (int(x) for x in args[2:6])
    dflags = 0
    if flags != -1:
        dflags = flags & AI_KEEP
        if flags & LINUX_AI_NUMERICSERV:
            dflags |= socket.AI_NUMERICSERV
    # Only the families the Mac has an address in, as Android's resolver
    # does for an unnumbered name (a numeric one is taken as it is).
    if host and not dflags & socket.AI_NUMERICHOST:
        dflags |= socket.AI_ADDRCONFIG
    fam = FROM_LINUX_AF.get(family if family != -1 else 0)
    if fam is None:
        return binary_msg(FAILED, struct.pack("<i", 5))       # EAI_FAMILY
    try:
        res = socket.getaddrinfo(host, serv, fam, max(socktype, 0), max(proto, 0), dflags)
    except socket.gaierror as e:
        return binary_msg(FAILED, struct.pack("<i", e.errno or EAI_NODATA))
    except (UnicodeError, OSError):
        return binary_msg(FAILED, struct.pack("<i", EAI_NODATA))
    out = [OK]
    first = True
    for fa, st, pr, canon, sa in res:
        if fa not in TO_LINUX_AF:
            continue
        out.append(be32(1) + be32(max(flags, 0)) + be32(TO_LINUX_AF[fa]) + be32(st) + be32(pr))
        out.append(len_and_data(linux_sockaddr(fa, sa)))
        c = (canon.encode() + b"\0") if (canon and first) else b""
        out.append(len_and_data(c))
        first = False
    out.append(be32(0))
    return b"".join(out)


def hostent(name, aliases, family, addrs):
    out = [OK, len_and_data(name.encode() + b"\0")]
    out += [len_and_data(a.encode() + b"\0") for a in aliases]
    out.append(be32(0))
    size = 4 if family == socket.AF_INET else 16
    out.append(struct.pack(">I", TO_LINUX_AF[family]) + struct.pack(">I", size))
    # netd sends 16 bytes per address whatever its size (sendhostent).
    out += [len_and_data(socket.inet_pton(family, a).ljust(16, b"\0")) for a in addrs]
    out.append(be32(0))
    return b"".join(out)


def gethostbyname(args):
    name, af = args[1], int(args[2])
    fam = FROM_LINUX_AF.get(af)
    if fam in (None, socket.AF_UNSPEC):
        fam = socket.AF_INET
    try:
        res = socket.getaddrinfo(name, None, fam, socket.SOCK_STREAM, 0, socket.AI_CANONNAME)
    except (socket.gaierror, UnicodeError, OSError):
        return binary_msg(FAILED, struct.pack("<i", HOST_NOT_FOUND))
    addrs = []
    for _, _, _, _, sa in res:
        a = sa[0].split("%", 1)[0]
        if a not in addrs:
            addrs.append(a)
    canon = (res[0][3] if res and res[0][3] else name)
    return hostent(canon, [], fam, addrs)


def gethostbyaddr(args):
    addr, af = args[0], int(args[2])
    fam = FROM_LINUX_AF.get(af, socket.AF_INET)
    try:
        name, aliases, addrs = socket.gethostbyaddr(addr)
    except (socket.herror, socket.gaierror, OSError):
        return binary_msg(FAILED, struct.pack("<i", HOST_NOT_FOUND))
    return hostent(name, aliases, fam, [addr])


def nameservers():
    out = []
    try:
        with open("/etc/resolv.conf") as f:
            for line in f:
                p = line.split()
                if len(p) >= 2 and p[0] == "nameserver":
                    out.append(p[1])
    except OSError:
        pass
    return out or ["1.1.1.1"]


def resnsend(args):
    # "resnsend <flags> <netid> <base64 query>": the query goes to the Mac's
    # first name server as is; the reply is the rcode, then the answer's
    # length and bytes (or a negative errno), big-endian (libnetd_client's
    # resNetworkResult).
    try:
        query = base64.b64decode(args[2])
    except (IndexError, ValueError):
        return be32(0) + be32(-22)                      # -EINVAL
    for ns in nameservers():
        fam = socket.AF_INET6 if ":" in ns else socket.AF_INET
        s = socket.socket(fam, socket.SOCK_DGRAM)
        s.settimeout(5)
        try:
            s.sendto(query, (ns.split("%", 1)[0], 53))
            ans = s.recv(65535)
        except OSError:
            continue
        finally:
            s.close()
        rcode = ans[3] & 0x0F if len(ans) > 3 else 2
        return be32(rcode) + len_and_data(ans)
    return be32(0) + be32(-110)                         # -ETIMEDOUT


HANDLERS = {"getaddrinfo": getaddrinfo, "gethostbyname": gethostbyname,
            "gethostbyaddr": gethostbyaddr, "resnsend": resnsend}


def handle(conn, log, counts):
    try:
        conn.settimeout(30)
        buf = b""
        while b"\0" not in buf and len(buf) < 65536:
            b = conn.recv(4096)
            if not b:
                return
            buf += b
        words = buf.split(b"\0", 1)[0].decode(errors="replace").split(" ")
        cmd, args = words[0], words[1:]
        h = HANDLERS.get(cmd)
        if h is None:
            if counts.setdefault("unknown:" + cmd, 0) == 0:
                log("dnsproxyd: unknown command %r" % cmd)
            counts["unknown:" + cmd] += 1
            conn.sendall(binary_msg(b"500\0", b""))
            return
        try:
            reply = h(args)
        except (ValueError, IndexError) as e:
            log("dnsproxyd: %s %r: %s" % (cmd, args[:2], e))
            reply = binary_msg(FAILED, struct.pack("<i", EAI_NODATA))
        counts[cmd] = counts.get(cmd, 0) + 1
        conn.sendall(reply)
    except OSError:
        pass
    finally:
        conn.close()


def serve(root, log=lambda m: print(m, file=sys.stderr)):
    """Bind <root>/dev/socket/dnsproxyd and answer on daemon threads.
    Returns the listening socket and the per-command counts."""
    path = root.rstrip("/") + "/dev/socket/dnsproxyd"
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)
    os.chmod(path, 0o666)
    s.listen(64)
    counts = {}

    def accept():
        while True:
            try:
                c, _ = s.accept()
            except OSError:
                return
            threading.Thread(target=handle, args=(c, log, counts), daemon=True).start()

    threading.Thread(target=accept, name="dnsproxyd", daemon=True).start()
    return s, counts


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    serve(sys.argv[1])
    print("dnsproxyd on %s/dev/socket/dnsproxyd" % sys.argv[1], flush=True)
    threading.Event().wait()
