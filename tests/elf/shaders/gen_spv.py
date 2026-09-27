#!/usr/bin/env python3
"""Compile the triangle shaders and embed them as C arrays.

Regenerate with:  python3 tests/elf/shaders/gen_spv.py
Needs glslangValidator (Homebrew's glslang, or the Vulkan SDK).
"""
import pathlib, shutil, struct, subprocess, sys

here = pathlib.Path(__file__).parent
out = here.parent / "tri_spv.h"
gv = shutil.which("glslangValidator") or shutil.which("glslang")
if not gv:
    sys.exit("glslangValidator not found")

def compile_one(stage):
    src = here / f"tri.{stage}"
    spv = here / f"tri.{stage}.spv"
    subprocess.run([gv, "-V", str(src), "-o", str(spv)], check=True,
                   stdout=subprocess.DEVNULL)
    d = spv.read_bytes()
    spv.unlink()
    return struct.unpack(f"<{len(d)//4}I", d)

def emit(words, name):
    lines = [f"static const uint32_t {name}[] = {{"]
    for i in range(0, len(words), 8):
        lines.append("    " + " ".join(f"0x{w:08x}," for w in words[i:i+8]))
    lines.append("};")
    return "\n".join(lines)

text = ("/* GENERATED from tests/elf/shaders/tri.{vert,frag}. Do not edit. */\n"
        "#include <stdint.h>\n\n"
        + emit(compile_one("vert"), "k_vert_spv") + "\n\n"
        + emit(compile_one("frag"), "k_frag_spv") + "\n")
out.write_text(text)
print(f"wrote {out}")
