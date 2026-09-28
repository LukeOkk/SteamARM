#!/usr/bin/env python3
"""Read a SteamOS (Steam Frame) disk image on macOS: no VM, no mount, no btrfs-progs.

The Steam Frame recovery image (steamframe-oobe-repair-<build>-<version>.img) is a
GPT disk whose root filesystem is btrfs. macOS cannot mount btrfs, and
`btrfs restore` (btrfs-progs, three versions up to v7.1) stopped on it with
"zstd frame incomplete" (MEASURED on the owner's Mac, 2026-09-28). This tool
reads the image itself, read-only, with its own btrfs reader:

  info      IMAGE                  partitions, filesystem of each, btrfs
                                   superblock, features, subvolumes
  diagnose  IMAGE [--part N]       walk every file extent; for compressed ones
                                   say whether the frame decodes to exactly the
                                   extent's ram_bytes, to more, or runs out of
                                   input -- the cause of "frame incomplete"
  extract   IMAGE OUT [--part N]   copy the tree out, as the kernel would show
                                   it; ownership, xattrs and device nodes go to
                                   OUT/.steamarm-manifest.json
  inventory ROOT [--json F --md F] what the extracted userspace contains: OS
                                   release, pacman packages, ELF architectures
                                   and interpreters, FEX / Steam / Proton /
                                   Steam Linux Runtime / gamescope / Vulkan ICDs
                                   / NSS, x86 remnants, pacman repositories
  compare   INV.json REPO.db...    image packages against pacman repository
                                   databases (e.g. holo-core-aarch64-preview)

Decompression follows the kernel, not btrfs-progs: the kernel stops a zstd
(or zlib) stream once it has produced the extent's ram_bytes and ignores the
rest of the frame; btrfs-progs `restore` insists that the frame end inside the
buffer and fails otherwise. `diagnose` counts how many extents differ.

The image is opened O_RDONLY and never written. Nothing here boots the image's
kernel or runs its code.

Needs: python3 (3.9+). zstd extents need the `zstandard` module
(pip3 install zstandard) or the `zstd` command (brew install zstd); lzo extents
are decoded here in Python.
"""
import argparse
import collections
import hashlib
import json
import os
import stat
import struct
import subprocess
import sys
import zlib

# ------------------------------------------------------------------ constants
SUPER_OFF = 0x10000
BTRFS_MAGIC = b'_BHRfS_M'
HDR = 101                       # struct btrfs_header
ITEM = 25                       # struct btrfs_item (key + offset + size)
KEYPTR = 33                     # struct btrfs_key_ptr

ROOT_TREE_OBJECTID = 1
CHUNK_TREE_OBJECTID = 3
FS_TREE_OBJECTID = 5
ROOT_TREE_DIR_OBJECTID = 6
CSUM_TREE_OBJECTID = 7
FIRST_FREE_OBJECTID = 256
EXTENT_CSUM_OBJECTID = 0xFFFFFFFFFFFFFFF6

INODE_ITEM, INODE_REF, INODE_EXTREF, XATTR_ITEM = 1, 12, 13, 24
DIR_ITEM, DIR_INDEX, EXTENT_DATA, EXTENT_CSUM = 84, 96, 108, 128
ROOT_ITEM, ROOT_BACKREF, ROOT_REF, CHUNK_ITEM = 132, 144, 156, 228

FT_NAMES = {0: 'unknown', 1: 'file', 2: 'dir', 3: 'chrdev', 4: 'blkdev',
            5: 'fifo', 6: 'sock', 7: 'symlink', 8: 'xattr'}
COMPRESS = {0: 'none', 1: 'zlib', 2: 'lzo', 3: 'zstd'}
CSUM_TYPES = {0: ('crc32c', 4), 1: ('xxhash64', 8), 2: ('sha256', 32), 3: ('blake2b', 32)}

INCOMPAT = ['MIXED_BACKREF', 'DEFAULT_SUBVOL', 'MIXED_GROUPS', 'COMPRESS_LZO',
            'COMPRESS_ZSTD', 'BIG_METADATA', 'EXTENDED_IREF', 'RAID56',
            'SKINNY_METADATA', 'NO_HOLES', 'METADATA_UUID', 'RAID1C34', 'ZONED',
            'EXTENT_TREE_V2', 'RAID_STRIPE_TREE', 'bit15', 'SIMPLE_QUOTA',
            'REMAP_TREE']
COMPAT_RO = ['FREE_SPACE_TREE', 'FREE_SPACE_TREE_VALID', 'VERITY', 'BLOCK_GROUP_TREE']
# Block group profiles whose data we cannot read by taking one stripe.
STRIPED = {1 << 3: 'RAID0', 1 << 6: 'RAID10', 1 << 7: 'RAID5', 1 << 8: 'RAID6'}

# GPT partition type GUIDs worth naming (mixed-endian text form).
PART_TYPES = {
    'C12A7328-F81F-11D2-BA4B-00A0C93EC93B': 'EFI system',
    '0FC63DAF-8483-4772-8E79-3D69D8477DE4': 'Linux filesystem',
    'B921B045-1DF0-41C3-AF44-4C6F280D3FAE': 'Linux root (ARM64)',
    '4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709': 'Linux root (x86-64)',
    '933AC7E1-2EB4-4F13-B844-0E14E2AEF915': 'Linux /home',
    '4D21B016-B534-45C2-A9FB-5C16E091FD2D': 'Linux /var',
    '0657FD6D-A4AB-43C4-84E5-0933C84B4F4F': 'Linux swap',
}


def die(msg):
    sys.exit('error: ' + msg)


def u8(b, o): return b[o]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


def flags_text(value, names):
    out = [n for i, n in enumerate(names) if value >> i & 1]
    rest = value & ~((1 << len(names)) - 1)
    if rest:
        out.append(f'unknown:{rest:#x}')
    return out


def guid_text(b):
    a, b2, c = struct.unpack_from('<IHH', b, 0)
    return f'{a:08X}-{b2:04X}-{c:04X}-{b[8:10].hex().upper()}-{b[10:16].hex().upper()}'


# ------------------------------------------------------------- image + GPT
class Image:
    """The disk image, read-only, with pread."""

    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDONLY)
        self.size = os.fstat(self.fd).st_size

    def read(self, off, n):
        if off < 0 or off + n > self.size:
            raise IOError(f'read {n} bytes at {off:#x} past the end of the image '
                          f'({self.size:#x}): the image or partition is truncated')
        out = bytearray()
        while len(out) < n:
            chunk = os.pread(self.fd, n - len(out), off + len(out))
            if not chunk:
                raise IOError(f'short read at {off + len(out):#x}')
            out += chunk
        return bytes(out)


def detect_fs(img, off, size):
    """Filesystem magic at a partition start (btrfs, ext*, vfat, erofs, squashfs)."""
    try:
        if size > SUPER_OFF + 0x48 and img.read(off + SUPER_OFF + 0x40, 8) == BTRFS_MAGIC:
            return 'btrfs'
        head = img.read(off, 4096) if size >= 4096 else img.read(off, size)
    except IOError:
        return 'unreadable'
    if len(head) >= 1024 + 58 and u16(head, 1024 + 56) == 0xEF53:
        compat = u32(head, 1024 + 0x5C)
        incompat = u32(head, 1024 + 0x60)
        return 'ext4' if incompat & 0x40 else ('ext3' if compat & 0x4 else 'ext2')
    if len(head) >= 1028 and u32(head, 1024) == 0xE0F5E1E2:
        return 'erofs'
    if head[:4] == b'hsqs':
        return 'squashfs'
    if len(head) >= 90 and (head[82:87] == b'FAT32' or head[54:59] in (b'FAT16', b'FAT12')):
        return 'vfat'
    if head[:8] == b'\0' * 8 and not any(head):
        return 'empty'
    return 'unknown'


def partitions(img):
    """GPT partitions (512- or 4096-byte logical sectors), or the whole image
    as partition 0 when it is a bare filesystem."""
    for lba in (512, 4096):
        if img.size < lba * 2 + 92:
            continue
        hdr = img.read(lba, 92)
        if hdr[:8] != b'EFI PART':
            continue
        entries_lba, count, esize = u64(hdr, 72), u32(hdr, 80), u32(hdr, 84)
        table = img.read(entries_lba * lba, count * esize)
        if zlib.crc32(table) != u32(hdr, 88):
            print(f'  warning: GPT entry array CRC mismatch (sector size {lba})', file=sys.stderr)
        parts = []
        for i in range(count):
            e = table[i * esize:(i + 1) * esize]
            if not any(e[:16]):
                continue
            first, last = u64(e, 32), u64(e, 40)
            name = e[56:128].decode('utf-16-le', 'replace').rstrip('\0')
            tguid = guid_text(e[0:16])
            off, size = first * lba, (last - first + 1) * lba
            parts.append(dict(index=i + 1, name=name, type_guid=tguid,
                              type=PART_TYPES.get(tguid, ''), uuid=guid_text(e[16:32]),
                              offset=off, size=size, sector=lba,
                              truncated=off + size > img.size,
                              fs=detect_fs(img, off, min(size, max(0, img.size - off)))))
        return parts
    return [dict(index=0, name='(whole image)', type_guid='', type='', uuid='',
                 offset=0, size=img.size, sector=0, truncated=False,
                 fs=detect_fs(img, 0, img.size))]


# ------------------------------------------------------------ decompression
def lzo1x_decompress(src, out_len):
    """LZO1X decompression (the format btrfs' lzo segments use)."""
    dst = bytearray()
    ip, n = 0, len(src)

    def length_run(ip, base, mask):
        t = src[ip] & mask
        ip += 1
        if t == 0:
            while src[ip] == 0:
                t += 255
                ip += 1
            t += mask + src[ip]
            ip += 1
        return t + base, ip

    def copy_match(dist, length):
        start = len(dst) - dist
        if start < 0:
            raise ValueError('lzo: match before start of output')
        for k in range(length):
            dst.append(dst[start + k])

    t = src[ip]
    state = 0
    if t > 17:
        ip += 1
        t -= 17
        dst += src[ip:ip + t]
        ip += t
        state = 4 if t >= 4 else t
    while ip < n:
        t = src[ip]
        if t < 16:
            if state == 0:                      # literal run
                if t == 0:
                    t, ip = length_run(ip, 3, 15)
                else:
                    t += 3
                    ip += 1
                dst += src[ip:ip + t]
                ip += t
                state = 4
                continue
            ip += 1
            if state == 4:                      # M1 after a literal run: 3 bytes, far
                dist = 1 + 0x800 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_match(dist, 3)
            else:                               # M1: 2 bytes, near
                dist = 1 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_match(dist, 2)
            state = t & 3
        elif t >= 64:                           # M2
            ip += 1
            dist = 1 + ((t >> 2) & 7) + (src[ip] << 3)
            ip += 1
            copy_match(dist, (t >> 5) + 1)
            state = t & 3
        elif t >= 32:                           # M3
            length, ip = length_run(ip, 2, 31)
            dist = 1 + (u16(src, ip) >> 2)
            state = src[ip] & 3
            ip += 2
            copy_match(dist, length)
        else:                                   # M4, or end of stream
            length, ip2 = length_run(ip, 2, 7)
            dist = (t & 8) << 11
            d = u16(src, ip2)
            ip = ip2 + 2
            dist += d >> 2
            state = d & 3
            if dist == 0:
                break                           # end marker
            copy_match(dist + 0x4000, length)
        if state:
            dst += src[ip:ip + state]
            ip += state
        if len(dst) >= out_len:
            break
    return bytes(dst)


def btrfs_lzo(data, ram, sectorsize):
    """btrfs' lzo layout: total length, then per-segment length + LZO1X data;
    a segment header never straddles a sector boundary."""
    total = u32(data, 0)
    pos, out = 4, bytearray()
    while pos < min(total, len(data)) and len(out) < ram:
        left = sectorsize - pos % sectorsize
        if left < 4:
            pos += left                         # padding to the next sector
            continue
        seg = u32(data, pos)
        pos += 4
        out += lzo1x_decompress(data[pos:pos + seg], 1 << 22)
        pos += seg
    return bytes(out), None


_ZSTD = None


def classify(out, ram, finished, tail):
    """Compare a decoded stream with the extent's ram_bytes. The last extent of
    a file has ram_bytes rounded up to the sector while the stream holds only
    the real bytes; the kernel zero-fills that tail, so a finished stream up
    to one sector short is normal."""
    if len(out) > ram:
        return out[:ram], f'frame-larger-than-ram (+{len(out) - ram})'
    if len(out) < ram:
        if finished and ram - len(out) < tail:
            return out + b'\0' * (ram - len(out)), None
        return out + b'\0' * (ram - len(out)), (
            'frame-incomplete-input' if not finished else f'short-output (-{ram - len(out)})')
    return out, None


def zstd_decode(data, ram, tail=4096):
    """Decode like the kernel: keep ram bytes. Returns (bytes, note); note is
    None when the frame matches ram_bytes."""
    global _ZSTD
    if _ZSTD is None:
        try:
            import zstandard
            _ZSTD = zstandard
        except ImportError:
            _ZSTD = False
    if _ZSTD:
        dobj = _ZSTD.ZstdDecompressor(max_window_size=1 << 31).decompressobj()
        try:
            out = dobj.decompress(data)
        except _ZSTD.ZstdError as e:
            return b'\0' * ram, f'zstd-error ({e})'
        return classify(out, ram, dobj.eof, tail)
    try:
        p = subprocess.run(['zstd', '-dcq', '--no-check'], input=data, capture_output=True)
    except FileNotFoundError:
        die('zstd extents need `pip3 install zstandard` or `brew install zstd`')
    return classify(p.stdout, ram, p.returncode == 0, tail)


def zlib_decode(data, ram, tail=4096):
    d = zlib.decompressobj()
    try:
        out = d.decompress(data, ram + 1)
    except zlib.error as e:
        return b'\0' * ram, f'zlib-error ({e})'
    return classify(out, ram, d.eof or len(out) > ram, tail)


def zstd_frame_header(data):
    """Frame_Content_Size from a zstd frame header, or None if absent."""
    if len(data) < 6 or u32(data, 0) != 0xFD2FB528:
        return 'no-magic'
    fhd = data[4]
    fcs_flag, single, did_flag = fhd >> 6, fhd >> 5 & 1, fhd & 3
    pos = 5 + (0 if single else 1) + (0, 1, 2, 4)[did_flag]
    size = (1 if single else 0, 2, 4, 8)[fcs_flag]
    if size == 0:
        return None
    raw = int.from_bytes(data[pos:pos + size], 'little')
    return raw + 256 if size == 2 else raw


# ------------------------------------------------------------------ btrfs
class Btrfs:
    def __init__(self, img, part_off, part_size):
        self.img, self.base, self.part_size = img, part_off, part_size
        sb = img.read(part_off + SUPER_OFF, 4096)
        if sb[0x40:0x48] != BTRFS_MAGIC:
            die('no btrfs superblock at this partition')
        self.sb = sb
        self.fsid = sb[0x20:0x30]
        self.generation = u64(sb, 0x48)
        self.root_bytenr = u64(sb, 0x50)
        self.chunk_root = u64(sb, 0x58)
        self.total_bytes = u64(sb, 0x70)
        self.bytes_used = u64(sb, 0x78)
        self.root_dir = u64(sb, 0x80)
        self.num_devices = u64(sb, 0x88)
        self.sectorsize = u32(sb, 0x90)
        self.nodesize = u32(sb, 0x94)
        self.compat_ro = u64(sb, 0xB4)
        self.incompat = u64(sb, 0xBC)
        self.csum_type = u16(sb, 0xC4)
        self.devid = u64(sb, 0xC9)
        self.label = sb[0x12B:0x22B].split(b'\0')[0].decode('utf-8', 'replace')
        self.chunks = []                        # (logical, length, type, [(devid, physical)])
        n = u32(sb, 0xA0)
        arr = sb[0x32B:0x32B + n]
        pos = 0
        while pos < n:
            logical = u64(arr, pos + 9)
            pos += 17
            clen = self._chunk(arr, pos, logical)
            pos += clen
        self.chunks.sort()
        for key, data in self.walk(self.chunk_root, want=(CHUNK_ITEM,)):
            self._chunk(data, 0, key[2])
        self.chunks = sorted(set((c[0], c[1], c[2], tuple(c[3])) for c in self.chunks))
        self._roots = None
        self._extent_cache = collections.OrderedDict()

    def _chunk(self, b, pos, logical):
        length, ctype = u64(b, pos), u64(b, pos + 24)
        nstripes = u16(b, pos + 44)
        stripes = [(u64(b, pos + 48 + 32 * i), u64(b, pos + 56 + 32 * i)) for i in range(nstripes)]
        self.chunks.append((logical, length, ctype, stripes))
        return 48 + 32 * nstripes

    def features(self):
        return dict(incompat=flags_text(self.incompat, INCOMPAT),
                    compat_ro=flags_text(self.compat_ro, COMPAT_RO))

    def phys(self, logical):
        for start, length, ctype, stripes in self.chunks:
            if start <= logical < start + length:
                for bit, name in STRIPED.items():
                    if ctype & bit:
                        raise IOError(f'{name} block group at {logical:#x}: striped profiles '
                                      'need every device; not supported')
                for devid, off in stripes:
                    if devid == self.devid or len(stripes) == 1:
                        return self.base + off + (logical - start)
                return self.base + stripes[0][1] + (logical - start)
        raise IOError(f'logical {logical:#x} is in no chunk')

    def read_logical(self, logical, n):
        out = bytearray()
        while n > 0:
            for start, length, _t, _s in self.chunks:
                if start <= logical < start + length:
                    take = min(n, start + length - logical)
                    break
            else:
                raise IOError(f'logical {logical:#x} is in no chunk')
            out += self.img.read(self.phys(logical), take)
            logical += take
            n -= take
        return bytes(out)

    def node(self, logical):
        b = self.read_logical(logical, self.nodesize)
        if b[0x20:0x30] != self.fsid and b[0x20:0x30] != self.sb[0x23B:0x24B]:
            raise IOError(f'tree block {logical:#x}: fsid mismatch (wrong offset or foreign data)')
        if u64(b, 0x30) != logical:
            raise IOError(f'tree block {logical:#x}: header says {u64(b, 0x30):#x}')
        return b

    def walk(self, bytenr, want=None, lo=None, hi=None):
        """Every (key, data) of a tree, in key order. lo/hi bound the key
        (objectid, type, offset) to prune subtrees."""
        b = self.node(bytenr)
        nritems, level = u32(b, 0x60), b[0x64]
        if level == 0:
            for i in range(nritems):
                o = HDR + i * ITEM
                key = (u64(b, o), b[o + 8], u64(b, o + 9))
                if lo and key < lo:
                    continue
                if hi and key > hi:
                    return
                if want and key[1] not in want:
                    continue
                doff, dsize = u32(b, o + 17), u32(b, o + 21)
                yield key, b[HDR + doff:HDR + doff + dsize]
            return
        ptrs = []
        for i in range(nritems):
            o = HDR + i * KEYPTR
            ptrs.append(((u64(b, o), b[o + 8], u64(b, o + 9)), u64(b, o + 17)))
        for i, (key, child) in enumerate(ptrs):
            nxt = ptrs[i + 1][0] if i + 1 < len(ptrs) else None
            if lo and nxt and nxt <= lo:
                continue
            if hi and key > hi:
                return
            yield from self.walk(child, want, lo, hi)

    # ----- roots and subvolumes
    def roots(self):
        if self._roots is None:
            self._roots = {}
            self.refs = {}
            self.default_subvol = FS_TREE_OBJECTID
            for key, data in self.walk(self.root_bytenr):
                if key[1] == ROOT_ITEM:
                    self._roots[key[0]] = dict(bytenr=u64(data, 176), level=data[238],
                                               dirid=u64(data, 168), flags=u64(data, 208))
                elif key[1] == ROOT_REF:
                    nlen = u16(data, 16)
                    self.refs[key[2]] = dict(parent=key[0], dirid=u64(data, 0),
                                             name=data[18:18 + nlen].decode('utf-8', 'replace'))
                elif key[1] == DIR_ITEM and key[0] == ROOT_TREE_DIR_OBJECTID:
                    nlen = u16(data, 27)
                    if data[30:30 + nlen] == b'default':
                        self.default_subvol = u64(data, 0)
        return self._roots

    def subvolumes(self):
        roots = self.roots()
        out = []
        for rid in sorted(roots):
            if rid != FS_TREE_OBJECTID and not (FIRST_FREE_OBJECTID <= rid < (1 << 63)):
                continue
            path, cur = [], rid
            while cur in self.refs:
                path.append(self.refs[cur]['name'])
                cur = self.refs[cur]['parent']
            out.append(dict(id=rid, path='/'.join(reversed(path)) or '<top level>',
                            readonly=bool(roots[rid]['flags'] & 1),
                            default=(rid == self.default_subvol)))
        return out

    # ----- file system trees
    def load_tree(self, root_id):
        """All inodes of one subvolume: {ino: {'inode':..., 'dir':[...], 'ext':[...], 'xattr':{...}}}."""
        root = self.roots().get(root_id)
        if not root:
            die(f'no subvolume with id {root_id}')
        inodes = collections.defaultdict(lambda: dict(inode=None, dir=[], ext=[], xattr={}))
        for key, d in self.walk(root['bytenr'], want=(INODE_ITEM, DIR_INDEX, EXTENT_DATA, XATTR_ITEM)):
            ino, t, off = key
            if t == INODE_ITEM:
                inodes[ino]['inode'] = dict(
                    size=u64(d, 16), nlink=u32(d, 40), uid=u32(d, 44), gid=u32(d, 48),
                    mode=u32(d, 52), rdev=u64(d, 56), flags=u64(d, 64),
                    atime=u64(d, 112) + u32(d, 120) / 1e9, mtime=u64(d, 136) + u32(d, 144) / 1e9)
            elif t == DIR_INDEX:
                nlen = u16(d, 27)
                inodes[ino]['dir'].append(dict(
                    index=off, child=u64(d, 0), child_type=d[8], type=d[29],
                    name=d[30:30 + nlen]))
            elif t == EXTENT_DATA:
                e = dict(off=off, ram=u64(d, 8), comp=d[16], enc=d[17], other=u16(d, 18), type=d[20])
                if e['type'] == 0:
                    e['inline'] = d[21:]
                else:
                    e.update(disk=u64(d, 21), disk_len=u64(d, 29), eoff=u64(d, 37), num=u64(d, 45))
                inodes[ino]['ext'].append(e)
            elif t == XATTR_ITEM:
                dlen, nlen = u16(d, 25), u16(d, 27)
                pos = 0
                while pos + 30 <= len(d):       # several xattrs can share a name hash
                    dlen, nlen = u16(d, pos + 25), u16(d, pos + 27)
                    name = d[pos + 30:pos + 30 + nlen].decode('utf-8', 'replace')
                    inodes[ino]['xattr'][name] = d[pos + 30 + nlen:pos + 30 + nlen + dlen]
                    pos += 30 + nlen + dlen
        for rec in inodes.values():
            rec['ext'].sort(key=lambda e: e['off'])
            rec['dir'].sort(key=lambda e: e['index'])
        return dict(inodes), root['dirid']

    def extent_bytes(self, e, stats=None):
        """The file bytes one EXTENT_DATA item contributes (kernel semantics)."""
        if e['enc'] or e['other']:
            raise IOError('encrypted or otherwise-encoded extent: not supported')
        if e['type'] == 0:
            raw = e['inline']
            if e['comp'] == 0:
                return raw[:e['ram']]
            data, note = self.decompress(raw, e['ram'], e['comp'])
            if stats is not None and note:
                stats[note.strip().split(' ')[0]] += 1
            return data
        if e['type'] == 2 or e['disk'] == 0:     # prealloc or hole
            return b'\0' * e['num']
        if e['comp'] == 0:
            return self.read_logical(e['disk'] + e['eoff'], e['num'])
        ck = e['disk']
        data = self._extent_cache.get(ck)
        if data is None:
            raw = self.read_logical(e['disk'], e['disk_len'])
            data, note = self.decompress(raw, e['ram'], e['comp'])
            if stats is not None and note:
                stats[note.strip().split(' ')[0]] += 1
            self._extent_cache[ck] = data
            if len(self._extent_cache) > 64:
                self._extent_cache.popitem(last=False)
        else:
            self._extent_cache.move_to_end(ck)
        return data[e['eoff']:e['eoff'] + e['num']]

    def decompress(self, raw, ram, comp):
        if comp == 3:
            return zstd_decode(raw, ram, self.sectorsize)
        if comp == 1:
            return zlib_decode(raw, ram, self.sectorsize)
        if comp == 2:
            data, _ = btrfs_lzo(raw, ram, self.sectorsize)
            return classify(data, ram, True, self.sectorsize)
        raise IOError(f'unknown compression type {comp}')


# ------------------------------------------------------------------ helpers
def pick_btrfs(img, want):
    parts = partitions(img)
    cands = [p for p in parts if p['fs'] == 'btrfs']
    if want is not None:
        cands = [p for p in parts if p['index'] == want]
        if not cands:
            die(f'no partition {want}')
        if cands[0]['fs'] != 'btrfs':
            die(f'partition {want} is {cands[0]["fs"]}, not btrfs')
    if not cands:
        die('no btrfs partition in this image')
    if len(cands) > 1 and want is None:
        # Prefer a root-looking partition name, else the largest.
        rootish = [p for p in cands if 'root' in p['name'].lower()]
        cands = sorted(rootish or cands, key=lambda p: -p['size'])
        print(f'  note: {len(cands)} btrfs partitions; using {cands[0]["index"]} '
              f'({cands[0]["name"]}); choose another with --part', file=sys.stderr)
    p = cands[0]
    if p['truncated']:
        print(f'  warning: partition {p["index"]} ends past the end of the image: the image '
              'is truncated (an incomplete download or decompression)', file=sys.stderr)
    return p, Btrfs(img, p['offset'], p['size'])


def choose_subvol(fs, args):
    fs.roots()
    if getattr(args, 'subvol_id', None):
        return args.subvol_id
    return fs.default_subvol


def human(n):
    for unit in ('B', 'KiB', 'MiB', 'GiB', 'TiB'):
        if n < 1024 or unit == 'TiB':
            return f'{n:.1f} {unit}' if unit != 'B' else f'{n} B'
        n /= 1024


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(8 << 20), b''):
            h.update(block)
    return h.hexdigest()


# ------------------------------------------------------------------ commands
def cmd_info(args):
    img = Image(args.image)
    st = os.stat(args.image)
    print(f'image     {args.image}')
    print(f'size      {img.size} bytes ({human(img.size)})')
    print(f'writable  {"yes -- chmod a-w recommended" if st.st_mode & 0o222 else "no"}')
    if args.hash:
        print(f'sha256    {sha256_file(args.image)}')
    parts = partitions(img)
    print('\npartitions')
    for p in parts:
        print(f'  {p["index"]:>2}  {p["name"]:<20} {p["fs"]:<9} {human(p["size"]):>10}  '
              f'at {p["offset"]:#x}  {p["type"] or p["type_guid"]}'
              f'{"  TRUNCATED" if p["truncated"] else ""}')
    for p in parts:
        if p['fs'] != 'btrfs':
            continue
        fs = Btrfs(img, p['offset'], p['size'])
        f = fs.features()
        print(f'\nbtrfs on partition {p["index"]} ({p["name"]})')
        print(f'  label {fs.label!r}  generation {fs.generation}  devices {fs.num_devices}')
        print(f'  sectorsize {fs.sectorsize}  nodesize {fs.nodesize}  '
              f'csum {CSUM_TYPES.get(fs.csum_type, ("?",))[0]}')
        print(f'  size {human(fs.total_bytes)}  used {human(fs.bytes_used)}')
        print(f'  incompat  {" ".join(f["incompat"])}')
        print(f'  compat_ro {" ".join(f["compat_ro"]) or "-"}')
        profiles = collections.Counter()
        for _s, length, ctype, stripes in fs.chunks:
            kind = 'data' if ctype & 1 else 'system' if ctype & 2 else 'metadata'
            prof = next((n for b, n in list(STRIPED.items()) + [(1 << 4, 'RAID1'), (1 << 5, 'DUP')]
                         if ctype & b), 'single')
            profiles[(kind, prof)] += length
        print('  block groups ' + ', '.join(f'{k} {p2} {human(v)}' for (k, p2), v in sorted(profiles.items())))
        if fs.num_devices != 1:
            print('  warning: multi-device filesystem; only this device can be read')
        if fs.incompat & (1 << 13):
            print('  warning: extent-tree-v2 is not supported by this reader')
        print('  subvolumes')
        for sv in fs.subvolumes():
            print(f'    {sv["id"]:>6}  {sv["path"]}{"  (read-only)" if sv["readonly"] else ""}'
                  f'{"  (default)" if sv["default"] else ""}')


def cmd_diagnose(args):
    img = Image(args.image)
    p, fs = pick_btrfs(img, args.part)
    sv = choose_subvol(fs, args)
    print(f'partition {p["index"]} ({p["name"]}), subvolume {sv}: loading the tree...', file=sys.stderr)
    inodes, _ = fs.load_tree(sv)
    stats = collections.Counter()
    by_comp = collections.Counter()
    examples = collections.defaultdict(list)
    seen = set()
    for ino, rec in inodes.items():
        for e in rec['ext']:
            by_comp[COMPRESS.get(e['comp'], str(e['comp']))] += 1
            if e['comp'] == 0:
                continue
            key = ('inline', ino, e['off']) if e['type'] == 0 else e['disk']
            if key in seen:
                continue
            seen.add(key)
            try:
                raw = e['inline'] if e['type'] == 0 else fs.read_logical(e['disk'], e['disk_len'])
            except IOError as err:
                stats['unreadable'] += 1
                examples['unreadable'].append(dict(ino=ino, err=str(err)))
                continue
            fcs = zstd_frame_header(raw) if e['comp'] == 3 else None
            _data, note = fs.decompress(raw, e['ram'], e['comp'])
            kind = (note or 'ok').strip().split(' ')[0]
            stats[kind] += 1
            if kind != 'ok' and len(examples[kind]) < args.examples:
                examples[kind].append(dict(
                    ino=ino, file_offset=e['off'], inline=e['type'] == 0,
                    disk_bytenr=e.get('disk'), disk_len=e.get('disk_len', len(raw)),
                    ram_bytes=e['ram'], frame_content_size=fcs, note=note))
    print(f'file extents by compression: {dict(by_comp)}')
    print(f'distinct compressed extents: {sum(stats.values())}')
    for k, v in sorted(stats.items(), key=lambda kv: -kv[1]):
        print(f'  {k:<28} {v}')
    for k, ex in examples.items():
        print(f'\n{k}: first {len(ex)}')
        for x in ex:
            print('  ' + json.dumps(x))
    print('\nMeaning:')
    print('  ok                     frame decodes to exactly ram_bytes')
    print('  frame-larger-than-ram  the frame holds more than the extent: the kernel stops at')
    print('                         ram_bytes; btrfs-progs `restore` reports "zstd frame incomplete"')
    print('  frame-incomplete-input input ends before the frame does: damaged or truncated image')
    print('  short-output           the stream ended more than a sector short of ram_bytes')
    print('  zstd-error/zlib-error  the data does not decode at all: damaged or misplaced')
    bad = sum(v for k, v in stats.items() if k not in ('ok', 'frame-larger-than-ram'))
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(dict(partition=p['index'], subvolume=sv, counts=stats,
                           by_compression=by_comp, examples=examples), f, indent=1)
    sys.exit(1 if bad else 0)


class Extractor:
    def __init__(self, fs, out, args):
        self.fs, self.out, self.args = fs, out, args
        self.manifest = dict(owners={}, xattrs={}, special={}, symlinks_rewritten={},
                             case_collisions=[], errors=[], subvolumes=[])
        self.hardlinks = {}
        self.lower = {}
        self.stats = collections.Counter()
        self.notes = collections.Counter()

    def rel(self, path):
        return os.path.relpath(path, self.out)

    def check_case(self, path):
        low = path.lower()
        other = self.lower.get(low)
        if other is not None and other != path:
            self.manifest['case_collisions'].append([self.rel(other), self.rel(path)])
            return False
        self.lower[low] = path
        return True

    def run(self, subvol):
        os.makedirs(self.out, exist_ok=True)
        self.case_insensitive = self.probe_case()
        self.extract_tree(subvol, self.out, depth=0)
        mpath = os.path.join(self.out, '.steamarm-manifest.json')
        with open(mpath, 'w') as f:
            json.dump(self.manifest, f, indent=1, sort_keys=True)
        return mpath

    def probe_case(self):
        probe = os.path.join(self.out, '.steamarm-CaseProbe')
        open(probe, 'w').close()
        insensitive = os.path.exists(probe.lower())
        os.unlink(probe)
        if insensitive:
            print('  warning: the output folder is on a case-insensitive volume. Linux trees can '
                  'hold names that differ only in case; those are listed in the manifest and the '
                  'second one is skipped. Use a case-sensitive APFS volume to keep them all.',
                  file=sys.stderr)
        return insensitive

    def extract_tree(self, subvol, dest, depth):
        inodes, top = self.fs.load_tree(subvol)
        self.manifest['subvolumes'].append(dict(id=subvol, path=self.rel(dest)))
        self.inodes = inodes
        self.extract_dir(subvol, inodes, top, dest, depth)

    def extract_dir(self, subvol, inodes, ino, dest, depth):
        os.makedirs(dest, exist_ok=True)
        rec = inodes.get(ino)
        if rec is None:
            return
        for ent in rec['dir']:
            name = ent['name'].decode('utf-8', 'surrogateescape')
            if name in ('.', '..') or '/' in name:
                continue
            path = os.path.join(dest, name)
            if self.case_insensitive and not self.check_case(path):
                continue
            if ent['child_type'] == ROOT_ITEM:  # a nested subvolume
                if self.args.no_subvols:
                    os.makedirs(path, exist_ok=True)
                    continue
                self.extract_tree(ent['child'], path, depth + 1)
                self.inodes = inodes
                continue
            child = inodes.get(ent['child'])
            if child is None or child['inode'] is None:
                self.manifest['errors'].append(f'{self.rel(path)}: no inode {ent["child"]}')
                continue
            try:
                self.extract_one(subvol, inodes, ent['child'], child, path, depth)
            except (IOError, OSError, ValueError) as err:
                self.manifest['errors'].append(f'{self.rel(path)}: {err}')
                self.stats['errors'] += 1
        self.finish(rec, dest, is_dir=True)

    def extract_one(self, subvol, inodes, ino, rec, path, depth):
        mode = rec['inode']['mode']
        fmt = stat.S_IFMT(mode)
        r = self.rel(path)
        if fmt == stat.S_IFDIR:
            self.extract_dir(subvol, inodes, ino, path, depth)
            self.stats['dirs'] += 1
            return
        if fmt == stat.S_IFLNK:
            target = b''.join(self.fs.extent_bytes(e, self.notes) for e in rec['ext'])
            target = target[:rec['inode']['size']].decode('utf-8', 'surrogateescape')
            link = target
            if target.startswith('/') and not self.args.keep_absolute_symlinks:
                # lxrun resolves a guest's paths on the host, so an absolute
                # link would leave the root (scripts/mkroot-rpm.sh does the same).
                link = os.path.relpath(os.path.join(self.out, target.lstrip('/')),
                                       os.path.dirname(path))
                self.manifest['symlinks_rewritten'][r] = target
            if os.path.lexists(path):
                os.unlink(path)
            os.symlink(link, path)
            self.record_owner(r, rec)
            self.stats['symlinks'] += 1
            return
        if fmt != stat.S_IFREG:
            kind = {stat.S_IFCHR: 'chrdev', stat.S_IFBLK: 'blkdev', stat.S_IFIFO: 'fifo',
                    stat.S_IFSOCK: 'socket'}.get(fmt, 'unknown')
            rdev = rec['inode']['rdev']
            self.manifest['special'][r] = dict(kind=kind, mode=oct(mode), rdev=rdev,
                                               major=(rdev >> 20), minor=rdev & 0xFFFFF)
            self.stats['special'] += 1
            return
        key = (subvol, ino)
        if rec['inode']['nlink'] > 1 and key in self.hardlinks:
            if os.path.lexists(path):
                os.unlink(path)
            os.link(self.hardlinks[key], path)
            self.stats['hardlinks'] += 1
            return
        size = rec['inode']['size']
        if (self.args.resume and os.path.isfile(path) and not os.path.islink(path)
                and os.path.getsize(path) == size
                and abs(os.path.getmtime(path) - rec['inode']['mtime']) < 1):
            self.stats['skipped'] += 1
        else:
            self.write_file(path, rec, size)
        self.hardlinks[key] = path
        self.finish(rec, path)
        self.stats['files'] += 1
        self.stats['bytes'] += size

    def write_file(self, path, rec, size):
        tmp = path + '.steamarm-part'
        with open(tmp, 'wb') as f:
            for e in rec['ext']:
                if e['off'] >= size:
                    break
                if e['type'] != 0 and (e['type'] == 2 or e['disk'] == 0):
                    continue                    # hole or prealloc: leave sparse
                data = self.fs.extent_bytes(e, self.notes)
                data = data[:max(0, size - e['off'])]
                f.seek(e['off'])
                f.write(data)
            f.truncate(size)
        os.replace(tmp, path)

    def record_owner(self, r, rec):
        i = rec['inode']
        if i['uid'] or i['gid']:
            self.manifest['owners'][r] = [i['uid'], i['gid']]
        if rec['xattr']:
            self.manifest['xattrs'][r] = {k: v.hex() for k, v in rec['xattr'].items()}

    def finish(self, rec, path, is_dir=False):
        r = self.rel(path) if path != self.out else '.'
        self.record_owner(r, rec)
        i = rec['inode']
        if i is None:
            return
        perm = stat.S_IMODE(i['mode'])
        try:
            # Keep directories writable for ourselves so later files can land.
            os.chmod(path, perm | (0o700 if is_dir else 0o200))
        except OSError:
            pass
        if perm & 0o7000 or (is_dir and perm & 0o200 == 0) or (not is_dir and perm & 0o200 == 0):
            self.manifest.setdefault('modes', {})[r] = oct(perm)
        try:
            os.utime(path, (i['atime'], i['mtime']), follow_symlinks=False)
        except OSError:
            pass


def cmd_extract(args):
    img = Image(args.image)
    p, fs = pick_btrfs(img, args.part)
    sv = choose_subvol(fs, args)
    out = os.path.abspath(args.out)
    print(f'partition {p["index"]} ({p["name"]}), subvolume {sv} -> {out}', file=sys.stderr)
    ex = Extractor(fs, out, args)
    mpath = ex.run(sv)
    s, n = ex.stats, ex.notes
    print(f'  {s["files"]} files ({human(s["bytes"])}), {s["dirs"]} dirs, {s["symlinks"]} symlinks, '
          f'{s["hardlinks"]} hardlinks, {s["special"]} special files (manifest only)'
          f'{", " + str(s["skipped"]) + " unchanged" if s["skipped"] else ""}')
    if n:
        print('  decompression notes: ' + ', '.join(f'{k} {v}' for k, v in n.items()))
    m = ex.manifest
    if m['case_collisions']:
        print(f'  {len(m["case_collisions"])} names differ only in case and were skipped (manifest)')
    if m['errors']:
        print(f'  {len(m["errors"])} errors; first: {m["errors"][0]}')
    print(f'  manifest {mpath}')
    bad = sum(v for k, v in n.items() if k != 'frame-larger-than-ram') + len(m['errors'])
    sys.exit(1 if bad else 0)


# ---------------------------------------------------------------- inventory
EM = {3: 'i386', 40: 'arm', 62: 'x86-64', 183: 'aarch64', 243: 'riscv'}
SKIP_TOP = {'proc', 'sys', 'dev', 'run', 'tmp'}


def elf_info(path):
    try:
        with open(path, 'rb') as f:
            h = f.read(64)
            if len(h) < 52 or h[:4] != b'\x7fELF':
                return None
            is64, le = h[4] == 2, h[5] == 1
            if not le:
                return dict(machine='big-endian', interp=None, type=None, aligns=[])
            etype, machine = u16(h, 16), u16(h, 18)
            interp, aligns = None, []
            if is64:
                phoff, phentsize, phnum = u64(h, 32), u16(h, 54), u16(h, 56)
            else:
                phoff, phentsize, phnum = u32(h, 28), u16(h, 42), u16(h, 44)
            if phnum and phentsize and phnum < 256:
                f.seek(phoff)
                ph = f.read(phentsize * phnum)
                for i in range(phnum):
                    o = i * phentsize
                    if len(ph) < o + (56 if is64 else 32):
                        continue
                    if u32(ph, o) == 1:                       # PT_LOAD: p_align
                        aligns.append(u64(ph, o + 48) if is64 else u32(ph, o + 28))
                    if u32(ph, o) != 3:
                        continue
                    off, sz = (u64(ph, o + 8), u64(ph, o + 32)) if is64 else (u32(ph, o + 4), u32(ph, o + 16))
                    if sz < 4096:
                        f.seek(off)
                        interp = f.read(sz).rstrip(b'\0').decode('utf-8', 'replace')
            return dict(machine=EM.get(machine, str(machine)), type={2: 'exec', 3: 'dyn'}.get(etype, etype),
                        interp=interp, aligns=aligns)
    except OSError:
        return None


def pacman_packages(root):
    for d in ('usr/lib/holo/pacmandb/local', 'var/lib/pacman/local',
              'usr/share/factory/var/lib/pacman/local', 'usr/lib/pacman/local'):
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        pkgs = []
        for ent in sorted(os.listdir(base)):
            desc = os.path.join(base, ent, 'desc')
            if not os.path.isfile(desc):
                continue
            fields, cur = {}, None
            for line in open(desc, encoding='utf-8', errors='replace'):
                line = line.rstrip('\n')
                if line.startswith('%') and line.endswith('%'):
                    cur = line.strip('%')
                    fields[cur] = []
                elif line and cur:
                    fields[cur].append(line)
            one = lambda k: (fields.get(k) or [''])[0]
            pkgs.append(dict(name=one('NAME'), version=one('VERSION'), arch=one('ARCH'),
                             license=fields.get('LICENSE', []), url=one('URL'),
                             packager=one('PACKAGER'), size=int(one('SIZE') or 0),
                             desc=one('DESC')))
        return d, pkgs
    return None, []


def read_kv(path):
    out = {}
    try:
        for line in open(path, encoding='utf-8', errors='replace'):
            if '=' in line and not line.lstrip().startswith('#'):
                k, v = line.rstrip('\n').split('=', 1)
                out[k.strip()] = v.strip().strip('"')
    except OSError:
        pass
    return out


# name -> path substrings (relative to the root) that identify the component
COMPONENTS = {
    'steam-client': ['usr/bin/steam', 'usr/lib/steam/', 'usr/share/steam/', 'bin_steam.sh', 'steamos-steam'],
    'steamwebhelper/cef': ['steamwebhelper', 'libcef.so'],
    'fex': ['FEXInterpreter', 'FEXServer', 'libFEXCore', 'fex-emu', 'binfmt.d/fex', 'FEXLoader'],
    'proton': ['proton', 'compatibilitytools.d'],
    'steam-linux-runtime': ['SteamLinuxRuntime', 'steamrt', 'pressure-vessel'],
    'gamescope': ['gamescope'],
    'lepton': ['lepton', 'Lepton'],
    'vulkan-icd': ['share/vulkan/icd.d/'],
    'mesa': ['libvulkan_freedreno', 'libvulkan_radeon', 'dri/', 'libgallium', 'libEGL_mesa', 'libGLX_mesa'],
    'nss': ['libnss3.so', 'libsoftokn3.so', 'libfreeblpriv3.so', 'libnssckbi.so', 'libnspr4.so'],
    'x11': ['libX11.so', 'Xwayland', 'libxcb.so'],
    'audio': ['pipewire', 'libpulse', 'wireplumber'],
    'qualcomm/hardware': ['qcom', 'adreno', 'firmware/qcom', 'hexagon', 'fastrpc'],
}


HOST_PAGE = 0x4000       # Darwin arm64; runtime/elf.c refuses program segments not aligned to it
PAGE_FLAGS = {1: '4K', 2: '16K', 3: '64K'}


def kernel_pages(root):
    """Page size the image's kernel was built for: CONFIG_ARM64_*_PAGES from a
    kernel config, and the page-size field of an arm64 Image header (flags
    bits 1-2, Documentation/arch/arm64/booting.rst), gzip-wrapped or raw."""
    import glob
    import gzip
    out = []
    for pat in ('usr/lib/modules/*/config', 'usr/lib/modules/*/build/.config', 'boot/config*'):
        for p in sorted(glob.glob(os.path.join(root, pat))):
            cfg = read_kv(p)
            page = next((k[len('CONFIG_ARM64_'):-len('_PAGES')] for k, v in cfg.items()
                         if v == 'y' and k.startswith('CONFIG_ARM64_') and k.endswith('_PAGES')
                         and k[len('CONFIG_ARM64_'):-len('_PAGES')] in ('4K', '16K', '64K')), None)
            out.append(dict(file=os.path.relpath(p, root), source='config', page=page,
                            va_bits=cfg.get('CONFIG_ARM64_VA_BITS')))
    for pat in ('usr/lib/modules/*/vmlinuz', 'usr/lib/modules/*/Image*', 'boot/Image*', 'boot/vmlinuz*'):
        for p in sorted(glob.glob(os.path.join(root, pat))):
            try:
                with open(p, 'rb') as f:
                    head = f.read(1 << 20)
            except OSError:
                continue
            kind = 'raw'
            if head[:2] == b'\x1f\x8b':
                kind = 'gzip'
                try:
                    head = gzip.GzipFile(fileobj=__import__('io').BytesIO(head)).read(64)
                except (OSError, EOFError):
                    head = b''
            elif head[:2] == b'MZ' and head[4:8] == b'zimg':
                out.append(dict(file=os.path.relpath(p, root), source='EFI zboot (compressed)', page=None))
                continue
            if len(head) >= 64 and head[0x38:0x3c] == b'ARM\x64':
                page = PAGE_FLAGS.get((u64(head, 0x18) >> 1) & 3, 'unspecified')
                out.append(dict(file=os.path.relpath(p, root), source=f'Image header ({kind})', page=page))
            else:
                out.append(dict(file=os.path.relpath(p, root), source=f'not an arm64 Image ({kind})', page=None))
    return out


def pacman_repos(root):
    """[repo] sections of etc/pacman.conf with their Server URLs ($repo/$arch
    expanded), following Include= files inside the root."""
    conf = os.path.join(root, 'etc/pacman.conf')
    if not os.path.isfile(conf):
        return []
    arch = 'aarch64'
    repos, cur = [], None

    def servers_from(path, name):
        out = []
        try:
            for line in open(path, encoding='utf-8', errors='replace'):
                line = line.split('#', 1)[0].strip()
                if line.startswith('Server'):
                    url = line.split('=', 1)[1].strip()
                    out.append(url.replace('$repo', name).replace('$arch', arch))
        except OSError:
            pass
        return out

    for line in open(conf, encoding='utf-8', errors='replace'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        if line.startswith('[') and line.endswith(']'):
            name = line[1:-1]
            cur = None if name == 'options' else dict(name=name, servers=[])
            if cur:
                repos.append(cur)
        elif '=' in line:
            k, v = (x.strip() for x in line.split('=', 1))
            if k == 'Architecture' and v not in ('auto', ''):
                arch = v.split()[0]
            elif cur and k == 'Server':
                cur['servers'].append(v.replace('$repo', cur['name']).replace('$arch', arch))
            elif cur and k == 'Include':
                cur['servers'] += servers_from(os.path.join(root, v.lstrip('/')), cur['name'])
    return repos


def read_repo_db(src):
    """Packages of a pacman sync database (<repo>.db: a tar, gzip/xz/bz2/zstd
    compressed), from a local file or an https URL."""
    import io
    import tarfile
    import urllib.request
    if src.startswith(('http://', 'https://')):
        with urllib.request.urlopen(src, timeout=60) as r:
            raw = r.read()
    else:
        raw = open(src, 'rb').read()
    if raw[:4] == b'\x28\xb5\x2f\xfd':
        global _ZSTD
        if _ZSTD is None:
            zstd_decode(b'', 0)            # resolves the zstandard import
        if not _ZSTD:
            die('this repo database is zstd-compressed: pip3 install zstandard')
        raw = _ZSTD.ZstdDecompressor().decompressobj().decompress(raw)
    pkgs = {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as tf:
        for m in tf.getmembers():
            if not m.name.endswith('/desc'):
                continue
            fields, cur = {}, None
            for line in tf.extractfile(m).read().decode('utf-8', 'replace').splitlines():
                if line.startswith('%') and line.endswith('%'):
                    cur = line.strip('%')
                    fields[cur] = []
                elif line and cur:
                    fields[cur].append(line)
            one = lambda k: (fields.get(k) or [''])[0]
            pkgs[one('NAME')] = dict(version=one('VERSION'), arch=one('ARCH'),
                                     filename=one('FILENAME'), sha256=one('SHA256SUM'),
                                     license=fields.get('LICENSE', []))
    return pkgs


def cmd_compare(args):
    """Image packages (inventory --json) against one or more repo databases."""
    inv = json.load(open(args.inventory))
    image = {p['name']: p for p in inv.get('packages', [])}
    repos = {}
    for src in args.repo:
        name = os.path.basename(src.rstrip('/')).split('.db')[0]
        for pkg, info in read_repo_db(src).items():
            repos.setdefault(pkg, (name, info))
    rows, counts = [], collections.Counter()
    for pkg in sorted(set(image) | set(repos)):
        iv = image.get(pkg, {}).get('version', '')
        rname, rinfo = repos.get(pkg, ('', {}))
        rv = rinfo.get('version', '')
        if iv and rv:
            state = 'same' if iv == rv else 'differs'
        else:
            state = 'image-only' if iv else 'repo-only'
        counts[state] += 1
        arch = image.get(pkg, {}).get('arch') or rinfo.get('arch', '')
        rows.append((pkg, iv, rv, rname, arch, state))
    L = ['## Steam Frame image vs repository (generated by steamframe-image.py compare)\n',
         ', '.join(f'{k} {v}' for k, v in sorted(counts.items())) + '\n',
         '| package | image | repository | repo | arch | state |', '|---|---|---|---|---|---|']
    for r in rows:
        if args.all or r[5] != 'same':
            L.append('| ' + ' | '.join(r) + ' |')
    md = '\n'.join(L) + '\n'
    if args.md:
        open(args.md, 'w').write(md)
    print(md if not args.md else f'{sum(counts.values())} packages: {dict(counts)} -> {args.md}')


def cmd_inventory(args):
    root = os.path.abspath(args.root)
    rep = dict(root=root)
    rep['os_release'] = read_kv(os.path.join(root, 'etc/os-release')) or read_kv(
        os.path.join(root, 'usr/lib/os-release'))
    db, pkgs = pacman_packages(root)
    rep['pacman_db'] = db
    rep['packages'] = pkgs
    elf = collections.Counter()
    interps = collections.Counter()
    non_arm = []
    comps = collections.defaultdict(list)
    libs = {}
    align, exec_a64, small = collections.Counter(), [], []
    nfiles = 0
    for dp, dns, fns in os.walk(root):
        rel = os.path.relpath(dp, root)
        if rel == '.':
            dns[:] = [d for d in dns if d not in SKIP_TOP]
        for fn in fns:
            path = os.path.join(dp, fn)
            relp = os.path.normpath(os.path.join(rel, fn))
            nfiles += 1
            for comp, pats in COMPONENTS.items():
                if any(p in relp for p in pats) and len(comps[comp]) < 400:
                    comps[comp].append(relp)
            if os.path.islink(path) or not os.path.isfile(path):
                continue
            info = elf_info(path)
            if not info:
                continue
            elf[info['machine']] += 1
            if info['interp']:
                interps[(info['machine'], info['interp'])] += 1
            if info['machine'] != 'aarch64':
                non_arm.append(dict(path=relp, machine=info['machine'], interp=info['interp']))
            else:
                # What lxrun can load: runtime/elf.c refuses ET_EXEC and, for the
                # programs it maps itself (executables and ld.so), segments not
                # aligned to the 16 KiB host page; libraries mapped by ld.so with
                # smaller alignment go through runtime/subpage.c instead.
                if info['aligns']:
                    align[min(info['aligns'])] += 1
                if info['type'] == 'exec':
                    exec_a64.append(relp)
                elif info['aligns'] and min(info['aligns']) % HOST_PAGE:
                    small.append(dict(path=relp, p_align=min(info['aligns']),
                                      program=bool(info['interp']) or 'ld-linux' in fn))
            if fn.startswith(('libc.so.6', 'libstdc++.so.6.', 'libnss3.so', 'libsoftokn3.so',
                              'libfreeblpriv3.so', 'libvulkan.so.1')):
                libs.setdefault(fn, []).append(relp)
    rep['files'] = nfiles
    rep['elf_by_machine'] = dict(elf)
    rep['interpreters'] = [dict(machine=m, interp=i, count=c) for (m, i), c in interps.most_common()]
    rep['non_aarch64_elf'] = non_arm
    rep['aarch64_loading'] = dict(
        min_p_align={hex(a): c for a, c in sorted(align.items())},
        et_exec=sorted(exec_a64),
        below_host_page=sorted(small, key=lambda x: (not x['program'], x['path'])))
    rep['kernel_pages'] = kernel_pages(root)
    rep['components'] = {k: sorted(v) for k, v in comps.items()}
    rep['key_libraries'] = libs
    icds = []
    for d in ('usr/share/vulkan/icd.d', 'etc/vulkan/icd.d'):
        base = os.path.join(root, d)
        if os.path.isdir(base):
            for fn in sorted(os.listdir(base)):
                try:
                    j = json.load(open(os.path.join(base, fn)))
                    icds.append(dict(file=f'{d}/{fn}', library=j.get('ICD', {}).get('library_path'),
                                     api=j.get('ICD', {}).get('api_version')))
                except (OSError, ValueError):
                    icds.append(dict(file=f'{d}/{fn}', library=None, api=None))
    rep['vulkan_icds'] = icds
    binfmt = []
    for d in ('usr/lib/binfmt.d', 'etc/binfmt.d'):
        base = os.path.join(root, d)
        if os.path.isdir(base):
            for fn in sorted(os.listdir(base)):
                try:
                    binfmt.append(dict(file=f'{d}/{fn}', text=open(os.path.join(base, fn)).read().strip()))
                except OSError:
                    pass
    rep['binfmt'] = binfmt
    units = []
    for d in ('usr/lib/systemd/system', 'usr/lib/systemd/user', 'etc/systemd/system'):
        base = os.path.join(root, d)
        if os.path.isdir(base):
            for fn in sorted(os.listdir(base)):
                if any(k in fn.lower() for k in ('steam', 'fex', 'lepton', 'gamescope', 'holo', 'jupiter', 'frame')):
                    units.append(f'{d}/{fn}')
    rep['systemd_units'] = units
    rep['pacman_repos'] = pacman_repos(root)
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(rep, f, indent=1, sort_keys=True)
    md = inventory_markdown(rep)
    if args.md:
        with open(args.md, 'w') as f:
            f.write(md)
    else:
        print(md)


def inventory_markdown(rep):
    L = []
    osr = rep['os_release']
    L.append('## Inventory (generated by scripts/steamframe-image.py inventory)\n')
    L.append('| | |\n|---|---|')
    for k in ('NAME', 'ID', 'ID_LIKE', 'VERSION_ID', 'BUILD_ID', 'VARIANT_ID', 'VERSION_CODENAME'):
        if k in osr:
            L.append(f'| {k} | `{osr[k]}` |')
    L.append(f'| files | {rep["files"]} |')
    L.append(f'| pacman db | `{rep["pacman_db"]}` ({len(rep["packages"])} packages) |')
    L.append('')
    L.append('### ELF files by machine\n')
    L.append('| machine | files |\n|---|---|')
    for m, c in sorted(rep['elf_by_machine'].items(), key=lambda kv: -kv[1]):
        L.append(f'| {m} | {c} |')
    L.append('\n### Program interpreters\n')
    L.append('| machine | PT_INTERP | files |\n|---|---|---|')
    for x in rep['interpreters'][:20]:
        L.append(f'| {x["machine"]} | `{x["interp"]}` | {x["count"]} |')
    ld = rep.get('aarch64_loading')
    if ld:
        progs = [x for x in ld['below_host_page'] if x['program']]
        L.append('\n### Loading under lxrun (aarch64 ELF)\n')
        L.append('Smallest PT_LOAD p_align per file: ' +
                 ', '.join(f'{a} {c}' for a, c in ld['min_p_align'].items()))
        L.append(f'\n- ET_EXEC (lxrun refuses them, runtime/elf.c): {len(ld["et_exec"])}')
        for p in ld['et_exec'][:20]:
            L.append(f'  - `{p}`')
        L.append(f'- executables (PT_INTERP) and ld.so below the 16 KiB host page '
                 f'(refused when lxrun runs them): {len(progs)}')
        for x in progs[:20]:
            L.append(f'  - `{x["path"]}` p_align {x["p_align"]:#x}')
        L.append(f'- libraries below the 16 KiB host page (runtime/subpage.c path): '
                 f'{len(ld["below_host_page"]) - len(progs)}')
        for x in rep.get('kernel_pages', []):
            L.append(f'- kernel `{x["file"]}` ({x["source"]}): page size {x["page"] or "unknown"}'
                     + (f', VA bits {x["va_bits"]}' if x.get('va_bits') else ''))
    L.append('\n### Components found (paths, first 12 each)\n')
    for k, v in sorted(rep['components'].items()):
        L.append(f'- **{k}** ({len(v)}): ' + ', '.join(f'`{p}`' for p in v[:12]))
    L.append('\n### Key libraries\n')
    for k, v in sorted(rep['key_libraries'].items()):
        L.append(f'- `{k}`: ' + ', '.join(f'`{p}`' for p in v[:6]))
    L.append('\n### Vulkan ICDs\n')
    for x in rep['vulkan_icds']:
        L.append(f'- `{x["file"]}` -> `{x["library"]}` (API {x["api"]})')
    L.append('\n### binfmt_misc (how x86 is dispatched)\n')
    for x in rep['binfmt']:
        L.append(f'- `{x["file"]}`: `{x["text"][:160]}`')
    L.append(f'\n### Non-AArch64 ELF files: {len(rep["non_aarch64_elf"])}\n')
    by = collections.Counter(os.path.dirname(x['path']) for x in rep['non_aarch64_elf'])
    for d, c in by.most_common(25):
        L.append(f'- `{d}/` {c}')
    L.append('\n### pacman repositories (etc/pacman.conf)\n')
    for r in rep.get('pacman_repos', []):
        L.append(f'- `[{r["name"]}]`: ' + ', '.join(f'`{u}`' for u in r['servers'][:3]))
    L.append('\n### Packages (name, version, arch, license)\n')
    L.append('| package | version | arch | license |\n|---|---|---|---|')
    for p in rep['packages']:
        L.append(f'| {p["name"]} | {p["version"]} | {p["arch"]} | {" ".join(p["license"])[:40]} |')
    L.append('')
    return '\n'.join(L)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('info', help='partitions, filesystems, btrfs superblock, subvolumes')
    a.add_argument('image')
    a.add_argument('--hash', action='store_true', help='also compute the sha256 of the image')
    a.set_defaults(func=cmd_info)
    a = sub.add_parser('diagnose', help='check every compressed extent')
    a.add_argument('image')
    a.add_argument('--part', type=int)
    a.add_argument('--subvol-id', type=int)
    a.add_argument('--examples', type=int, default=5)
    a.add_argument('--json')
    a.set_defaults(func=cmd_diagnose)
    a = sub.add_parser('extract', help='copy the filesystem tree out, read-only')
    a.add_argument('image')
    a.add_argument('out')
    a.add_argument('--part', type=int)
    a.add_argument('--subvol-id', type=int, help='default: the default subvolume')
    a.add_argument('--no-subvols', action='store_true', help='do not descend into nested subvolumes')
    a.add_argument('--keep-absolute-symlinks', action='store_true')
    a.add_argument('--resume', action='store_true', help='skip files already extracted (same size and mtime)')
    a.set_defaults(func=cmd_extract)
    a = sub.add_parser('inventory', help='describe an extracted root')
    a.add_argument('root')
    a.add_argument('--json')
    a.add_argument('--md')
    a.set_defaults(func=cmd_inventory)
    a = sub.add_parser('compare', help='image packages vs pacman repo databases (holo-core-aarch64-preview, ...)')
    a.add_argument('inventory', help='JSON written by `inventory --json`')
    a.add_argument('repo', nargs='+', help='<repo>.db file or https URL (see the repositories inventory lists)')
    a.add_argument('--md')
    a.add_argument('--all', action='store_true', help='also list packages whose versions match')
    a.set_defaults(func=cmd_compare)
    args = ap.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
