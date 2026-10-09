#!/usr/bin/env python3
"""Mark a game's Steam install-script steps as done, for the native ARM64 tool.

Before a game's first start the Steam client runs
legacycompat/iscriptevaluator.exe on legacycompat/evaluatorscript_<appid>.vdf
through the game's compatibility tool. The evaluator is 64-bit, but it hands
the steps (redistributable installers, the Gaming Runtime installer, firewall
rules) to legacycompat/SteamService.exe, which is a 32-bit program, and the
native path on macOS cannot run 32-bit Windows programs (WoW64 needs the low
4 GiB, which macOS reserves): SteamService aborted in build_wow64_parameters
and the evaluator, and with it the game's launch, waited forever (Minecraft
Dungeons II). The installers are 32-bit too, or cover runtimes Proton's
builtins already provide (the Visual C++ runtime, DirectX).

So the tool does not run the evaluator: this writes every step's "has run"
registry value -- what the evaluator itself checks before running a step --
into a .reg file, and the tool imports it with regedit and reports success.
Both registry views are written, since the 32-bit SteamService reads
Wow6432Node (as Proton's default prefix marks .NET and XNA).

    installscript.py EVALUATORSCRIPT.vdf OUT.reg
Exit 0: OUT.reg written (the steps are listed on stderr); 2: nothing to mark.
"""
import re
import sys


def parse_vdf(text):
    """Valve KeyValues text -> nested dicts (later keys win; look keys up with ci())."""
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text)
    pos = 0

    def block():
        nonlocal pos
        out = {}
        while pos < len(tokens):
            s, brace = tokens[pos]
            pos += 1
            if brace == "}":
                return out
            key = s
            if pos < len(tokens) and tokens[pos][1] == "{":
                pos += 1
                out[key] = block()
            elif pos < len(tokens):
                out[key] = tokens[pos][0]
                pos += 1
        return out

    return block()


def ci(d, key, default=None):
    """d[key], ignoring case (KeyValues keys are case-insensitive)."""
    for k, v in d.items():
        if k.lower() == key:
            return v
    return default


def unescape(s):
    return s.replace("\\\\", "\\")


def steps(tree):
    """(registry key, value name, minimum) for every run-process step with a has-run key."""
    root = ci(tree, "evaluatorscript", tree)
    for entry in root.values():
        if not isinstance(entry, dict):
            continue
        script = ci(entry, "compat_installscript") or ci(entry, "installscript") or {}
        for section_name, section in script.items():
            if section_name.lower() != "run process" or not isinstance(section, dict):
                continue
            for name, step in section.items():
                if not isinstance(step, dict):
                    continue
                key = ci(step, "hasrunkey")
                if not key:
                    continue
                value = ci(step, "runkeyname") or name
                try:
                    minimum = max(1, int(ci(step, "minimumhasrunvalue", "1")))
                except ValueError:
                    minimum = 1
                yield unescape(key), value, minimum, ci(entry, "appid", "?")


def views(key):
    """The key as written, and its 32-bit view."""
    m = re.match(r"(HKEY_LOCAL_MACHINE|HKLM)\\Software\\(.*)$", key, re.I)
    if not m:
        return [key]
    rest = m.group(2)
    if rest.lower().startswith("wow6432node\\"):
        return ["HKEY_LOCAL_MACHINE\\Software\\" + rest]
    return ["HKEY_LOCAL_MACHINE\\Software\\" + rest, "HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\" + rest]


def main(argv):
    if len(argv) != 3:
        print("usage: installscript.py EVALUATORSCRIPT.vdf OUT.reg", file=sys.stderr)
        return 1
    with open(argv[1], encoding="utf-8", errors="replace") as f:
        tree = parse_vdf(f.read())
    lines = ["REGEDIT4", ""]
    n = 0
    for key, value, minimum, appid in steps(tree):
        for k in views(key):
            lines += ["[%s]" % k, '"%s"=dword:%08x' % (value.replace('"', '\\"'), minimum), ""]
        print("steamarm-native-proton: install script of app %s: \"%s\" marked done, not run (%s)"
              % (appid, value, key), file=sys.stderr)
        n += 1
    if not n:
        return 2
    with open(argv[2], "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
