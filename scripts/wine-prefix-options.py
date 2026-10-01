#!/usr/bin/env python3
"""Wine options SteamARM keeps in the Wine prefixes of games and apps.

  scripts/wine-prefix-options.py emulate-modeset on|off [USER_REG...]

emulate-modeset  HKCU\\Software\\Wine\\X11 Driver "EmulateModeset": Wine's
                 display-mode emulation (win32u). A game that asks for a lower
                 full-screen resolution gets it virtually: it renders at that
                 size and Wine stretches the picture over the whole screen
                 (the launcher's "Escala de resolución"). MEASURED with
                 tests/win/modeset.c: 1280x720 drawn in a 1600x900 X window;
                 without it the real mode change stalled (2 frames).
                 benchmarks/stage43-settings-presets.txt.

Without USER_REG: every user.reg of the Steam game prefixes (compatdata) in
the x86 Steam root and both ARM64 roots, and of the launcher's Windows apps.
A prefix is only changed while no wineserver runs (it rewrites user.reg when
it exits); the caller checks. "off" over the defaults undoes only what an
earlier "on" did (a marker in the launcher's directory). Prints one line per
changed file.
"""
import glob
import os
import re
import sys
import time

STATE = os.environ.get("STEAMARM_STATE") or os.path.expanduser("~/SteamARM-roots")
SECTION = r"Software\\Wine\\X11 Driver"     # as user.reg spells it (backslashes doubled)
VALUE = "EmulateModeset"


def default_files(state=STATE):
    pats = []
    for root, home in (("steamroot", "fexhome"), ("arm64root", "armhome"), ("armroot", "armhome")):
        base = os.path.join(state, root, "tmp", home)
        pats.append(os.path.join(base, ".local/share/Steam/steamapps/compatdata/*/pfx/user.reg"))
    pats.append(os.path.join(state, "steamroot/tmp/fexhome/.steamarm/prefixes/*/pfx/user.reg"))
    out = []
    for p in pats:
        out.extend(sorted(glob.glob(p)))
    return out


def set_value(text, on, now=None):
    """user.reg text with EmulateModeset set ("Y") or removed. Unchanged text
    when it already is so."""
    lines = text.split("\n")
    header = re.compile(r"^\[" + re.escape(SECTION) + r"\](\s|$)", re.IGNORECASE)
    start = next((i for i, l in enumerate(lines) if header.match(l)), None)
    entry = '"%s"="Y"' % VALUE
    if start is None:
        if not on:
            return text
        stamp = int(now if now is not None else time.time())
        tail = "" if text.endswith("\n") else "\n"
        return text + tail + "\n[%s] %d\n%s\n" % (SECTION, stamp, entry)
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("[")), len(lines))
    idx = next((i for i in range(start + 1, end) if lines[i].lower().startswith('"%s"=' % VALUE.lower())), None)
    if on:
        if idx is not None:
            if lines[idx] == entry:
                return text
            lines[idx] = entry
        else:
            # After the section's #time line, if there is one.
            at = start + 1
            while at < end and lines[at].startswith("#"):
                at += 1
            lines.insert(at, entry)
    else:
        if idx is None:
            return text
        del lines[idx]
    return "\n".join(lines)


def apply(path, on):
    try:
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
    except OSError:
        return False
    new = set_value(text, on)
    if new == text:
        return False
    tmp = path + ".steamarm-tmp"
    with open(tmp, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(new)
    os.replace(tmp, path)
    return True


# Without USER_REG, "off" undoes only what "on" did: the marker records that
# this script set the value, so one set by hand in a prefix is left alone.
MARKER = os.path.join(STATE, "launcher", "emulate-modeset.applied")


def main(argv):
    if len(argv) < 3 or argv[1] != "emulate-modeset" or argv[2] not in ("on", "off"):
        sys.stderr.write(__doc__)
        return 2
    on = argv[2] == "on"
    explicit = argv[3:]
    if not explicit and not on and not os.path.exists(MARKER):
        return 0
    files = explicit or default_files()
    for f in files:
        if apply(f, on):
            print("%s: EmulateModeset %s" % (f, "Y" if on else "removed"))
    if not explicit:
        if on:
            os.makedirs(os.path.dirname(MARKER), exist_ok=True)
            open(MARKER, "w").close()
        elif os.path.exists(MARKER):
            os.remove(MARKER)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
