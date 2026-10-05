#!/usr/bin/env python3
"""Mark a PE file as a Wine builtin, as `winebuild --builtin` does (no winebuild here).

    wine-builtin-mark.py [--prefer-native] FILE...
    wine-builtin-mark.py --check FILE...

What Wine checks (Proton experimental_11.0 sources):
  - server/mapping.c get_image_params: reads the 64-byte DOS header plus the
    next 32 bytes; image.wine_builtin = the bytes at 0x40 equal
    "Wine builtin DLL" with its NUL (17 bytes), and the file has those 96 bytes.
  - dlls/ntdll/unix/virtual.c virtual_map_builtin_module: a file found in
    WINEDLLPATH without that flag is skipped ("found in WINEDLLPATH but not a
    builtin, ignoring"); with prefer_native (load order "default", no
    override) and DllCharacteristics & 0x0010 (Wine's
    IMAGE_DLLCHARACTERISTICS_PREFER_NATIVE) the builtin is skipped too.
winebuild's make_builtin_files (tools/winebuild/spec32.c) writes the 32-byte,
NUL-padded signature at 0x40 after checking e_lfanew >= 0x40 + 32, and for
--prefer-native ORs 0x0010 into DllCharacteristics at e_lfanew + 0x5e. Same here.
"""
import struct
import sys

SIG = b"Wine builtin DLL".ljust(32, b"\0")
PREFER_NATIVE = 0x0010


def headers(data, name):
    if data[:2] != b"MZ":
        sys.exit(f"{name}: not an MZ file")
    lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[lfanew:lfanew + 4] != b"PE\0\0":
        sys.exit(f"{name}: no PE header at e_lfanew {lfanew:#x}")
    return lfanew


def main(argv):
    check = "--check" in argv
    prefer = "--prefer-native" in argv
    files = [a for a in argv if not a.startswith("--")]
    if not files:
        sys.exit(__doc__.strip().splitlines()[2])
    for name in files:
        with open(name, "rb") as f:
            data = bytearray(f.read())
        lfanew = headers(data, name)
        pos = lfanew + 0x5E
        charact = struct.unpack_from("<H", data, pos)[0]
        if check:
            marked = data[0x40:0x40 + 17] == SIG[:17]
            print(f"{name}: e_lfanew {lfanew:#x}, builtin marker {'yes' if marked else 'no'}, "
                  f"DllCharacteristics {charact:#06x} (prefer-native {'yes' if charact & PREFER_NATIVE else 'no'})")
            continue
        if lfanew < 0x40 + len(SIG):
            sys.exit(f"{name}: not enough space ({lfanew:#x}) for the Wine signature")
        data[0x40:0x40 + len(SIG)] = SIG
        if prefer:
            struct.pack_into("<H", data, pos, charact | PREFER_NATIVE)
        with open(name, "r+b") as f:
            f.write(data)


if __name__ == "__main__":
    main(sys.argv[1:])
