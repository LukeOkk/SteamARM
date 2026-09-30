"""Small synthetic APKs for tests/test_apk_inspect.py and tests/test_android_pm.py.

Everything here is generated: a binary AndroidManifest.xml (AXML), a
resources.arsc, PNG/WebP headers, ELF headers, a PKCS#7 block for the v1
scheme and an APK Signing Block for v2/v3. No third-party APK is committed.
The certificates are opaque DER blobs, not real X.509: the inspector only
hashes them (it does not verify signatures).
"""
import io
import struct
import zipfile

ANDROID_NS = "http://schemas.android.com/apk/res/android"

# android.R.attr ids (the same table as scripts/apk-inspect.py ANDROID_ATTRS).
ATTR_IDS = {
    "label": 0x01010001, "icon": 0x01010002, "name": 0x01010003, "enabled": 0x0101000E,
    "exported": 0x01010010, "value": 0x01010024, "resource": 0x01010025,
    "targetActivity": 0x01010202, "minSdkVersion": 0x0101020C, "versionCode": 0x0101021B,
    "versionName": 0x0101021C, "targetSdkVersion": 0x01010270, "maxSdkVersion": 0x01010271,
    "glEsVersion": 0x01010281, "required": 0x0101028E, "roundIcon": 0x0101052C,
    "isGame": 0x010103F4, "extractNativeLibs": 0x010104EA, "appCategory": 0x01010545,
    "compileSdkVersion": 0x01010572,
}

T_REF, T_STRING, T_INT_DEC, T_INT_HEX, T_BOOL = 0x01, 0x03, 0x10, 0x11, 0x12


# ------------------------------------------------------------------ string pools

def _len8(n):
    return bytes([n]) if n < 0x80 else bytes([0x80 | (n >> 8), n & 0xFF])


def _len16(n):
    return struct.pack("<H", n) if n < 0x8000 else struct.pack("<HH", 0x8000 | (n >> 16), n & 0xFFFF)


def string_pool(strings, utf8=True):
    data, offsets = b"", []
    for s in strings:
        offsets.append(len(data))
        if utf8:
            enc = s.encode("utf-8")
            data += _len8(len(s)) + _len8(len(enc)) + enc + b"\0"
        else:
            enc = s.encode("utf-16-le")
            data += _len16(len(enc) // 2) + enc + b"\0\0"
    while len(data) % 4:
        data += b"\0"
    header = 28
    strings_start = header + 4 * len(strings)
    size = strings_start + len(data)
    return (struct.pack("<HHIIIIII", 0x0001, header, size, len(strings), 0,
                        0x100 if utf8 else 0, strings_start, 0)
            + b"".join(struct.pack("<I", o) for o in offsets) + data)


# ------------------------------------------------------------------ binary XML

class E:
    """An element: E(tag, [(attr, value)], [children]). An attribute is
    'android:name', 'name' (no namespace) and a value is a str, an int, a bool
    or ref(0x7f...), hexint(n)."""

    def __init__(self, tag, attrs=(), children=()):
        self.tag, self.attrs, self.children = tag, list(attrs), list(children)


class ref(int):
    pass


class hexint(int):
    pass


def axml(root, utf8=True, obfuscate_names=False):
    """Binary XML for the tree `root`. With obfuscate_names the android:*
    attribute name strings are replaced by junk; their resource ids stay."""
    # android:* attribute names first (the resource map covers them), then the rest.
    attr_names, others = [], []

    def collect(e):
        for k, v in e.attrs:
            name = k.split(":", 1)[1] if k.startswith("android:") else k
            # Only attributes with a known id get a resource map entry (the
            # map covers the start of the pool); other android:* names (drawable,
            # version, ...) are matched by name, as for any library attribute.
            if k.startswith("android:") and name in ATTR_IDS:
                if name not in attr_names:
                    attr_names.append(name)
            elif name not in others:
                others.append(name)
            if isinstance(v, str) and not isinstance(v, (ref,)) and v not in others:
                others.append(v)
        if e.tag not in others:
            others.append(e.tag)
        for c in e.children:
            collect(c)

    collect(root)
    pool = list(attr_names) + [s for s in others + [ANDROID_NS, "android"] if s not in attr_names]
    index = {s: i for i, s in enumerate(pool)}
    shown = [("æ" * (i + 1)) if obfuscate_names and i < len(attr_names) else s for i, s in enumerate(pool)]
    chunks = [string_pool(shown, utf8)]
    resmap = [ATTR_IDS[n] for n in attr_names]
    chunks.append(struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(resmap)) + b"".join(struct.pack("<I", r) for r in resmap))
    ns = struct.pack("<HHIII", 0x0100, 16, 24, 1, 0xFFFFFFFF) + struct.pack("<II", index["android"], index[ANDROID_NS])
    chunks.append(ns)

    def emit(e, line=[1]):
        attrs = b""
        for k, v in e.attrs:
            android = k.startswith("android:")
            name = k.split(":", 1)[1] if android else k
            ns_i = index[ANDROID_NS] if android else 0xFFFFFFFF
            raw = 0xFFFFFFFF
            if isinstance(v, bool):
                dtype, data = T_BOOL, 0xFFFFFFFF if v else 0
            elif isinstance(v, ref):
                dtype, data = T_REF, int(v)
            elif isinstance(v, hexint):
                dtype, data = T_INT_HEX, int(v)
            elif isinstance(v, int):
                dtype, data = T_INT_DEC, v & 0xFFFFFFFF
            else:
                dtype, data = T_STRING, index[v]
                raw = index[v]
            attrs += struct.pack("<IIIHBBI", ns_i, index[name], raw, 8, 0, dtype, data)
        line[0] += 1
        body = struct.pack("<IIHHHHHH", 0xFFFFFFFF, index[e.tag], 20, 20, len(e.attrs), 0, 0, 0) + attrs
        chunks.append(struct.pack("<HHIII", 0x0102, 16, 16 + len(body), line[0], 0xFFFFFFFF) + body)
        for c in e.children:
            emit(c)
        chunks.append(struct.pack("<HHIII", 0x0103, 16, 24, line[0], 0xFFFFFFFF)
                      + struct.pack("<II", 0xFFFFFFFF, index[e.tag]))

    emit(root)
    chunks.append(struct.pack("<HHIII", 0x0101, 16, 24, 1, 0xFFFFFFFF) + struct.pack("<II", index["android"], index[ANDROID_NS]))
    body = b"".join(chunks)
    return struct.pack("<HHI", 0x0003, 8, 8 + len(body)) + body


def manifest(package="org.example.app", version_code=42, version_name="1.2.3", min_sdk=24, target_sdk=34,
             label="Example", icon=None, activities=None, permissions=(), features=(), extra_manifest=(),
             app_attrs=(), meta=(), uses_sdk=True, **kw):
    """A typical manifest. activities: [E('activity', ...)] or None for one
    launcher activity '.MainActivity'."""
    if activities is None:
        activities = [launcher_activity(".MainActivity")]
    app = [("android:label", label)] + ([("android:icon", icon)] if icon is not None else []) + list(app_attrs)
    kids = []
    if uses_sdk:
        sdk = []
        if min_sdk is not None:
            sdk.append(("android:minSdkVersion", min_sdk))
        if target_sdk is not None:
            sdk.append(("android:targetSdkVersion", target_sdk))
        kids.append(E("uses-sdk", sdk))
    for p in permissions:
        if isinstance(p, E):
            kids.append(p)
        else:
            kids.append(E("uses-permission", [("android:name", p)]))
    kids += list(features)
    application = E("application", app, list(activities) + [
        E("meta-data", [("android:name", k), ("android:value", v)]) for k, v in meta])
    kids.append(application)
    attrs = [("android:versionCode", version_code), ("android:versionName", version_name),
             ("package", package)] + list(extra_manifest)
    return axml(E("manifest", attrs, kids), **kw)


def launcher_activity(name, tag="activity", category="android.intent.category.LAUNCHER", extra=()):
    return E(tag, [("android:name", name)] + list(extra), [
        E("intent-filter", [], [E("action", [("android:name", "android.intent.action.MAIN")]),
                                E("category", [("android:name", category)])])])


# ------------------------------------------------------------------ resources.arsc

def config(language="", density=0, sdk=0, night=False, size=64, ui_mode=0):
    raw = bytearray(size)
    struct.pack_into("<I", raw, 0, size)
    if language:
        raw[8:10] = language.encode("ascii")[:2]
    struct.pack_into("<H", raw, 14, density)
    struct.pack_into("<H", raw, 24, sdk)
    if size > 29:
        raw[29] = (0x20 if night else 0) | ui_mode
    return bytes(raw)


def arsc(types, package_id=0x7F, package_name="org.example.app", layout="dense", utf8=True):
    """A resource table. types: {type name: {entry index: (key, [(config bytes, value)])}}
    where a value is a str (a string, or a file path 'res/...'), an int or ref(id).
    Type ids follow the dict order (1, 2, ...). layout: dense | sparse | offset16 | compact."""
    values, keys = [], []

    def vindex(s):
        if s not in values:
            values.append(s)
        return values.index(s)

    def kindex(s):
        if s not in keys:
            keys.append(s)
        return keys.index(s)

    type_names = list(types)
    # Collect every configuration of every type.
    type_chunks = []
    for t_i, tname in enumerate(type_names, start=1):
        entries = types[tname]
        count = max(entries) + 1 if entries else 0
        configs = []
        for idx, (key, per_cfg) in entries.items():
            kindex(key)
            for cfg, _ in per_cfg:
                if cfg not in configs:
                    configs.append(cfg)
        for cfg in configs:
            present = {}
            for idx, (key, per_cfg) in entries.items():
                for c, v in per_cfg:
                    if c == cfg:
                        present[idx] = (key, v)
            blobs, offsets = b"", {}
            for idx in sorted(present):
                key, v = present[idx]
                if isinstance(v, ref):
                    dtype, data = T_REF, int(v)
                elif isinstance(v, int):
                    dtype, data = T_INT_DEC, v & 0xFFFFFFFF
                else:
                    dtype, data = T_STRING, vindex(v)
                offsets[idx] = len(blobs)
                if layout == "compact":
                    blobs += struct.pack("<HHI", kindex(key), 0x0008 | (dtype << 8), data)
                else:
                    blobs += struct.pack("<HHI", 8, 0, kindex(key)) + struct.pack("<HBBI", 8, 0, dtype, data)
            flags = {"sparse": 0x01, "offset16": 0x02}.get(layout, 0)
            if layout == "sparse":
                table = b"".join(struct.pack("<HH", i, offsets[i] // 4) for i in sorted(offsets))
                n = len(offsets)
            elif layout == "offset16":
                table = b"".join(struct.pack("<H", offsets[i] // 4 if i in offsets else 0xFFFF) for i in range(count))
                n = count
            else:
                table = b"".join(struct.pack("<I", offsets[i] if i in offsets else 0xFFFFFFFF) for i in range(count))
                n = count
            while len(table) % 4:
                table += b"\0"
            header = 20 + len(cfg)
            entries_start = header + len(table)
            chunk = (struct.pack("<HHIBBHII", 0x0201, header, entries_start + len(blobs), t_i, flags, 0, n, entries_start)
                     + cfg + table + blobs)
            type_chunks.append(chunk)
    tpool = string_pool(type_names, utf8)
    kpool = string_pool(keys, utf8)
    name = package_name.encode("utf-16-le")[:254].ljust(256, b"\0")
    pkg_header = 288
    body = tpool + kpool + b"".join(type_chunks)
    pkg = (struct.pack("<HHII", 0x0200, pkg_header, pkg_header + len(body), package_id) + name
           + struct.pack("<IIIII", pkg_header, len(type_names), pkg_header + len(tpool), len(keys), 0) + body)
    vpool = string_pool(values, utf8)
    return struct.pack("<HHII", 0x0002, 12, 12 + len(vpool) + len(pkg), 1) + vpool + pkg


# ------------------------------------------------------------------ images and ELF

def png(width, height):
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + ihdr + b"\0\0\0\0" + b"\0" * 16


def webp_lossy(width, height):
    vp8 = b"\x00\x00\x00" + b"\x9d\x01\x2a" + struct.pack("<HH", width, height) + b"\0" * 10
    return b"RIFF" + struct.pack("<I", 4 + 8 + len(vp8)) + b"WEBP" + b"VP8 " + struct.pack("<I", len(vp8)) + vp8


def elf(machine=183, is64=True, align=0x4000):
    """A tiny ELF shared object header with one PT_LOAD."""
    if is64:
        eh = (b"\x7fELF\x02\x01\x01" + b"\0" * 9 + struct.pack("<HHIQQQIHHHHHH", 3, machine, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0))
        ph = struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, 0x100, 0x100, align)
    else:
        eh = (b"\x7fELF\x01\x01\x01" + b"\0" * 9 + struct.pack("<HHIIIIIHHHHHH", 3, machine, 1, 0, 52, 0, 0, 52, 32, 1, 0, 0, 0))
        ph = struct.pack("<IIIIIIII", 1, 0, 0, 0, 0x100, 0x100, 5, align)
    return eh + ph


# ------------------------------------------------------------------ signing

def der(tag, content):
    n = len(content)
    if n < 0x80:
        length = bytes([n])
    elif n < 0x100:
        length = b"\x81" + bytes([n])
    else:
        length = b"\x82" + struct.pack(">H", n)
    return bytes([tag]) + length + content


def fake_cert(name):
    """An opaque DER SEQUENCE standing in for an X.509 certificate."""
    return der(0x30, der(0x0C, name.encode()))


def x509ish(subject, serial, issuer="SteamARM test CA"):
    """A DER certificate with the X.509 layout the parser reads (TBS with
    version, serial, algorithm, issuer); not a valid certificate."""
    name = lambda cn: der(0x30, der(0x31, der(0x30, der(0x06, b"\x55\x04\x03") + der(0x0C, cn.encode()))))
    alg = der(0x30, der(0x06, bytes.fromhex("2a864886f70d01010b")) + der(0x05, b""))
    tbs = der(0x30, der(0xA0, der(0x02, b"\x02")) + der(0x02, bytes([serial])) + alg + name(issuer)
              + der(0x30, b"") + name(subject))
    return der(0x30, tbs + alg + der(0x03, b"\x00sig"))


def pkcs7(certs, signer=None):
    """SignedData with `certs`; with `signer` (one of them, in x509ish
    form) a SignerInfo names it by issuer and serial number."""
    oid_signed = der(0x06, bytes.fromhex("2a864886f70d010702"))
    oid_data = der(0x06, bytes.fromhex("2a864886f70d010701"))
    infos = b""
    if signer is not None:
        # issuer and serial straight from the signer's TBS (x509ish layout)
        issuer_serial = _issuer_serial(signer)
        alg = der(0x30, der(0x06, bytes.fromhex("608648016503040201")))
        infos = der(0x30, der(0x02, b"\x01") + der(0x30, issuer_serial[0] + issuer_serial[1]) + alg + alg
                    + der(0x04, b"sig"))
    signed = der(0x30, der(0x02, b"\x01") + der(0x31, b"") + der(0x30, oid_data)
                 + der(0xA0, b"".join(certs)) + der(0x31, infos))
    return der(0x30, oid_signed + der(0xA0, signed))


def _tlv(b, o):
    n = b[o + 1]
    h = 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(b[o + 2:o + 2 + k], "big")
        h += k
    return o + h, o + h + n


def _issuer_serial(cert):
    vs, _ = _tlv(cert, 0)
    tvs, tve = _tlv(cert, vs)
    kids, o = [], tvs
    while o < tve:
        s, e = _tlv(cert, o)
        kids.append(cert[o:e])
        o = e
    if kids[0][0] == 0xA0:
        kids = kids[1:]
    return kids[2], kids[0]


def lp(b):
    return struct.pack("<I", len(b)) + b


def v2_value(certs):
    signed = lp(b"") + lp(b"".join(lp(c) for c in certs)) + lp(b"")
    signer = lp(signed) + lp(b"") + lp(b"pubkey")
    return lp(lp(signer))


def v3_value(certs, lineage=()):
    attrs = b""
    if lineage:
        nodes = b"".join(lp(lp(lp(c) + struct.pack("<I", 0x0103)) + struct.pack("<II", 0, 0x0103) + lp(b""))
                         for c in lineage)
        attrs = lp(struct.pack("<I", 0x3BA06F8C) + struct.pack("<I", 1) + nodes)
    signed = lp(b"") + lp(b"".join(lp(c) for c in certs)) + struct.pack("<II", 28, 0x7FFFFFFF) + lp(attrs)
    signer = lp(signed) + struct.pack("<II", 28, 0x7FFFFFFF) + lp(b"") + lp(b"pubkey")
    return lp(lp(signer))


def with_signing_block(zip_bytes, pairs):
    """Insert an APK Signing Block with {id: value} before the central directory."""
    eocd = zip_bytes.rfind(b"PK\x05\x06")
    cd_off = struct.unpack_from("<I", zip_bytes, eocd + 16)[0]
    body = b"".join(struct.pack("<QI", 4 + len(v), k) + v for k, v in pairs.items())
    size = len(body) + 8 + 16
    block = struct.pack("<Q", size) + body + struct.pack("<Q", size) + b"APK Sig Block 42"
    new_cd = cd_off + len(block)
    out = zip_bytes[:cd_off] + block + zip_bytes[cd_off:]
    eocd += len(block)
    return out[:eocd + 16] + struct.pack("<I", new_cd) + out[eocd + 20:]


# ------------------------------------------------------------------ the APK

def apk(path, manifest_bytes=None, resources=None, files=None, v1=None, v2=None, v3=None, lineage=(),
        extra_blocks=None, v1_signer=None, v31=None):
    """Write an APK to `path`. v1/v2/v3: a list of certificate DER blobs."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        if manifest_bytes is not None:
            z.writestr("AndroidManifest.xml", manifest_bytes)
        if resources is not None:
            z.writestr(zipfile.ZipInfo("resources.arsc"), resources)   # stored, as aapt does
        z.writestr("classes.dex", b"dex\n035\0")
        for name, data in (files or {}).items():
            z.writestr(name, data)
        if v1:
            z.writestr("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\r\n")
            z.writestr("META-INF/CERT.SF", b"Signature-Version: 1.0\r\n")
            z.writestr("META-INF/CERT.RSA", pkcs7(v1, v1_signer))
    data = buf.getvalue()
    pairs = dict(extra_blocks or {})
    if v2:
        pairs[0x7109871A] = v2_value(v2)
    if v3:
        pairs[0xF05368C0] = v3_value(v3, lineage)
    if v31:
        pairs[0x1B93AD61] = v3_value(v31)
    if pairs:
        data = with_signing_block(data, pairs)
    with open(path, "wb") as f:
        f.write(data)
    return path


def zip_of(path, files):
    with zipfile.ZipFile(path, "w") as z:
        for name, data in files.items():
            z.writestr(name, data)
    return path


def split_set(directory, cert, package="org.example.app", code=42, required=True):
    """Signed base and config APK bytes for an x86_64 device fixture."""
    from pathlib import Path
    directory = Path(directory)
    out = {}
    for split in (None, "config.x86_64", "config.arm64_v8a", "config.xxhdpi", "config.es"):
        name = "base.apk" if split is None else split + ".apk"
        attrs = [] if split is None else [("split", split)]
        meta = [("com.android.vending.splits.required", True)] if split is None and required else []
        files = {"lib/x86_64/libtest.so": elf()} if split == "config.x86_64" else (
            {"lib/arm64-v8a/libtest.so": elf()} if split == "config.arm64_v8a" else {})
        path = directory / name
        apk(str(path), manifest(package=package, version_code=code, extra_manifest=attrs, meta=meta),
            files=files, v1=[cert])
        out[name] = path.read_bytes()
    return out
