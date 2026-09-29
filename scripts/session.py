#!/usr/bin/env python3
"""One launcher session as one process group, with an exit status.

scripts/run-app.sh starts every app through `session.py run`, which puts the
program and everything it forks into a process group of its own, records the
group and, when the program ends, its exit status. "Detener" then signals
that group instead of every lxrun process on the machine, and the launcher
learns how the program ended instead of guessing from how long it lived
(docs/APPLICATION_MANAGER.md). Only the standard library; runs on macOS and
Linux (macOS has no setsid command, hence this file).

    session.py run <launcher-dir> <command> [args...]
        New session and process group (falls back to a new process group when
        already a group leader). Writes <launcher-dir>/running.pgid, runs the
        command, waits for it, writes <launcher-dir>/running.status (the exit
        code, or 128 + the signal), then gives the rest of the group GRACE
        seconds to leave before SIGTERM and, KILL_AFTER seconds later, SIGKILL.
        Exits with the program's status.
    session.py stop <launcher-dir> [grace-seconds]
        SIGTERM to the recorded group; members still alive after the grace
        period get SIGKILL. Waits for running.status to be written. Does
        nothing when no group is recorded or its leader is not a `session.py run`.
    session.py detach <command> [args...]
        setsid, then exec: for services (FEXServer, safeguard.sh) that must
        not belong to the app's group.

The wrapper handles SIGTERM/SIGINT/SIGHUP with a no-op handler, not SIG_IGN:
an ignored disposition would be inherited by the program through exec.
"""
import os
import signal
import subprocess
import sys
import time

GRACE = 5.0        # seconds the group gets to finish on its own after the program
KILL_AFTER = 2.0   # seconds between SIGTERM and SIGKILL for what is left


def log(msg):
    sys.stderr.write("[session] %s\n" % msg)
    sys.stderr.flush()


def own_group():
    """A process group of our own: a new session when possible. setsid fails
    only for a process that already leads a group (a job of an interactive
    shell, or a session leader), and that group is then already ours."""
    try:
        os.setsid()
    except PermissionError:
        try:
            os.setpgid(0, 0)
        except PermissionError:  # a session leader: its group is its own
            pass
    return os.getpgrp()


def members(pgid):
    """Live PIDs in process group pgid, without its leader (the wrapper), the
    ps that lists them, and zombies (nothing to signal; whether they are ever
    reaped is up to their parent). ps -A -o works on macOS and Linux."""
    p = subprocess.Popen(["ps", "-Ao", "pid=,pgid=,stat="], stdout=subprocess.PIPE, text=True)
    out = p.communicate()[0]
    pids = []
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 3 and f[1] == str(pgid) and not f[2].startswith("Z"):
            pid = int(f[0])
            if pid != pgid and pid != p.pid:
                pids.append(pid)
    return pids


def command_of(pid):
    p = subprocess.run(["ps", "-o", "command=", "-p", str(pid)], stdout=subprocess.PIPE, text=True)
    return p.stdout.strip()


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def wait_empty(pgid, seconds):
    """True when no member other than the leader remains within `seconds`."""
    deadline = time.monotonic() + seconds
    while True:
        left = members(pgid)
        if not left:
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.1)


def kill_members(pgid, sig):
    for pid in members(pgid):
        try:
            os.kill(pid, sig)
        except (ProcessLookupError, PermissionError):
            pass


def write_atomic(path, text):
    tmp = path + ".new"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def status_of(returncode):
    """Shell convention: a signal death is 128 + the signal number."""
    return 128 - returncode if returncode < 0 else returncode


def cmd_run(ldir, argv):
    if not argv:
        log("run: no command")
        return 2
    pgid = own_group()
    os.makedirs(ldir, exist_ok=True)
    write_atomic(os.path.join(ldir, "running.pgid"), "%d\n" % pgid)
    # A handler, not SIG_IGN: the program must still be killable by the same
    # signals, and an ignored signal would survive its exec.
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, lambda *_: None)
    log("process group %d: %s" % (pgid, " ".join(argv)))
    try:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL)
    except OSError as e:
        log("cannot start %s: %s" % (argv[0], e))
        write_atomic(os.path.join(ldir, "running.status"), "127\n")
        return 127
    status = status_of(proc.wait())
    write_atomic(os.path.join(ldir, "running.status"), "%d\n" % status)
    log("%s ended with status %d" % (os.path.basename(argv[0]), status))
    # The program is gone; helpers it left behind get a moment, then the
    # signals. What is left after that has no owner and would block the next
    # launch ("Another Linux program is running").
    if not wait_empty(pgid, GRACE):
        log("group still has %d process(es): SIGTERM" % len(members(pgid)))
        kill_members(pgid, signal.SIGTERM)
        if not wait_empty(pgid, KILL_AFTER):
            log("SIGKILL to %d process(es)" % len(members(pgid)))
            kill_members(pgid, signal.SIGKILL)
            wait_empty(pgid, 1.0)
    return status


def cmd_stop(ldir, grace):
    path = os.path.join(ldir, "running.pgid")
    try:
        pgid = int(open(path).read().strip())
    except (OSError, ValueError):
        return 0
    if not alive(pgid) or "session.py run" not in command_of(pgid):
        return 0   # stale file: the group is gone, or its number was reused
    try:
        os.killpg(pgid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        return 0
    if not wait_empty(pgid, grace):
        kill_members(pgid, signal.SIGKILL)
        wait_empty(pgid, 1.0)
    # The wrapper writes running.status once its program is gone.
    deadline = time.monotonic() + KILL_AFTER
    while alive(pgid) and time.monotonic() < deadline:
        time.sleep(0.1)
    if alive(pgid):
        try:
            os.kill(pgid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
    return 0


def cmd_detach(argv):
    if not argv:
        log("detach: no command")
        return 2
    own_group()
    os.execvp(argv[0], argv)


def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "run":
        return cmd_run(a[1], a[2:])
    if len(a) >= 2 and a[0] == "stop":
        return cmd_stop(a[1], float(a[2]) if len(a) > 2 else 3.0)
    if len(a) >= 2 and a[0] == "detach":
        return cmd_detach(a[1:])
    sys.stderr.write(__doc__.split("\n\n", 1)[1])
    return 2


if __name__ == "__main__":
    sys.exit(main())
