#!/usr/bin/env python3
"""Read an Android APK without running any of it: its binary manifest, the
label and icon from resources.arsc, native ABIs, split hints and signing
blocks (docs/APK_SUPPORT.md). Pure Python 3.9+, standard library only.

  scripts/apk-inspect.py app.apk                      JSON on stdout
  scripts/apk-inspect.py --pretty app.apk
  scripts/apk-inspect.py --extract-icon out.png app.apk
  scripts/apk-inspect.py --xml app.apk                the decoded manifest
  scripts/apk-inspect.py --fdroid-index index-v2.json app.apk
                                                     compare with F-Droid's index

Exit status: 0 an APK was read; 2 not an APK or unreadable; 3 a bundle
(XAPK, APKS, APKM, AAB) that is recognised but not supported yet.

Nothing here verifies a signature cryptographically: the signing section says
which schemes are present and which certificate each one names, not that the
signature over the APK is valid.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import zipfile
import zlib

# ------------------------------------------------------------------ constants

# Chunk types (frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h).
RES_STRING_POOL_TYPE = 0x0001
RES_TABLE_TYPE = 0x0002
RES_XML_TYPE = 0x0003
RES_XML_START_NAMESPACE_TYPE = 0x0100
RES_XML_END_NAMESPACE_TYPE = 0x0101
RES_XML_START_ELEMENT_TYPE = 0x0102
RES_XML_END_ELEMENT_TYPE = 0x0103
RES_XML_CDATA_TYPE = 0x0104
RES_XML_RESOURCE_MAP_TYPE = 0x0180
RES_TABLE_PACKAGE_TYPE = 0x0200
RES_TABLE_TYPE_TYPE = 0x0201
RES_TABLE_TYPE_SPEC_TYPE = 0x0202

UTF8_FLAG = 0x100

# Res_value data types.
TYPE_NULL = 0x00
TYPE_REFERENCE = 0x01
TYPE_ATTRIBUTE = 0x02
TYPE_STRING = 0x03
TYPE_FLOAT = 0x04
TYPE_DIMENSION = 0x05
TYPE_FRACTION = 0x06
TYPE_DYNAMIC_REFERENCE = 0x07
TYPE_INT_DEC = 0x10
TYPE_INT_HEX = 0x11
TYPE_INT_BOOLEAN = 0x12
TYPE_FIRST_COLOR = 0x1C
TYPE_LAST_COLOR = 0x1F

# ResTable_type flags and ResTable_entry flags.
TYPE_FLAG_SPARSE = 0x01
TYPE_FLAG_OFFSET16 = 0x02
ENTRY_FLAG_COMPLEX = 0x0001
ENTRY_FLAG_COMPACT = 0x0008
NO_ENTRY32 = 0xFFFFFFFF
NO_ENTRY16 = 0xFFFF

DENSITY_ANY = 0xFFFE
DENSITY_NONE = 0xFFFF

ANDROID_NS = "http://schemas.android.com/apk/res/android"

# android:* attributes by resource id (android.R.attr, public.xml). Used when
# an obfuscator has renamed or emptied the attribute's name string: Android
# itself matches these attributes by id, not by name. Each id was checked
# against real APKs (benchmarks/stage25-apk-install.txt).
ANDROID_ATTRS = {
    0x01010001: "label",
    0x01010002: "icon",
    0x01010003: "name",
    0x0101000E: "enabled",
    0x01010010: "exported",
    0x01010024: "value",
    0x01010025: "resource",
    0x01010202: "targetActivity",
    0x0101020C: "minSdkVersion",
    0x0101021B: "versionCode",
    0x0101021C: "versionName",
    0x01010270: "targetSdkVersion",
    0x01010271: "maxSdkVersion",
    0x01010281: "glEsVersion",
    0x0101028E: "required",
    0x0101052C: "roundIcon",
    0x010103F4: "isGame",
    0x010104EA: "extractNativeLibs",
    0x01010545: "appCategory",
    0x01010572: "compileSdkVersion",
}

# APK Signing Block ids (apksig: ApkSigningBlockUtils, V2/V3SchemeConstants).
SIG_BLOCK_MAGIC = b"APK Sig Block 42"
SIG_BLOCK_IDS = {
    0x7109871A: "v2",
    0xF05368C0: "v3",
    0x1B93AD61: "v3.1",
    0x42726577: "verity-padding",
    0x6DFF800D: "source-stamp-v2",
    0x2B09189E: "source-stamp-v1",
    0x2146444E: "google-play-frosting",
    0x504B4453: "dependency-info",
}
V3_LINEAGE_ATTR = 0x3BA06F8C

KNOWN_ABIS = ("arm64-v8a", "armeabi-v7a", "armeabi", "x86_64", "x86",
              "riscv64", "mips", "mips64")

# Limits: an APK is untrusted input.
MAX_XML = 16 << 20
MAX_ARSC = 128 << 20
MAX_ICON = 16 << 20
MAX_SIGFILE = 1 << 20


class APKError(Exception):
    """The file is not an APK this parser can read."""


class UnsupportedBundle(APKError):
    """A recognised bundle format (XAPK, APKS, ...) that is not supported yet."""

    def __init__(self, fmt, reason):
        super().__init__(reason)
        self.format = fmt
        self.reason = reason


# ------------------------------------------------------------------ low level

def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def chunk_header(b, o):
    """(type, header size, chunk size) of the chunk at `o`, checked against `b`."""
    if o + 8 > len(b):
        raise APKError("truncated chunk header at %d" % o)
    ctype, hsize, size = struct.unpack_from("<HHI", b, o)
    if hsize < 8 or size < hsize or o + size > len(b):
        raise APKError("bad chunk at %d: type 0x%04x header %d size %d" % (o, ctype, hsize, size))
    return ctype, hsize, size


class StringPool:
    """ResStringPool: UTF-8 or UTF-16 strings, decoded on demand."""

    def __init__(self, b, o):
        ctype, hsize, size = chunk_header(b, o)
        if ctype != RES_STRING_POOL_TYPE or hsize < 28:
            raise APKError("expected a string pool at %d" % o)
        count, _styles, flags, strings_start, _styles_start = struct.unpack_from("<IIIII", b, o + 8)
        self.b = b
        self.end = o + size
        self.utf8 = bool(flags & UTF8_FLAG)
        if o + hsize + 4 * count > self.end:
            raise APKError("string pool offsets overrun its chunk")
        self.offsets = struct.unpack_from("<%dI" % count, b, o + hsize)
        self.data = o + strings_start
        self.cache = {}

    def __len__(self):
        return len(self.offsets)

    def get(self, i):
        if i is None or i < 0 or i >= len(self.offsets):
            return None
        if i in self.cache:
            return self.cache[i]
        b, p = self.b, self.data + self.offsets[i]
        try:
            if self.utf8:
                n = b[p]
                p += 2 if n & 0x80 else 1          # UTF-16 length, unused
                n = b[p]
                if n & 0x80:
                    n = ((n & 0x7F) << 8) | b[p + 1]
                    p += 2
                else:
                    p += 1
                s = decode_utf8(bytes(b[p:min(p + n, self.end)]))
            else:
                n = u16(b, p)
                if n & 0x8000:
                    n = ((n & 0x7FFF) << 16) | u16(b, p + 2)
                    p += 4
                else:
                    p += 2
                s = bytes(b[p:min(p + 2 * n, self.end)]).decode("utf-16-le", "replace")
        except (IndexError, struct.error):
            s = None
        self.cache[i] = s
        return s


def decode_utf8(raw):
    """UTF-8 as the pool holds it. Older tools wrote characters outside the
    BMP as two encoded surrogates (modified UTF-8): they are paired back up,
    and anything unpaired or invalid becomes U+FFFD."""
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        pass
    try:
        s = raw.decode("utf-8", "surrogatepass")
    except UnicodeDecodeError:
        return raw.decode("utf-8", "replace")
    return s.encode("utf-16-le", "surrogatepass").decode("utf-16-le", "replace")


def res_value(dtype, data, strings=None):
    """A typed value as JSON-friendly Python: str, int, bool, float or a
    reference string '@0x7f0e0001'."""
    if dtype == TYPE_STRING:
        return strings.get(data) if strings is not None else None
    if dtype in (TYPE_INT_DEC, TYPE_INT_HEX):
        return struct.unpack("<i", struct.pack("<I", data))[0]
    if dtype == TYPE_INT_BOOLEAN:
        return data != 0
    if dtype in (TYPE_REFERENCE, TYPE_DYNAMIC_REFERENCE):
        return "@0x%08x" % data
    if dtype == TYPE_ATTRIBUTE:
        return "?0x%08x" % data
    if dtype == TYPE_FLOAT:
        return struct.unpack("<f", struct.pack("<I", data))[0]
    if TYPE_FIRST_COLOR <= dtype <= TYPE_LAST_COLOR:
        return "#%08x" % data
    if dtype == TYPE_NULL:
        return None
    return data


def ref_id(value):
    """The resource id of a '@0x...' value, else None."""
    if isinstance(value, str) and value.startswith("@0x"):
        try:
            return int(value[1:], 16)
        except ValueError:
            return None
    return None


# ------------------------------------------------------------------ binary XML

class Attr:
    __slots__ = ("ns", "name", "res_id", "dtype", "data", "value")

    def __init__(self, ns, name, res_id, dtype, data, value):
        self.ns, self.name, self.res_id = ns, name, res_id
        self.dtype, self.data, self.value = dtype, data, value


class Element:
    __slots__ = ("ns", "tag", "attrs", "children", "line")

    def __init__(self, ns, tag, attrs, line):
        self.ns, self.tag, self.attrs, self.line = ns, tag, attrs, line
        self.children = []

    def get(self, name, android=True):
        """The value of android:<name> (or of the plain attribute <name>)."""
        for a in self.attrs:
            if a.name == name and ((a.ns == ANDROID_NS) == android):
                return a.value
        return None

    def find(self, tag):
        return [c for c in self.children if c.tag == tag]

    def first(self, tag):
        for c in self.children:
            if c.tag == tag:
                return c
        return None

    def iter(self):
        """This element and its descendants in document order, without
        recursion (an untrusted document may nest thousands deep)."""
        stack = [self]
        while stack:
            e = stack.pop()
            yield e
            stack.extend(reversed(e.children))


def parse_axml(b):
    """Binary XML (RES_XML_TYPE) -> the root Element."""
    b = memoryview(b) if not isinstance(b, (bytes, bytearray)) else b
    ctype, hsize, size = chunk_header(b, 0)
    if ctype != RES_XML_TYPE:
        raise APKError("not a binary XML document (chunk type 0x%04x)" % ctype)
    end = min(size, len(b))
    strings, resmap = None, []
    stack, root = [], None
    o = hsize
    while o + 8 <= end:
        ctype, chsize, csize = chunk_header(b, o)
        if ctype == RES_STRING_POOL_TYPE:
            strings = StringPool(b, o)
        elif ctype == RES_XML_RESOURCE_MAP_TYPE:
            n = (csize - chsize) // 4
            resmap = list(struct.unpack_from("<%dI" % n, b, o + chsize))
        elif ctype == RES_XML_START_ELEMENT_TYPE:
            if strings is None:
                raise APKError("element before the string pool")
            line = u32(b, o + 8)
            e = o + chsize
            ns_i, name_i, astart, asize, acount = struct.unpack_from("<IIHHH", b, e)
            if asize < 20:
                raise APKError("attribute size %d" % asize)
            attrs = []
            for k in range(acount):
                a = e + astart + k * asize
                if a + 20 > o + csize:
                    raise APKError("attribute overruns its element")
                ans, aname, raw, _vsize, _res0, dtype, data = struct.unpack_from("<IIIHBBI", b, a)
                rid = resmap[aname] if aname < len(resmap) else None
                name = strings.get(aname) or ""
                uri = strings.get(ans) if ans != NO_ENTRY32 else None
                # Android matches framework attributes by id: an obfuscated
                # name string does not change what the attribute is.
                if rid in ANDROID_ATTRS:
                    name, uri = ANDROID_ATTRS[rid], ANDROID_NS
                if dtype == TYPE_STRING and raw != NO_ENTRY32:
                    value = strings.get(raw)
                elif raw != NO_ENTRY32 and dtype == TYPE_NULL:
                    value = strings.get(raw)
                else:
                    value = res_value(dtype, data, strings)
                attrs.append(Attr(uri, name, rid, dtype, data, value))
            el = Element(strings.get(ns_i) if ns_i != NO_ENTRY32 else None,
                         strings.get(name_i) or "", attrs, line)
            if stack:
                stack[-1].children.append(el)
            elif root is None:
                root = el
            stack.append(el)
        elif ctype == RES_XML_END_ELEMENT_TYPE:
            if stack:
                stack.pop()
        # namespaces, CDATA and unknown chunks carry nothing needed here
        o += csize
    if root is None:
        raise APKError("binary XML without a root element")
    return root


def xml_text(root, resources=None):
    """The Element tree as readable XML text (for --xml); references are
    shown by resource name when the resource table is known."""
    out = []

    def fmt(a):
        v = a.value
        if resources is not None and ref_id(v) is not None:
            v = resources.name(ref_id(v))
        if isinstance(v, bool):
            v = "true" if v else "false"
        elif v is None:
            v = ""
        prefix = "android:" if a.ns == ANDROID_NS else ("" if not a.ns else "{%s}" % a.ns)
        v = str(v).replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;")
        return '%s%s="%s"' % (prefix, a.name, v)

    stack = [(root, 0, False)]
    while stack:
        e, depth, closing = stack.pop()
        pad = "  " * depth
        if closing:
            out.append("%s</%s>" % (pad, e.tag))
            continue
        attrs = "".join(" " + fmt(a) for a in e.attrs)
        if e.children:
            out.append("%s<%s%s>" % (pad, e.tag, attrs))
            stack.append((e, depth, True))
            stack.extend((c, depth + 1, False) for c in reversed(e.children))
        else:
            out.append("%s<%s%s/>" % (pad, e.tag, attrs))
    return "\n".join(out) + "\n"


# ------------------------------------------------------------------ resources.arsc

class Config:
    """The parts of ResTable_config the label and icon choice needs."""
    __slots__ = ("language", "country", "density", "sdk", "night", "other")

    def __init__(self, raw):
        n = len(raw)

        def at(o, fmt):
            size = struct.calcsize(fmt)
            return struct.unpack_from(fmt, raw, o)[0] if o + size <= n else 0

        self.language = self._locale(raw[8:10]) if n >= 10 else ""
        self.country = self._locale(raw[10:12]) if n >= 12 else ""
        self.density = at(14, "<H")
        self.sdk = at(24, "<H")
        ui_mode = at(29, "<B")
        self.night = (ui_mode & 0x30) == 0x20
        # Anything else set (mcc/mnc, orientation, screen sizes, a UI mode
        # type such as car or television, "notnight", ...).
        rest = bytes(raw[4:8]) + bytes(raw[12:14]) + bytes(raw[16:24]) + bytes(raw[28:29]) \
            + bytes(raw[30:min(n, 48)])
        self.other = any(rest) or bool(ui_mode & 0x0F) or (ui_mode & 0x30) == 0x10

    @staticmethod
    def _locale(two):
        if not any(two):
            return ""
        if two[0] & 0x80:      # packed three-letter code
            b0, b1 = two[0], two[1]
            first, second, third = b1 & 0x1F, ((b1 & 0xE0) >> 5) | ((b0 & 0x03) << 3), (b0 & 0x7C) >> 2
            return "".join(chr(0x61 + c) for c in (first, second, third))
        return bytes(two).decode("ascii", "replace")

    @property
    def is_default(self):
        return not self.language and not self.country and not self.night and not self.other

    def describe(self):
        parts = []
        if self.language:
            parts.append(self.language + ("-r" + self.country if self.country else ""))
        if self.density:
            parts.append({DENSITY_ANY: "anydpi", DENSITY_NONE: "nodpi"}.get(self.density, "%ddpi" % self.density))
        if self.night:
            parts.append("night")
        if self.sdk:
            parts.append("v%d" % self.sdk)
        return "-".join(parts) or "default"


class ResourceTable:
    """resources.arsc: enough to resolve simple values (strings, references,
    file paths) for any configuration. Complex (bag) entries are skipped.
    A type (string, mipmap, ...) is parsed the first time one of its
    resources is looked up, and a value is decoded only when it is asked
    for: a large game's table holds hundreds of thousands of entries."""

    def __init__(self, b):
        ctype, hsize, size = chunk_header(b, 0)
        if ctype != RES_TABLE_TYPE:
            raise APKError("not a resource table (chunk type 0x%04x)" % ctype)
        self.b = b
        self.values = None
        self.chunks = {}        # (package id, type id) -> [type chunk offset]
        self.pools = {}         # package id -> (type strings, key strings, type id offset)
        self.parsed = {}        # (package id, type id) -> {entry index: [(Config, dtype, data)]}
        self.keys = {}          # (package id, type id, entry index) -> key string index
        self.errors = []
        end = min(size, len(b))
        o = hsize
        while o + 8 <= end:
            ctype, chsize, csize = chunk_header(b, o)
            if ctype == RES_STRING_POOL_TYPE and self.values is None:
                self.values = StringPool(b, o)
            elif ctype == RES_TABLE_PACKAGE_TYPE:
                self._package(o, chsize, csize)
            o += csize

    def _package(self, o, hsize, size):
        b = self.b
        pkg_id = u32(b, o + 8)
        type_strings_off = u32(b, o + 268)
        key_strings_off = u32(b, o + 276)
        type_id_offset = u32(b, o + 284) if hsize >= 288 else 0
        type_strings = StringPool(b, o + type_strings_off) if type_strings_off else None
        key_strings = StringPool(b, o + key_strings_off) if key_strings_off else None
        self.pools[pkg_id] = (type_strings, key_strings, type_id_offset)
        p = o + hsize
        end = o + size
        while p + 8 <= end:
            ctype, chsize, csize = chunk_header(b, p)
            if ctype == RES_TABLE_TYPE_TYPE and chsize >= 24:
                self.chunks.setdefault((pkg_id, b[p + 8]), []).append(p)
            p += csize

    def _entries(self, pkg_id, type_id):
        key = (pkg_id, type_id)
        if key not in self.parsed:
            table = self.parsed[key] = {}
            for o in self.chunks.get(key, []):
                try:
                    self._type(o, pkg_id, table)
                except (APKError, struct.error, IndexError) as e:
                    # One bad chunk hides its own entries, not the table.
                    self.errors.append("type chunk at %d: %s" % (o, e))
        return self.parsed[key]

    def _type(self, o, pkg_id, table):
        b = self.b
        _, hsize, size = chunk_header(b, o)
        type_id, flags = b[o + 8], b[o + 9]
        count, entries_start = u32(b, o + 12), u32(b, o + 16)
        cfg_size = u32(b, o + 20)
        cfg = Config(bytes(b[o + 20:o + 20 + min(cfg_size, hsize - 20)]))
        offsets = o + hsize
        base = o + entries_start
        end = o + size
        if flags & TYPE_FLAG_SPARSE:
            if offsets + 4 * count > end:
                raise APKError("type chunk offsets overrun it")
            pairs = [(idx, off * 4) for idx, off in struct.iter_unpack("<HH", b[offsets:offsets + 4 * count])]
        elif flags & TYPE_FLAG_OFFSET16:
            if offsets + 2 * count > end:
                raise APKError("type chunk offsets overrun it")
            pairs = [(k, off * 4) for k, (off,) in enumerate(struct.iter_unpack("<H", b[offsets:offsets + 2 * count]))
                     if off != NO_ENTRY16]
        else:
            if offsets + 4 * count > end:
                raise APKError("type chunk offsets overrun it")
            raw = struct.unpack_from("<%dI" % count, b, offsets)
            pairs = [(k, off) for k, off in enumerate(raw) if off != NO_ENTRY32]
        for idx, off in pairs:
            e = base + off
            if e + 8 > end:
                continue
            esize, eflags = u16(b, e), u16(b, e + 2)
            if eflags & ENTRY_FLAG_COMPACT:
                key = esize
                dtype, data = eflags >> 8, u32(b, e + 4)
            else:
                key = u32(b, e + 4)
                if eflags & ENTRY_FLAG_COMPLEX:
                    dtype, data = None, None     # a bag (style, array, ...)
                else:
                    v = e + esize
                    if v + 8 > end:
                        continue
                    dtype, data = b[v + 3], u32(b, v + 4)
            self.keys.setdefault((pkg_id, type_id, idx), key)
            if dtype is not None:
                table.setdefault(idx, []).append((cfg, dtype, data))

    def lookup(self, rid):
        """[(Config, value, dtype)] of resource id `rid`, every configuration."""
        pkg, typ, idx = (rid >> 24) & 0xFF, (rid >> 16) & 0xFF, rid & 0xFFFF
        return [(cfg, res_value(dtype, data, self.values), dtype)
                for cfg, dtype, data in self._entries(pkg, typ).get(idx, [])]

    def name(self, rid):
        pkg, typ, idx = (rid >> 24) & 0xFF, (rid >> 16) & 0xFF, rid & 0xFFFF
        type_strings, key_strings, type_id_offset = self.pools.get(pkg, (None, None, 0))
        self._entries(pkg, typ)
        t = type_strings.get(typ - 1 - type_id_offset) if type_strings is not None else None
        k = key_strings.get(self.keys.get((pkg, typ, idx))) if key_strings is not None else None
        return "@%s/%s" % (t, k) if t and k else "@0x%08x" % rid

    def resolve_string(self, value, depth=0):
        """A string value, following references: the default configuration
        first, then English, then whatever there is."""
        rid = ref_id(value)
        if rid is None:
            return value if isinstance(value, str) or value is None else str(value)
        if depth > 8:
            return None
        cands = self.lookup(rid)
        if not cands:
            return None

        def rank(c):
            cfg = c[0]
            if cfg.is_default:
                return 0
            if cfg.language == "en" and not cfg.country and not cfg.other and not cfg.night:
                return 1
            if cfg.language == "en":
                return 2
            if not cfg.language:
                return 3
            return 4
        for cfg, v, _ in sorted(cands, key=rank):
            s = self.resolve_string(v, depth + 1)
            if s:
                return s
        return None


# ------------------------------------------------------------------ images

RASTER = (".png", ".webp", ".jpg", ".jpeg")


def image_info(data):
    """(format, width, height) of a PNG, WebP or JPEG, else (None, None, None)."""
    if data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) >= 24:
        w, h = struct.unpack(">II", data[16:24])
        return "png", w, h
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP" and len(data) >= 30:
        kind = data[12:16]
        if kind == b"VP8 " and len(data) >= 30:
            w, h = struct.unpack_from("<HH", data, 26)
            return "webp", w & 0x3FFF, h & 0x3FFF
        if kind == b"VP8L" and len(data) >= 25:
            bits = struct.unpack_from("<I", data, 21)[0]
            return "webp", (bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1
        if kind == b"VP8X" and len(data) >= 30:
            w = int.from_bytes(data[24:27], "little") + 1
            h = int.from_bytes(data[27:30], "little") + 1
            return "webp", w, h
        return "webp", None, None
    if data[:3] == b"\xff\xd8\xff":
        return "jpeg", None, None
    return None, None, None


def density_of_path(path):
    """Density from a res/ directory name (mipmap-xxxhdpi-v4), for the
    file-name fallback."""
    names = {"ldpi": 120, "mdpi": 160, "tvdpi": 213, "hdpi": 240, "xhdpi": 320,
             "xxhdpi": 480, "xxxhdpi": 640, "nodpi": DENSITY_NONE, "anydpi": DENSITY_ANY}
    parts = path.split("/")
    if len(parts) < 3:
        return 0
    for q in parts[1].split("-")[1:]:
        if q in names:
            return names[q]
        m = re.match(r"^(\d+)dpi$", q)
        if m:
            return int(m.group(1))
    return 0


def density_rank(d):
    """Higher is better for an icon: real densities by value; nodpi is a
    single fixed-size bitmap; default (0) means mdpi."""
    if d == DENSITY_ANY:
        return -1
    if d == DENSITY_NONE:
        return 400
    return d or 160


# ------------------------------------------------------------------ signing

def der_tlv(b, o):
    """(tag, value start, value end) of the DER element at `o`."""
    if o + 2 > len(b):
        raise ValueError("truncated DER")
    tag, n = b[o], b[o + 1]
    o += 2
    if n & 0x80:
        k = n & 0x7F
        if k == 0 or k > 4 or o + k > len(b):
            raise ValueError("unsupported DER length")
        n = int.from_bytes(b[o:o + k], "big")
        o += k
    if o + n > len(b):
        raise ValueError("DER element overruns")
    return tag, o, o + n


def der_children(b, start, end):
    out, o = [], start
    while o < end:
        tag, vs, ve = der_tlv(b, o)
        out.append((tag, o, vs, ve))
        o = ve
    return out


def cert_issuer_serial(cert):
    """(issuer Name TLV, serialNumber TLV) of a DER X.509 certificate, or None."""
    try:
        tag, vs, ve = der_tlv(cert, 0)
        tag, tvs, tve = der_tlv(cert, vs)            # TBSCertificate
        kids = der_children(cert, tvs, tve)
        if kids and kids[0][0] == 0xA0:              # [0] version
            kids = kids[1:]
        serial, issuer = kids[0], kids[2]
        return bytes(cert[issuer[1]:issuer[3]]), bytes(cert[serial[1]:serial[3]])
    except (ValueError, IndexError):
        return None


def pkcs7_certificates(b):
    """The signers' DER certificates of a PKCS#7 SignedData
    (META-INF/*.RSA|DSA|EC). A block may carry a chain or other certificates
    too; each SignerInfo names its own by issuer and serial number (RFC 5652),
    and only those are the APK's signers. When no SignerInfo can be matched,
    every certificate is returned."""
    tag, vs, ve = der_tlv(b, 0)
    if tag != 0x30:
        raise ValueError("not a SEQUENCE")
    kids = der_children(b, vs, ve)
    if len(kids) < 2 or kids[1][0] != 0xA0:
        raise ValueError("no [0] content")
    _, _, cvs, cve = kids[1]
    tag, svs, sve = der_tlv(b, cvs)
    if tag != 0x30:
        raise ValueError("no SignedData")
    parts = der_children(b, svs, sve)
    certs = []
    for ktag, kstart, kvs, kve in parts:
        if ktag == 0xA0:          # [0] IMPLICIT certificates
            certs = [bytes(b[s:e]) for _, s, _, e in der_children(b, kvs, kve)]
    wanted = []
    if parts and parts[-1][0] == 0x31:               # SET OF SignerInfo
        for _, _, ivs, ive in der_children(b, parts[-1][2], parts[-1][3]):
            info = der_children(b, ivs, ive)
            if len(info) >= 2 and info[1][0] == 0x30:     # issuerAndSerialNumber
                sid = der_children(b, info[1][2], info[1][3])
                if len(sid) == 2:
                    wanted.append((bytes(b[sid[0][1]:sid[0][3]]), bytes(b[sid[1][1]:sid[1][3]])))
    signers = [c for c in certs if cert_issuer_serial(c) in wanted]
    return signers or certs


def lp_slices(b):
    """The u32-length-prefixed items of `b`."""
    out, o = [], 0
    while o + 4 <= len(b):
        n = u32(b, o)
        if o + 4 + n > len(b):
            raise ValueError("length-prefixed item overruns")
        out.append(b[o + 4:o + 4 + n])
        o += 4 + n
    if o != len(b):
        raise ValueError("trailing bytes")
    return out


def lp_one(b, o):
    n = u32(b, o)
    if o + 4 + n > len(b):
        raise ValueError("length-prefixed item overruns")
    return b[o + 4:o + 4 + n], o + 4 + n


def v2v3_signers(value, v3):
    """[{certificates: [sha256], lineage: [sha256], minSdk, maxSdk}] of a v2
    or v3 block value (apksig V2SchemeVerifier / V3SchemeVerifier layouts)."""
    signers = []
    seq, _ = lp_one(value, 0)
    for signer in lp_slices(seq):
        signed, o = lp_one(signer, 0)
        digests, p = lp_one(signed, 0)
        certs_blob, p = lp_one(signed, p)
        certs = [hashlib.sha256(c).hexdigest() for c in lp_slices(certs_blob)]
        info = {"certificates": certs}
        if v3:
            info["minSdk"], info["maxSdk"] = struct.unpack_from("<II", signed, p)
            p += 8
            attrs, p = lp_one(signed, p)
            for attr in lp_slices(attrs):
                if len(attr) >= 4 and u32(attr, 0) == V3_LINEAGE_ATTR:
                    info["lineage"] = v3_lineage(attr[4:])
        signers.append(info)
    return signers


def v3_lineage(b):
    """Certificate sha256s of a v3 proof-of-rotation lineage, oldest first."""
    certs = []
    if len(b) < 4 or u32(b, 0) != 1:
        return certs
    o = 4
    while o + 4 <= len(b):
        node, o = lp_one(b, o)
        signed, _ = lp_one(node, 0)
        cert, _ = lp_one(signed, 0)
        certs.append(hashlib.sha256(cert).hexdigest())
    return certs


def signing_block(f, size):
    """{id: value bytes} of the APK Signing Block, or None when there is none."""
    tail = min(size, 65535 + 22)
    f.seek(size - tail)
    buf = f.read(tail)
    eocd = buf.rfind(b"PK\x05\x06")
    if eocd < 0 or eocd + 22 > len(buf):
        return None
    cd_offset = u32(buf, eocd + 16)
    if cd_offset < 32 or cd_offset > size:
        return None
    f.seek(cd_offset - 24)
    footer = f.read(24)
    if len(footer) != 24 or footer[8:] != SIG_BLOCK_MAGIC:
        return None
    block_size = u64(footer, 0)
    start = cd_offset - block_size - 8
    if block_size < 24 or start < 0 or block_size > (256 << 20):
        return None
    f.seek(start)
    block = f.read(block_size + 8)
    if u64(block, 0) != block_size:
        return None
    pairs, o, end = {}, 8, len(block) - 24
    while o + 12 <= end:
        n = u64(block, o)
        if n < 4 or o + 8 + n > end:
            break
        pairs[u32(block, o + 8)] = block[o + 12:o + 8 + n]
        o += 8 + n
    return pairs


def signing_info(zf, f, size):
    info = {"v1": False, "v2": False, "v3": False, "v31": False,
            "blocks": [], "certificates": [], "certificateSource": None, "rotatedCertificates": [], "lineage": [],
            "verified": False}
    names = zf.namelist()
    sig_files = [n for n in names if re.match(r"^META-INF/[^/]+\.(RSA|DSA|EC)$", n, re.I)]
    sf_files = [n for n in names if re.match(r"^META-INF/[^/]+\.SF$", n, re.I)]
    v1_certs = []
    if sig_files and sf_files and "META-INF/MANIFEST.MF" in names:
        info["v1"] = True
        for n in sig_files:
            try:
                data = read_entry(zf, n, MAX_SIGFILE)
                v1_certs += [hashlib.sha256(c).hexdigest() for c in pkcs7_certificates(data)]
            except (ValueError, APKError, zipfile.BadZipFile):
                info.setdefault("errors", []).append("v1: could not read the certificate in %s" % n)
    try:
        pairs = signing_block(f, size)
    except (OSError, struct.error):
        pairs = None
    v2_certs, v3_certs, v31_certs, lineage = [], [], [], []
    if pairs:
        info["blocks"] = [SIG_BLOCK_IDS.get(k, "0x%08x" % k) for k in pairs]
        for bid, label in ((0x7109871A, "v2"), (0xF05368C0, "v3"), (0x1B93AD61, "v31")):
            if bid not in pairs:
                continue
            info[label] = True
            try:
                signers = v2v3_signers(pairs[bid], v3=bid != 0x7109871A)
            except (ValueError, struct.error):
                info.setdefault("errors", []).append("%s: signer block not readable" % label)
                continue
            # The first certificate of each signer is its signing certificate.
            certs = [s["certificates"][0] for s in signers if s["certificates"]]
            if bid == 0x7109871A:
                v2_certs = certs
            elif bid == 0xF05368C0:
                v3_certs = certs
            else:
                v31_certs = certs
            for s in signers:
                for c in s.get("lineage", []):
                    if c not in lineage:
                        lineage.append(c)
    # Android uses the strongest scheme present: v3(.1), then v2, then v1.
    for source, certs in (("v3", v3_certs), ("v2", v2_certs), ("v1", v1_certs)):
        if certs:
            info["certificates"] = sorted(set(certs))
            info["certificateSource"] = source
            break
    # v3.1 carries a rotated key that Android 13 and later use in place of
    # the v3 signer; android-pm.py accepts either as the app's identity.
    info["rotatedCertificates"] = sorted(set(v31_certs))
    info["lineage"] = lineage
    return info


# ------------------------------------------------------------------ zip access

# What zipfile raises for a damaged or unusual entry.
ZIP_ERRORS = (zipfile.BadZipFile, zlib.error, EOFError, OSError, RuntimeError, NotImplementedError)


def read_raw(zf, info, limit):
    """An entry read from its local header, ignoring what zipfile refuses:
    an unknown compression method is taken as stored (as Android's zip
    reader does) and the encryption flag is ignored (a "fake encryption"
    trick aimed at analysis tools). Deflate is inflated; anything else is
    returned as it is."""
    fp = zf.fp
    fp.seek(info.header_offset)
    header = fp.read(30)
    if len(header) < 30 or header[:4] != b"PK\x03\x04":
        raise APKError("bad local header for %s" % info.filename)
    n, m = struct.unpack_from("<HH", header, 26)
    fp.seek(info.header_offset + 30 + n + m)
    raw = fp.read(min(info.compress_size, 1 << 30))
    if info.compress_type == zipfile.ZIP_DEFLATED:
        try:
            d = zlib.decompressobj(-15)
            return d.decompress(raw, limit + 1)
        except zlib.error as e:
            raise APKError("%s: %s" % (info.filename, e))
    return raw[:limit + 1]


def read_entry(zf, name, limit):
    """An entry's bytes, at most `limit` of them (see read_raw for the
    entries zipfile will not open)."""
    try:
        info = zf.getinfo(name)
    except KeyError:
        raise APKError("no %s in the archive" % name)
    if info.file_size > limit:
        raise APKError("%s is %d bytes (limit %d)" % (name, info.file_size, limit))
    try:
        with zf.open(info) as fh:
            data = fh.read(limit + 1)
    except (NotImplementedError, RuntimeError):
        data = read_raw(zf, info, limit)
    except ZIP_ERRORS as e:
        raise APKError("%s: %s" % (name, e))
    if len(data) > limit:
        raise APKError("%s is larger than %d bytes" % (name, limit))
    return data


def bundle_format(names, path):
    """A recognised bundle format of a zip without AndroidManifest.xml."""
    apks = [n for n in names if n.lower().endswith(".apk")]
    ext = os.path.splitext(path)[1].lower()
    if "base/manifest/AndroidManifest.xml" in names or "BundleConfig.pb" in names:
        return "aab", ("an Android App Bundle (.aab) is a publishing format: APKs have to be built "
                       "from it with bundletool first")
    if "toc.pb" in names or ext == ".apks":
        return "apks", ("an APKS set (bundletool build-apks) holds a base APK and split APKs; "
                        "split installs are not supported yet")
    if "manifest.json" in names and (apks or ext == ".xapk"):
        return "xapk", ("an XAPK holds a base APK, split APKs and/or OBB data; "
                        "split installs and OBB data are not supported yet")
    if "info.json" in names and (apks or ext == ".apkm"):
        return "apkm", "an APKM (APKMirror bundle) holds split APKs; split installs are not supported yet"
    if apks:
        return "apk-bundle", ("the archive holds %d APK files but no AndroidManifest.xml of its own; "
                              "split installs are not supported yet" % len(apks))
    return None, None


# ------------------------------------------------------------------ the manifest

def full_class(package, name):
    if not isinstance(name, str) or not name:
        return None
    if name.startswith("."):
        return (package or "") + name
    if "." not in name:
        return (package + "." + name) if package else name
    return name


def as_int(v):
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        try:
            return int(v, 0)
        except ValueError:
            return None
    return None


def as_bool(v, default):
    if v is None:
        return default
    if isinstance(v, bool):
        return v
    if isinstance(v, str):
        return v.lower() == "true"
    if isinstance(v, int):
        return v != 0
    return default


def intent_filter_is_launcher(f, category):
    actions = {a.get("name") for a in f.find("action")}
    cats = {c.get("name") for c in f.find("category")}
    return "android.intent.action.MAIN" in actions and category in cats


def launcher_activity(app, package, category="android.intent.category.LAUNCHER"):
    """The first enabled activity or activity-alias with a MAIN/`category`
    intent filter: (class, alias or None)."""
    if app is None:
        return None, None
    for el in app.children:
        if el.tag not in ("activity", "activity-alias"):
            continue
        if not as_bool(el.get("enabled"), True):
            continue
        if any(intent_filter_is_launcher(f, category) for f in el.find("intent-filter")):
            name = full_class(package, el.get("name"))
            if el.tag == "activity-alias":
                return full_class(package, el.get("targetActivity")), name
            return name, None
    return None, None


# Permissions Android grants an app that did not name them (AOSP
# frameworks/base/data/etc/platform.xml <split-permission>, and the
# PackageParser rule for apps built before API 4). aapt reports the same as
# "uses-implied-permission".
IMPLIED_PERMISSIONS = (
    # (permission held, implied permission, applies below this targetSdk or None)
    ("android.permission.WRITE_EXTERNAL_STORAGE", "android.permission.READ_EXTERNAL_STORAGE", None),
    ("android.permission.READ_CONTACTS", "android.permission.READ_CALL_LOG", 16),
    ("android.permission.WRITE_CONTACTS", "android.permission.WRITE_CALL_LOG", 16),
)


def implied_permissions(names, target_sdk):
    names = set(names)
    out = set()
    if isinstance(target_sdk, int) and target_sdk < 4:
        out |= {"android.permission.WRITE_EXTERNAL_STORAGE", "android.permission.READ_PHONE_STATE"}
    for held, implied, below in IMPLIED_PERMISSIONS:
        if (held in names or held in out) and (below is None or
                                                (isinstance(target_sdk, int) and target_sdk < below)):
            out.add(implied)
    return sorted(out - names)


def gles_version(v):
    n = as_int(v)
    if not n:
        return None
    return "%d.%d" % (n >> 16, n & 0xFFFF)


def vk_version(n):
    return "%d.%d.%d" % ((n >> 22) & 0x7F, (n >> 12) & 0x3FF, n & 0xFFF)


def abi_verdict(abis):
    """ARM64-first (docs/APK_SUPPORT.md): which ABI SteamARM would pick, and
    why the others cannot run. launcher/ApplicationCore.swift AndroidABI
    implements the same rules."""
    abis = set(abis)
    if "arm64-v8a" in abis:
        return {"id": "arm64", "abi": "arm64-v8a",
                "summary": "arm64-v8a native code (preferred): Apple Silicon runs it directly"}
    if abis & {"armeabi-v7a", "armeabi"}:
        return {"id": "arm32-only", "abi": "armeabi-v7a" if "armeabi-v7a" in abis else "armeabi",
                "summary": "32-bit ARM only: Apple Silicon has no AArch32 execution state, "
                           "so this code cannot run natively"}
    if abis & {"x86_64", "x86"}:
        return {"id": "x86-only", "abi": "x86_64" if "x86_64" in abis else "x86",
                "summary": "x86 only: it would need FEX; not supported for Android apps yet"}
    if abis:
        return {"id": "unsupported", "abi": sorted(abis)[0],
                "summary": "native code only for %s: not supported" % ", ".join(sorted(abis))}
    return {"id": "none", "abi": None,
            "summary": "no native code (ART only): runs on any ABI the Android runtime has"}


def native_libs(zf, names):
    """Per ABI: the .so count, their size, the smallest LOAD p_align seen and
    whether any are stored uncompressed (loadable straight from the APK)."""
    out = {}
    for n in names:
        m = re.match(r"^lib/([^/]+)/[^/]+$", n)
        if not m:
            continue
        abi = m.group(1)
        info = zf.getinfo(n)
        d = out.setdefault(abi, {"count": 0, "bytes": 0, "minLoadAlign": None, "stored": 0})
        d["count"] += 1
        d["bytes"] += info.file_size
        if info.compress_type == zipfile.ZIP_STORED:
            d["stored"] += 1
        try:
            with zf.open(info) as fh:
                align = elf_min_load_align(fh.read(1 << 16))
        except ZIP_ERRORS + (struct.error,):
            align = None
        if align and (d["minLoadAlign"] is None or align < d["minLoadAlign"]):
            d["minLoadAlign"] = align
    return out


def elf_min_load_align(head):
    """The smallest PT_LOAD p_align of the ELF whose first bytes are `head`
    (program headers within them), else None."""
    if head[:4] != b"\x7fELF" or len(head) < 52 or head[5] != 1:
        return None
    is64 = head[4] == 2
    if is64:
        phoff, = struct.unpack_from("<Q", head, 32)
        phentsize, phnum = struct.unpack_from("<HH", head, 54)
    else:
        phoff, = struct.unpack_from("<I", head, 28)
        phentsize, phnum = struct.unpack_from("<HH", head, 42)
    if phnum == 0 or phnum > 256 or phoff < 52 or phoff + phentsize * phnum > len(head):
        return None
    ph = head[phoff:phoff + phentsize * phnum]
    best = None
    for i in range(phnum):
        p = i * phentsize
        if p + (56 if is64 else 32) > len(ph):
            break
        if u32(ph, p) != 1:        # PT_LOAD
            continue
        align = u64(ph, p + 48) if is64 else u32(ph, p + 28)
        best = align if best is None else min(best, align)
    return best


class APK:
    """One APK, read once."""

    def __init__(self, path):
        self.path = path
        try:
            self.size = os.path.getsize(path)
            self.f = open(path, "rb")
        except OSError as e:
            raise APKError("cannot read %s: %s" % (path, e.strerror or e))
        try:
            self.zf = zipfile.ZipFile(self.f)
        except ZIP_ERRORS + (ValueError, struct.error) as e:
            self.f.close()
            raise APKError("not a zip archive: %s" % e)
        try:
            self._read(path)
        except APKError:
            self.close()
            raise
        except (struct.error, IndexError, ValueError, TypeError) + ZIP_ERRORS as e:
            self.close()
            raise APKError("unreadable APK: %s" % e)

    def _read(self, path):
        self.names = self.zf.namelist()
        if "AndroidManifest.xml" not in self.names:
            fmt, reason = bundle_format(self.names, path)
            if fmt:
                raise UnsupportedBundle(fmt, reason)
            raise APKError("no AndroidManifest.xml: not an APK")
        self.manifest = parse_axml(read_entry(self.zf, "AndroidManifest.xml", MAX_XML))
        if self.manifest.tag != "manifest":
            raise APKError("the manifest's root is <%s>, not <manifest>" % self.manifest.tag)
        self.resources = None
        self.resources_error = None
        if "resources.arsc" in self.names:
            try:
                self.resources = ResourceTable(read_entry(self.zf, "resources.arsc", MAX_ARSC))
            except (APKError, struct.error, IndexError) as e:
                self.resources_error = str(e)

    def close(self):
        try:
            self.zf.close()
        except AttributeError:
            pass
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- values

    def string(self, v):
        if ref_id(v) is not None:
            return self.resources.resolve_string(v) if self.resources else None
        return v if v is None or isinstance(v, str) else str(v)

    def number(self, v):
        """An integer attribute, possibly a reference to an integer resource."""
        rid = ref_id(v)
        if rid is not None and self.resources:
            for _, val, _ in self.resources.lookup(rid):
                n = as_int(val)
                if n is not None:
                    return n
            return None
        return as_int(v)

    # -- icon

    def _drawable_candidates(self, v, depth=0, seen=None):
        """(path, density, config) of every file a drawable reference names,
        following references and adaptive-icon/bitmap/inset XML."""
        rid = ref_id(v)
        if rid is None or self.resources is None or depth > 6:
            if isinstance(v, str) and v.startswith("res/"):
                return [(v, density_of_path(v), None)]
            return []
        seen = seen or set()
        if rid in seen:
            return []
        seen.add(rid)
        out = []
        for cfg, val, _ in self.resources.lookup(rid):
            if isinstance(val, str) and val.startswith("res/"):
                out.append((val, cfg.density, cfg))
            elif ref_id(val) is not None:
                out += self._drawable_candidates(val, depth + 1, seen)
        return out

    def _xml_drawable_refs(self, path):
        """References an XML drawable points at, foreground first: adaptive
        icon <foreground>, then <bitmap src>, <inset drawable>, layer items."""
        try:
            root = parse_axml(read_entry(self.zf, path, MAX_XML))
        except (APKError, struct.error, IndexError):
            return [], None
        refs = []
        kind = root.tag
        order = ["foreground", "background"] if kind == "adaptive-icon" else []
        els = [root.first(t) for t in order if root.first(t) is not None] + [root]
        for el in els:
            for sub in el.iter():
                for key in ("drawable", "src"):
                    v = sub.get(key)
                    if ref_id(v) is not None and v not in refs:
                        refs.append(v)
        return refs, kind

    def icon(self):
        """The best raster icon: {path, density, format, width, height, source}."""
        app = self.manifest.first("application")
        v = app.get("icon") if app is not None else None
        if v is None and app is not None:
            v = app.get("roundIcon")
        tried, notes = [], []
        if v is not None:
            queue, depth = [(v, "manifest")], 0
            while queue and depth < 4:
                depth += 1
                next_queue = []
                for ref, source in queue:
                    cands = self._drawable_candidates(ref)
                    raster = [c for c in cands if c[0].lower().endswith(RASTER) and c[0] in self.names]
                    usable = [c for c in raster if c[2] is None or not (c[2].night or c[2].language)] or raster
                    if usable:
                        path, dens, _ = max(usable, key=lambda c: (density_rank(c[1]), c[0]))
                        return self._icon_result(path, dens, source)
                    for path, _, _ in cands:
                        if path.lower().endswith(".xml") and path in self.names and path not in tried:
                            tried.append(path)
                            refs, kind = self._xml_drawable_refs(path)
                            if kind:
                                notes.append("%s is <%s>" % (path, kind))
                            label = "adaptive-foreground" if kind == "adaptive-icon" else "xml-" + (kind or "drawable")
                            next_queue += [(r, label) for r in refs]
                queue = next_queue
        # No usable resource: a launcher-named bitmap by file name.
        guess = [n for n in self.names if re.match(r"^res/(mipmap|drawable)[^/]*/ic_launcher(_round)?\.(png|webp)$", n)]
        if guess:
            path = max(guess, key=lambda n: (density_rank(density_of_path(n)), "round" not in n, n))
            result = self._icon_result(path, density_of_path(path), "filename")
            if notes:
                result["note"] = "; ".join(notes)
            return result
        return {"path": None, "source": None,
                "note": "; ".join(notes) or ("no android:icon" if v is None else "no raster image for the icon")}

    def _icon_result(self, path, density, source):
        try:
            data = read_entry(self.zf, path, MAX_ICON)
        except APKError as e:
            # An unreadable or oversized icon is no reason to reject the APK.
            return {"path": None, "source": None, "note": str(e)}
        fmt, w, h = image_info(data)
        return {"path": path, "density": density, "format": fmt, "width": w, "height": h,
                "bytes": len(data), "source": source}

    def icon_bytes(self, path):
        return read_entry(self.zf, path, MAX_ICON)

    # -- the report

    def report(self):
        m = self.manifest
        package = m.get("package", android=False)
        if not isinstance(package, str) or not package:
            raise APKError("the manifest has no package name")
        app = m.first("application")
        sdk = m.first("uses-sdk")
        min_sdk = self.number(sdk.get("minSdkVersion")) if sdk is not None else None
        min_sdk_raw = sdk.get("minSdkVersion") if sdk is not None else None
        target_sdk = self.number(sdk.get("targetSdkVersion")) if sdk is not None else None
        if min_sdk is None and isinstance(min_sdk_raw, str):
            min_sdk = min_sdk_raw               # a preview codename
        if min_sdk is None:
            min_sdk = 1                          # Android's default
        if target_sdk is None:
            target_sdk = min_sdk                 # defaults to minSdkVersion
        version_code = self.number(m.get("versionCode"))
        major = self.number(m.get("versionCodeMajor"))
        activity, alias = launcher_activity(app, package)
        leanback, _ = launcher_activity(app, package, "android.intent.category.LEANBACK_LAUNCHER")
        label = self.string(app.get("label")) if app is not None else None
        label_source = "application" if label else None
        if not label and activity and app is not None:
            for el in app.children:
                if el.tag in ("activity", "activity-alias") and \
                        full_class(package, el.get("name")) in (activity, alias):
                    label = self.string(el.get("label"))
                    label_source = "activity" if label else None
                    break
        if not label:
            label, label_source = package, "package"

        permissions = []
        for el in m.children:
            if el.tag in ("uses-permission", "uses-permission-sdk-23", "uses-permission-sdk-m"):
                name = self.string(el.get("name"))
                if not name:
                    continue
                p = {"name": name}
                mx = self.number(el.get("maxSdkVersion"))
                if mx is not None:
                    p["maxSdkVersion"] = mx
                if el.tag != "uses-permission":
                    p["sdk23"] = True
                permissions.append(p)
        features, gles, vulkan = [], None, {}
        for el in m.find("uses-feature"):
            required = as_bool(el.get("required"), True)
            name = self.string(el.get("name"))
            if name:
                f = {"name": name, "required": required}
                ver = self.number(el.get("version"))
                if ver is not None:
                    f["version"] = ver
                features.append(f)
                if name == "android.hardware.vulkan.version" and ver is not None:
                    vulkan["version"] = vk_version(ver)
                    vulkan["required"] = required
                elif name == "android.hardware.vulkan.level" and ver is not None:
                    vulkan["level"] = ver
                elif name == "android.hardware.vulkan.compute" and ver is not None:
                    vulkan["compute"] = ver
            g = el.get("glEsVersion")
            if g is not None:
                n = self.number(g)
                if n and (gles is None or n > gles[0]):
                    gles = (n, gles_version(n), required)
        abis = sorted({n.split("/")[1] for n in self.names
                       if re.match(r"^lib/[^/]+/[^/]+$", n)})
        meta = {}
        if app is not None:
            for el in app.find("meta-data"):
                k = self.string(el.get("name"))
                if k:
                    meta[k] = el.get("value") if el.get("value") is not None else el.get("resource")
        splits = {
            "split": m.get("split", android=False),
            "configForSplit": m.get("configForSplit", android=False),
            "isFeatureSplit": as_bool(m.get("isFeatureSplit"), False),
            "isSplitRequired": as_bool(m.get("isSplitRequired"), False),
            "requiredSplitTypes": m.get("requiredSplitTypes"),
            "splitTypes": m.get("splitTypes"),
            "vendingSplitsRequired": as_bool(meta.get("com.android.vending.splits.required"), False),
            "vendingSplits": "com.android.vending.splits" in meta,
            "fusedModules": meta.get("com.android.dynamic.apk.fused.modules"),
        }
        splits["needsSplits"] = bool(splits["isSplitRequired"] or splits["requiredSplitTypes"]
                                     or splits["vendingSplitsRequired"])
        splits["isSplit"] = bool(splits["split"])
        report = {
            "format": "apk",
            "supported": True,
            "fileName": os.path.basename(self.path),
            "size": self.size,
            "package": package,
            "versionCode": ((major << 32) | version_code) if major and version_code is not None else version_code,
            "versionName": self.string(m.get("versionName")),
            "minSdk": min_sdk,
            "targetSdk": target_sdk,
            "maxSdk": self.number(sdk.get("maxSdkVersion")) if sdk is not None else None,
            "compileSdk": self.number(m.get("compileSdkVersion")) or self.number(
                m.get("platformBuildVersionCode", android=False)),
            "label": label,
            "labelSource": label_source,
            "launcherActivity": activity,
            "launcherAlias": alias,
            "leanbackLauncherActivity": leanback,
            "permissions": permissions,
            "impliedPermissions": implied_permissions([p["name"] for p in permissions], target_sdk),
            "features": features,
            "glEsVersion": gles[1] if gles else None,
            "vulkan": vulkan or None,
            "abis": abis,
            "abiVerdict": abi_verdict(abis),
            "nativeLibs": native_libs(self.zf, self.names),
            "extractNativeLibs": as_bool(app.get("extractNativeLibs"), True) if app is not None else True,
            "isGame": bool(app is not None and (as_bool(app.get("isGame"), False)
                                                or self.number(app.get("appCategory")) == 0)),
            "hasCode": any(re.match(r"^classes\d*\.dex$", n) for n in self.names),
            "splits": splits,
            "signing": signing_info(self.zf, self.f, self.size),
            "icon": self.icon(),
        }
        if self.resources is not None and self.resources.errors:
            self.resources_error = "; ".join(self.resources.errors[:5])
        if self.resources_error:
            report["resourcesError"] = self.resources_error
        return report


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def inspect(path, with_sha256=True):
    """The JSON-ready report of `path`. Raises APKError / UnsupportedBundle."""
    with APK(path) as apk:
        try:
            r = apk.report()
        except (struct.error, IndexError, ValueError, KeyError, TypeError, RecursionError) + ZIP_ERRORS as e:
            raise APKError("unreadable APK: %s" % e)
    if with_sha256:
        try:
            r["sha256"] = sha256_file(path)
        except OSError as e:
            raise APKError("cannot read %s: %s" % (path, e.strerror or e))
    return r


def unsupported_report(path, err):
    return {"format": err.format, "supported": False, "reason": err.reason,
            "fileName": os.path.basename(path), "size": os.path.getsize(path)}


# ------------------------------------------------------------------ F-Droid

def fdroid_compare(report, index_path):
    """The report against the F-Droid index-v2 entry with the same file
    sha256: {"matched": {...}, "mismatched": {...}} (None when absent)."""
    with open(index_path, encoding="utf-8") as f:
        index = json.load(f)
    pkg = index.get("packages", {}).get(report.get("package") or "")
    if not pkg:
        return None
    version = None
    for v in pkg.get("versions", {}).values():
        if v.get("file", {}).get("sha256") == report.get("sha256"):
            version = v
            break
    if version is None:
        return {"found": False}
    m = version.get("manifest", {})
    sdk = m.get("usesSdk", {})
    fd_perms = sorted({p["name"] for p in m.get("usesPermission", []) or []}
                      | {p["name"] for p in m.get("usesPermissionSdk23", []) or []})
    # F-Droid lists the implied permissions too, and only required features.
    ours_perms = sorted({p["name"] for p in report["permissions"]} | set(report["impliedPermissions"]))
    pairs = {
        "versionCode": (report["versionCode"], m.get("versionCode")),
        "versionName": (report["versionName"], m.get("versionName")),
        "minSdk": (report["minSdk"], sdk.get("minSdkVersion")),
        "targetSdk": (report["targetSdk"], sdk.get("targetSdkVersion")),
        "abis": (report["abis"], sorted(m.get("nativecode") or [])),
        "permissions": (ours_perms, fd_perms),
        "features": (sorted({f["name"] for f in report["features"] if f["required"]}),
                     sorted({f["name"] for f in m.get("features", []) or []})),
        "signer": (report["signing"]["certificates"], sorted(m.get("signer", {}).get("sha256", []))),
        "size": (report["size"], version.get("file", {}).get("size")),
    }
    names = pkg.get("metadata", {}).get("name", {})
    out = {"found": True, "matched": [], "mismatched": {},
           "fdroidName": names.get("en-US") or next(iter(names.values()), None)}
    for k, (ours, theirs) in pairs.items():
        if ours == theirs:
            out["matched"].append(k)
        else:
            out["mismatched"][k] = {"ours": ours, "fdroid": theirs}
    return out


# ------------------------------------------------------------------ CLI

def main(argv=None):
    ap = argparse.ArgumentParser(description="Read an APK's manifest, label, icon, ABIs and signing blocks.")
    ap.add_argument("apk")
    ap.add_argument("--pretty", action="store_true", help="indented JSON")
    ap.add_argument("--extract-icon", metavar="PATH", help="write the chosen icon's bytes to PATH")
    ap.add_argument("--xml", action="store_true", help="print the decoded AndroidManifest.xml instead")
    ap.add_argument("--fdroid-index", metavar="INDEX", help="compare with F-Droid's index-v2.json")
    args = ap.parse_args(argv)
    indent = 2 if args.pretty else None
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")      # labels are not ASCII
    try:
        if args.xml:
            with APK(args.apk) as apk:
                sys.stdout.write(xml_text(apk.manifest, apk.resources))
            return 0
        report = inspect(args.apk)
        if args.extract_icon and report["icon"].get("path"):
            with APK(args.apk) as apk:
                data = apk.icon_bytes(report["icon"]["path"])
            with open(args.extract_icon, "wb") as f:
                f.write(data)
            report["icon"]["extractedTo"] = args.extract_icon
        if args.fdroid_index:
            report["fdroid"] = fdroid_compare(report, args.fdroid_index)
    except UnsupportedBundle as e:
        print(json.dumps(unsupported_report(args.apk, e), indent=indent, ensure_ascii=False))
        return 3
    except APKError as e:
        print(json.dumps({"supported": False, "error": str(e),
                          "fileName": os.path.basename(args.apk)}, indent=indent, ensure_ascii=False))
        return 2
    print(json.dumps(report, indent=indent, ensure_ascii=False, sort_keys=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
