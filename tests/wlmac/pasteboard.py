#!/usr/bin/env python3
"""A named macOS pasteboard, never the general one (the clipboard of the
person at the Mac): `pasteboard.py NAME` prints its text, `pasteboard.py
NAME TEXT` replaces it. Through osascript's JavaScript bridge to AppKit."""
import subprocess
import sys

JXA = """ObjC.import('AppKit');
function run(argv) {
    var pb = $.NSPasteboard.pasteboardWithName(argv[0]);
    if (argv.length > 1) { pb.clearContents; pb.setStringForType($(argv[1]), $.NSPasteboardTypeString); return ''; }
    var s = pb.stringForType($.NSPasteboardTypeString);
    return s.isNil() ? '' : s.js;
}"""


def pasteboard(name, text=None):
    if not name:
        raise ValueError("a pasteboard name is required (never the general pasteboard)")
    argv = ["osascript", "-l", "JavaScript", "-e", JXA, name] + ([text] if text is not None else [])
    return subprocess.run(argv, capture_output=True, text=True, check=True).stdout.rstrip("\n")


if __name__ == "__main__":
    out = pasteboard(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    if len(sys.argv) == 2:
        print(out)
