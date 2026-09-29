# Sourced by scripts/mkarmroot.sh, scripts/mkframeroot.sh and
# scripts/run-steam-arm64.sh: paths, guests and the rebuild swap of a guest
# root. Bash 3.2 (macOS's /bin/bash); every function returns normally under
# `set -euo pipefail` unless it says it refuses.
#
# A rebuilt root is made beside the old one (ROOT.new) and swapped in. The
# runtime state (ROOT_KEEP: tmp/, which holds the client's home tmp/armhome,
# and opt/apps, what was installed there) is carried from the old root into
# the new one. Nothing that holds that state is deleted: root_recover puts
# back what an interrupted swap left in ROOT.new, and root_clear refuses to
# delete a leftover that still holds any.

ROOT_KEEP="tmp opt/apps"

# canon_path PATH
#   PATH as an absolute path with every symlink in it resolved (pwd -P) and
#   no trailing slash. A relative PATH is taken from $CANON_BASE (a script sets
#   it to its caller's directory before it cd's), else from $PWD. A tail that
#   does not exist yet is appended as written. Refused (status 1, nothing
#   printed): an empty PATH, "." or ".." in the tail that does not exist, and
#   a symlink that does not lead to a directory (dangling, or to a file).
canon_path() {
    local p="$1" rest="" b d
    [ -n "$p" ] || return 1
    case "$p" in /*) ;; *) p="${CANON_BASE:-$PWD}/$p" ;; esac
    while :; do
        while [ "${#p}" -gt 1 ] && [ "${p%/}" != "$p" ]; do p="${p%/}"; done
        if [ -d "$p" ]; then
            d=$(CDPATH='' cd -- "$p" 2>/dev/null && pwd -P) || return 1
            break
        fi
        if [ -L "$p" ]; then return 1; fi
        b="${p##*/}"
        case "$b" in ""|.|..) return 1 ;; esac
        rest="/$b$rest"
        p="${p%/*}"
        [ -n "$p" ] || p=/
    done
    [ "$d" != / ] || d=""
    d="$d$rest"
    printf '%s\n' "${d:-/}"
}

# path_within A B: A (canonical) is B (canonical) or lies inside it.
path_within() {
    [ "$2" = / ] && return 0
    case "$1/" in "$2"/*) return 0 ;; esac
    return 1
}

# existing_ancestor PATH (canonical): PATH itself when it exists, else the
# nearest directory above it that does (where mkdir -p would create it).
existing_ancestor() {
    local p="$1"
    while [ ! -e "$p" ] && [ "$p" != / ]; do p="${p%/*}"; [ -n "$p" ] || p=/; done
    printf '%s\n' "$p"
}

# procs_env VAR PID...: "PID VALUE" for each PID whose environment sets VAR.
# The environment is the one the process was exec'd with (what `ps -E` shows,
# read exactly, spaces included): /proc/PID/environ on Linux, KERN_PROCARGS2
# on macOS. macOS hides it for platform binaries (/bin/sleep, /bin/bash);
# lxrun is not one. A PID that is gone or unreadable is skipped.
procs_env() {
    local var="$1"; shift
    [ $# -gt 0 ] || return 0
    /usr/bin/python3 - "$var" "$@" <<'PY' 2>/dev/null || true
import ctypes, struct, sys

def env_of(pid):
    try:
        with open("/proc/%d/environ" % pid, "rb") as f:
            return f.read().split(b"\0")
    except OSError:
        pass
    try:
        libc = ctypes.CDLL(None)
        mib = (ctypes.c_int * 3)(1, 49, pid)      # CTL_KERN, KERN_PROCARGS2
        size = ctypes.c_size_t(0)
        if libc.sysctl(mib, 3, None, ctypes.byref(size), None, 0):
            return []
        buf = ctypes.create_string_buffer(size.value)
        if libc.sysctl(mib, 3, buf, ctypes.byref(size), None, 0):
            return []
    except Exception:
        return []
    raw = buf.raw[:size.value]
    if len(raw) < 4:
        return []
    argc = struct.unpack("i", raw[:4])[0]
    parts = raw[4:].split(b"\0")
    # The executable's path, NUL padding, argc arguments, the environment,
    # an empty string, then Apple's own strings.
    i = 1
    while i < len(parts) and parts[i] == b"":
        i += 1
    i += argc
    env = []
    for s in parts[i:]:
        if s == b"":
            break
        env.append(s)
    return env

want = sys.argv[1].encode() + b"="
for a in sys.argv[2:]:
    try:
        pid = int(a)
    except ValueError:
        continue
    for kv in env_of(pid):
        if kv.startswith(want):
            sys.stdout.write("%d %s\n" % (pid, kv[len(want):].decode("utf-8", "replace")))
            break
PY
}

# guests_on_root DIR: the PIDs of the guests (processes whose executable is
# $ROOTS_GUEST_NAME, default lxrun; tests use a name of their own) whose
# LXRT_ROOT resolves to the directory DIR resolves to. The runtime puts the
# LXRT_ROOT string in front of every guest path it looks up
# (runtime/dispatch.c, translate_one), so a guest started through a /tmp link
# is on whatever that link points at now.
guests_on_root() {
    local want pids pid r n="${ROOTS_GUEST_NAME:-lxrun}"
    want=$(canon_path "$1") || return 0
    pids=$(ps -Ao pid=,comm= | awk -v n="$n" '$NF == n || $NF ~ ("/" n "$") {print $1}')
    [ -n "$pids" ] || return 0
    # shellcheck disable=SC2086
    { procs_env LXRT_ROOT $pids || true; } | while read -r pid r; do
        if [ "$(CANON_BASE=/ canon_path "$r" 2>/dev/null)" = "$want" ]; then echo "$pid"; fi
    done
    return 0
}

# root_holds_state DIR [KEEP-DIR]: DIR holds runtime state that is in no
# image or stage: a guest home in tmp/ (tmp/armhome, tmp/fexhome, ...) or apps
# installed in opt/apps. With KEEP-DIR (tmp or opt/apps), only that one.
root_holds_state() {
    local h
    if [ "${2:-tmp}" = tmp ]; then
        for h in "$1"/tmp/*home; do
            if [ -e "$h" ] || [ -L "$h" ]; then return 0; fi
        done
    fi
    if [ "${2:-opt/apps}" = opt/apps ] && [ -d "$1/opt/apps" ] &&
       [ -n "$(ls -A "$1/opt/apps" 2>/dev/null)" ]; then
        return 0
    fi
    return 1
}

# The first line of ROOT.new/.lxrt-carried as root_swap writes it; then each
# state directory, on a line of its own, named before it moves. An older
# builder (scripts/mkframeroot.sh before roots.sh) left the file empty.
ROOT_CARRIED_HEAD="# root_swap (scripts/roots.sh): each directory below was named before it moved here"

# root_listed NEW: NEW/.lxrt-carried is root_swap's list.
root_listed() {
    [ -f "$1/.lxrt-carried" ] && [ "$(head -1 "$1/.lxrt-carried")" = "$ROOT_CARRIED_HEAD" ]
}

# root_carried NEW KEEP-DIR: NEW/KEEP-DIR was carried out of the root. With
# root_swap's list, exactly what it names; without one (an older builder's
# leftover), whatever holds state.
root_carried() {
    if root_listed "$1"; then
        grep -qxF "$2" "$1/.lxrt-carried"
    else
        root_holds_state "$1" "$2"
    fi
}

# root_recover ROOT [MARKER]
#   Put back what an interrupted root_swap left in ROOT.new, before anything
#   is deleted. Runs when ROOT.new has .lxrt-carried, or holds state without
#   one (the leftover of scripts/mkarmroot.sh before roots.sh: its swap kept
#   no record). A missing ROOT comes back from ROOT.old, or is made anew (with
#   MARKER in it when one is given). Refuses (status 1, nothing moved) when
#   ROOT.new holds carried state and
#   - ROOT exists without MARKER (what is left of a half-deleted root;
#     ROOT.new is then the only copy of the client's home), or
#   - a carried directory exists in ROOT again.
root_recover() {
    local root="$1" marker="${2:-}" new="$1.new" old="$1.old" d carried="" did=""
    if [ ! -d "$new" ] || [ -L "$new" ]; then return 0; fi
    if [ ! -f "$new/.lxrt-carried" ] && ! root_holds_state "$new"; then return 0; fi
    for d in $ROOT_KEEP; do
        if { [ -e "$new/$d" ] || [ -L "$new/$d" ]; } && root_carried "$new" "$d"; then carried="$carried $d"; fi
    done
    # Stopped between root_swap's two renames: the old root is ROOT.old.
    if [ ! -e "$root" ] && [ ! -L "$root" ] && [ -d "$old" ] && [ ! -L "$old" ]; then
        mv "$old" "$root"
        did=1
    fi
    if [ -n "$carried" ] && [ -n "$marker" ] && [ -e "$root" ] && [ ! -f "$root/$marker" ]; then
        echo "refusing: $new holds the runtime state (client home, apps) of an interrupted rebuild of $root, and $root has no $marker. Keep $new; delete what is left of $root only after moving $new/tmp and $new/opt/apps to a safe place, then run this again" >&2
        return 1
    fi
    for d in $carried; do
        if [ -e "$root/$d" ] || [ -L "$root/$d" ]; then
            echo "refusing: $new/$d was carried out of $root, and $root/$d exists again. Keep the one with the client's data (by hand), remove the other, then run this again" >&2
            return 1
        fi
    done
    if [ ! -e "$root" ]; then
        mkdir "$root"
        if [ -n "$marker" ]; then touch "$root/$marker"; fi
        did=1
    fi
    # Back into ROOT where ROOT lacks it: what was carried, or anything when
    # there is no list (moving the new tree's own copy there is harmless).
    for d in $ROOT_KEEP; do
        if { [ -e "$new/$d" ] || [ -L "$new/$d" ]; } && [ ! -e "$root/$d" ] && [ ! -L "$root/$d" ] &&
           { ! root_listed "$new" || root_carried "$new" "$d" || root_holds_state "$new" "$d"; }; then
            mkdir -p "$(dirname "$root/$d")"
            mv "$new/$d" "$root/$d"
            did=1
        fi
    done
    rm -f "$new/.lxrt-carried"
    if [ -n "$did" ]; then echo "recovered the runtime state of an interrupted rebuild into $root"; fi
}

# root_clear ROOT: remove ROOT.new and ROOT.old. Refuses (status 1) when
# either is a symlink or still holds runtime state (root_holds_state, or a
# swap record root_recover has not dealt with).
root_clear() {
    local x
    for x in "$1.new" "$1.old"; do
        if [ -L "$x" ]; then
            echo "refusing: $x is a symlink; remove it by hand" >&2; return 1
        fi
        [ -e "$x" ] || continue
        if root_holds_state "$x" || [ -f "$x/.lxrt-carried" ]; then
            echo "refusing to delete $x: it holds a guest home (tmp/*home) or installed apps (opt/apps); move them to a safe place first" >&2
            return 1
        fi
        rm -rf "$x"
    done
}

# root_swap ROOT: ROOT.new is complete; carry ROOT_KEEP over from ROOT and
# swap it in. Each state directory is named in ROOT.new/.lxrt-carried before
# it moves (a rename within one directory, so one volume). ROOT becomes
# ROOT.old before ROOT.new becomes ROOT, and ROOT.old is deleted only once
# every carried directory is in the new ROOT and ROOT.old holds no state.
root_swap() {
    local root="$1" new="$1.new" old="$1.old" d moved=""
    if [ -e "$old" ] || [ -L "$old" ]; then
        echo "error: $old appeared during the build; nothing swapped" >&2; return 1
    fi
    if [ ! -e "$root" ]; then mv "$new" "$root"; return 0; fi
    echo "$ROOT_CARRIED_HEAD" > "$new/.lxrt-carried"
    for d in $ROOT_KEEP; do
        [ -e "$root/$d" ] || [ -L "$root/$d" ] || continue
        rm -rf "${new:?}/$d"
        mkdir -p "$(dirname "$new/$d")"
        echo "$d" >> "$new/.lxrt-carried"
        mv "$root/$d" "$new/$d"
        moved="$moved $d"
    done
    mv "$root" "$old"
    mv "$new" "$root"
    for d in $moved; do
        if [ ! -e "$root/$d" ] && [ ! -L "$root/$d" ]; then
            echo "error: $d did not arrive in $root; $old is kept" >&2; return 1
        fi
    done
    rm -f "$root/.lxrt-carried"
    if root_holds_state "$old"; then
        echo "note: $old gained runtime state during the swap (a guest wrote to it); kept, remove it by hand" >&2
    else
        rm -rf "$old"
    fi
}
