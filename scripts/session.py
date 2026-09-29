#!/usr/bin/env python3
"""One launcher session as one process group, with an exit status.

scripts/run-app.sh starts every app through `session.py run`, which puts the
program and what it forks into a process group of its own, records the group
and, when the program ends, how it ended. "Detener" then signals that group
instead of every lxrun process on the machine, and the launcher learns how the
program ended instead of guessing from how long it lived
(docs/APPLICATION_MANAGER.md). A process that starts a session of its own
(setsid: wineserver, daemons) leaves the group; run-app.sh --stop still kills
leftover guests after the group. Only the standard library; runs on macOS and
Linux (macOS has no setsid command, hence this file).

    session.py run <launcher-dir> <command> [args...]
        A new session and process group. When this process already leads a
        group (a job of an interactive shell), a child takes the new session
        and this process relays its exit status. Writes
        <launcher-dir>/running.pgid, runs the command, waits for it, writes
        <launcher-dir>/running.status: "N" for exit code N, or "N signal S"
        for a death by signal S (N = 128 + S). Then gives the rest of the
        group GRACE seconds to leave before SIGTERM and, KILL_AFTER seconds
        later, SIGKILL. Exits with N.
    session.py stop <launcher-dir> [grace-seconds]
        SIGTERM to the recorded group; members still alive after the grace
        period get SIGKILL. Waits for running.status to be written. Does
        nothing when no group is recorded or its leader is not a `session.py run`.
    session.py detach <command> [args...]
        A new session, then exec: for services (FEXServer, safeguard.sh) that
        must not belong to the app's group.

Callers start this file as `env PYTHONCOERCECLOCALE=0 STEAMARM_SESSION_PY=1
python3 ...`: without the first, Python sets LC_CTYPE (C.UTF-8, or UTF-8 on
macOS) in its environment when no locale is set, and the program would inherit
it. The second says the first was added by the caller; both are removed before
the program starts.

The wrapper handles SIGTERM/SIGINT/SIGHUP with a no-op handler, not SIG_IGN:
an ignored disposition would be inherited by the program through exec. What
Python itself ignores (SIGPIPE, SIGXFSZ) is set back to the default for the
program, and a SIGHUP ignored by nohup stays ignored.
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


def clean_env():
    """The caller's environment, without what starting Python needed."""
    if os.environ.pop("STEAMARM_SESSION_PY", None) is not None:
        os.environ.pop("PYTHONCOERCECLOCALE", None)


def exit_code(wait_status):
    code = os.waitstatus_to_exitcode(wait_status)
    return 128 - code if code < 0 else code


def own_group():
    """A new session, so a process group of our own. setsid is refused to a
    process that already leads a group (a job of an interactive shell), and
    setpgid(0, 0) changes nothing there: the rest of that job (a pipe's reader,
    say) would share the group and be signalled with it. A child never leads a
    group, so it takes the new session; this process relays its exit status."""
    try:
        os.setsid()
        return os.getpgrp()
    except PermissionError:
        pass
    child = os.fork()
    if child == 0:
        os.setsid()
        return os.getpgrp()
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, lambda sig, _frame: os.killpg(child, sig))
    _, st = os.waitpid(child, 0)
    os._exit(exit_code(st))


def members(pgid):
    """Live PIDs in process group pgid, without its leader (the wrapper), the
    ps that lists them, and zombies (nothing to signal; whether they are ever
    reaped is up to their parent). ps -A -o works on macOS and Linux. None
    when ps cannot be read: that is not an empty group."""
    for _ in range(3):
        p = subprocess.Popen(["ps", "-Ao", "pid=,pgid=,stat="],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        out = p.communicate()[0]
        if p.returncode == 0 and out.strip():
            break
        time.sleep(0.1)
    else:
        return None
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
        if members(pgid) == []:
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.1)


def kill_members(pgid, sig, leader_too=False):
    """Signal every member but the leader. When the group cannot be listed,
    signal the whole group instead -- unless that is a SIGKILL that would take
    the leader (the wrapper) down before it records the status."""
    pids = members(pgid)
    if pids is None:
        if sig == signal.SIGKILL and not leader_too:
            log("cannot list the group; SIGKILL not sent")
            return
        try:
            os.killpg(pgid, sig)
        except (ProcessLookupError, PermissionError):
            pass
        return
    for pid in pids:
        try:
            os.kill(pid, sig)
        except (ProcessLookupError, PermissionError):
            pass


def write_atomic(path, text):
    tmp = path + ".new"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def status_line(returncode):
    """'N', or 'N signal S' for a death by signal S (N = 128 + S, as a shell has it)."""
    if returncode < 0:
        return "%d signal %d" % (128 - returncode, -returncode)
    return "%d" % returncode


def cmd_run(ldir, argv):
    if not argv:
        log("run: no command")
        return 2
    hup_ignored = signal.getsignal(signal.SIGHUP) == signal.SIG_IGN   # nohup
    pgid = own_group()
    os.makedirs(ldir, exist_ok=True)
    write_atomic(os.path.join(ldir, "running.pgid"), "%d\n" % pgid)
    # A handler, not SIG_IGN: the program must still be killable by the same
    # signals, and an ignored signal would survive its exec.
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, lambda *_: None)
    log("process group %d: %s" % (pgid, " ".join(argv)))
    try:
        # restore_signals (the default) gives the program SIGPIPE and SIGXFSZ
        # back; nohup's ignored SIGHUP is put back by hand.
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL,
                                preexec_fn=(lambda: signal.signal(signal.SIGHUP, signal.SIG_IGN))
                                if hup_ignored else None)
    except OSError as e:
        log("cannot start %s: %s" % (argv[0], e))
        write_atomic(os.path.join(ldir, "running.status"), "127\n")
        return 127
    rc = proc.wait()
    line = status_line(rc)
    write_atomic(os.path.join(ldir, "running.status"), line + "\n")
    log("%s ended with status %s" % (os.path.basename(argv[0]), line))
    # The program is gone; helpers it left behind get a moment, then the
    # signals. What is left after that has no owner and would block the next
    # launch ("Another Linux program is running").
    if not wait_empty(pgid, GRACE):
        log("group still has process(es) left: SIGTERM")
        kill_members(pgid, signal.SIGTERM)
        if not wait_empty(pgid, KILL_AFTER):
            log("SIGKILL to what is left")
            kill_members(pgid, signal.SIGKILL)
            wait_empty(pgid, 1.0)
    return int(line.split()[0])


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
        kill_members(pgid, signal.SIGKILL, leader_too=True)
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
    # Python ignores these at start-up; an exec would pass that on.
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    signal.signal(signal.SIGXFSZ, signal.SIG_DFL)
    os.execvp(argv[0], argv)


def main():
    clean_env()
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
