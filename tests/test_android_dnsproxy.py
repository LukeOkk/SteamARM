"""scripts/android_dnsproxy.py: netd's dnsproxyd replies, read back the way
bionic's getaddrinfo and gethostbyname proxies read them. Numeric and
localhost names only, so no network is needed."""
import os
import socket
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
import android_dnsproxy as dp  # noqa: E402


def ask(root, cmd):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(root + "/dev/socket/dnsproxyd")
    c.sendall(cmd.encode() + b"\0")
    data = b""
    while True:
        b = c.recv(65536)
        if not b:
            break
        data += b
    c.close()
    return data


class Reader:
    def __init__(self, b):
        self.b, self.i = b, 0

    def take(self, n):
        v = self.b[self.i:self.i + n]
        assert len(v) == n, "short reply"
        self.i += n
        return v

    def be32(self):
        return struct.unpack(">i", self.take(4))[0]

    def lendata(self):
        return self.take(self.be32())


def read_addrinfo(reply):
    r = Reader(reply)
    code = r.take(4)
    if code != b"222\0":
        return code, []
    out = []
    while r.be32():
        flags, fam, st, pr = r.be32(), r.be32(), r.be32(), r.be32()
        sa = r.lendata()
        r.lendata()
        out.append((fam, st, pr, sa))
    assert r.i == len(reply)
    return code, out


class DnsProxyTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="dp", dir="/tmp")
        os.makedirs(self.root + "/dev/socket")
        self.sock, self.counts = dp.serve(self.root, log=lambda m: None)

    def tearDown(self):
        self.sock.close()
        os.unlink(self.root + "/dev/socket/dnsproxyd")

    def test_numeric_ipv4_with_port(self):
        code, ai = read_addrinfo(ask(self.root, "getaddrinfo 10.1.2.3 443 4 2 1 6 0"))
        self.assertEqual(code, b"222\0")
        self.assertEqual(len(ai), 1)
        fam, st, pr, sa = ai[0]
        self.assertEqual((fam, st, pr), (2, 1, 6))
        # Linux sockaddr_in: family (host order), port (network order), address
        self.assertEqual(sa, struct.pack("<H", 2) + struct.pack(">H", 443) + bytes([10, 1, 2, 3]) + b"\0" * 8)

    def test_numeric_ipv6(self):
        code, ai = read_addrinfo(ask(self.root, "getaddrinfo ::1 ^ 4 10 1 0 0"))
        self.assertEqual(code, b"222\0")
        fam, _, _, sa = ai[0]
        self.assertEqual(fam, 10)
        self.assertEqual(len(sa), 28)
        self.assertEqual(sa[:2], struct.pack("<H", 10))
        self.assertEqual(sa[8:24], socket.inet_pton(socket.AF_INET6, "::1"))

    def test_bionic_addrconfig_with_service_name(self):
        # bionic's AI_ADDRCONFIG (0x400, glibc's AI_NUMERICSERV) with a named
        # service: what apt asks for its mirror, "<host> https".
        code, ai = read_addrinfo(ask(self.root, "getaddrinfo localhost https 1024 2 1 6 0"))
        self.assertEqual(code, b"222\0")
        self.assertTrue(ai)
        self.assertEqual(ai[0][3][2:4], struct.pack(">H", 443))

    def test_bionic_numericserv(self):
        # bionic's AI_NUMERICSERV is 0x8: a service name is then refused.
        reply = ask(self.root, "getaddrinfo 10.1.2.3 https 12 2 1 6 0")
        self.assertEqual(reply[:4], b"401\0")

    def test_no_hints_localhost(self):
        code, ai = read_addrinfo(ask(self.root, "getaddrinfo localhost ^ -1 -1 -1 -1 100"))
        self.assertEqual(code, b"222\0")
        self.assertTrue(ai)
        self.assertTrue(all(f in (2, 10) for f, _, _, _ in ai))

    def test_failure_is_sendbinarymsg(self):
        reply = ask(self.root, "getaddrinfo 999.1.1.1 ^ 4 2 1 0 0")        # AI_NUMERICHOST, not a number
        self.assertEqual(reply[:4], b"401\0")
        self.assertEqual(struct.unpack(">I", reply[4:8])[0], 4)
        self.assertEqual(len(reply), 12)

    def test_gethostbyname_localhost(self):
        r = Reader(ask(self.root, "gethostbyname 0 localhost 2"))
        self.assertEqual(r.take(4), b"222\0")
        self.assertTrue(r.lendata())                  # h_name
        while r.lendata():                            # aliases, then an empty one
            pass
        self.assertEqual(struct.unpack(">II", r.take(8)), (2, 4))
        addr = r.lendata()
        self.assertEqual(len(addr), 16)
        self.assertEqual(addr[:4], bytes([127, 0, 0, 1]))
        while r.lendata():
            pass

    def test_unknown_command(self):
        self.assertEqual(ask(self.root, "frobnicate 1 2")[:4], b"500\0")


if __name__ == "__main__":
    unittest.main()
