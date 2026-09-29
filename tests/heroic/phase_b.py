#!/usr/bin/env python3
"""Heroic Games Launcher (linux-arm64, Electron) under lxrun: the acceptance run.

    tests/heroic/phase_b.py [--cycles N] [--shots DIR] [--program GUEST-PATH]

Each cycle: start Heroic natively (scripts/run-native.sh, no FEX, no VM) ->
its main window mapped and viewable on the X display -> a click on the
sidebar's Settings link through the page's DevTools protocol (the page is
Heroic's own UI; nothing is typed, nobody logs in) and the settings route
shown -> SIGTERM to Heroic's main process only, as `kill` would (or, with
--close quit, a click on Heroic's own Quit) -> every process of the session
gone -> next cycle.

Only processes this script started are ever signalled: its child and that
child's descendants, and orphans carrying this run's STEAMARM_RUN_ID.
Exit status: 0 when every cycle passed every step.

Needs: build/lxrun, the Fedora armroot (/tmp/lxrt-armroot) with Heroic in
opt/apps/heroic (scripts/install-heroic-arm64.sh), the X server on :2
(scripts/run-x11-native.sh start), xwininfo.
"""
import argparse, base64, json, os, re, signal, subprocess, sys, time, urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "scripts"))
DEFAULT_PROGRAM = "/opt/apps/heroic/Heroic-2.22.3-linux-arm64/heroic"
TITLE = "Heroic Games Launcher"


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def ps_rows():
    out = subprocess.run(["/bin/ps", "-axo", "pid=,ppid=,rss=,command="],
                         capture_output=True, text=True).stdout
    rows = {}
    for line in out.splitlines():
        a = line.split(None, 3)
        if len(a) >= 3:
            rows[int(a[0])] = (int(a[1]), int(a[2]), a[3] if len(a) > 3 else "")
    return rows


def run_id_of(pids):
    """{pid: STEAMARM_RUN_ID} through scripts/roots.sh's procs_env (KERN_PROCARGS2)."""
    if not pids:
        return {}
    out = subprocess.run(["/bin/bash", "-c", '. scripts/roots.sh; procs_env STEAMARM_RUN_ID "$@"', "x"]
                         + [str(p) for p in pids], cwd=REPO, capture_output=True, text=True).stdout
    res = {}
    for line in out.splitlines():
        a = line.split(None, 1)
        if len(a) == 2:
            res[int(a[0])] = a[1].strip()
    return res


class Session:
    def __init__(self, args, run_id, logpath):
        self.args, self.run_id, self.known = args, run_id, set()
        env = dict(os.environ)
        env.update({
            "LXRT_ROOT": args.root,
            "HOME_IN_GUEST": args.home,
            # Electron's binaries use x18 outside their few .eh_frame FDEs
            # (docs/HEROIC_INTEGRATION.md): every image of the app is
            # rewritten over its whole text.
            "LXRT_X18_ALL_TEXT": "/opt/apps/heroic/",
            "STEAMARM_RUN_ID": run_id,
            "DISPLAY": args.display,
        })
        # The launcher entry's command line (launcher/ApplicationCore.swift,
        # HeroicARM64.command), plus the DevTools port this script drives.
        cmd = [os.path.join(REPO, "scripts/run-native.sh"), args.program, "--no-sandbox"]
        cmd += [] if args.gpu else ["--disable-gpu"]
        cmd += ["--js-flags=" + args.js_flags, "--remote-debugging-port=%d" % args.cdp_port] + args.extra
        self.t0 = time.time()
        self.logf = open(logpath, "w")
        self.p = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=self.logf,
                                  stderr=subprocess.STDOUT, start_new_session=True)
        self.known.add(self.p.pid)

    def track(self):
        """Live processes of this session (descendants, then run-id orphans)."""
        rows = ps_rows()
        changed = True
        while changed:
            changed = False
            for pid, (ppid, _, _) in rows.items():
                if pid not in self.known and ppid in self.known:
                    self.known.add(pid)
                    changed = True
        orphans = [pid for pid, (ppid, _, cmd) in rows.items()
                   if pid not in self.known and ppid == 1 and "lxrun" in cmd]
        for pid, rid in run_id_of(orphans).items():
            if rid == self.run_id:
                self.known.add(pid)
        alive = {pid: rows[pid] for pid in self.known
                 if pid in rows and "<defunct>" not in rows[pid][2]}
        return alive

    def rss_mb(self):
        return sum(r[1] for r in self.track().values()) // 1024

    def elapsed(self):
        return time.time() - self.t0


def find_window(display):
    tree = subprocess.run(["xwininfo", "-root", "-tree", "-display", display],
                          capture_output=True, text=True).stdout
    for line in tree.splitlines():
        m = re.match(r'\s*(0x[0-9a-f]+) "%s": .*?(\d+)x(\d+)\+-?\d+\+-?\d+\s+\+(-?\d+)\+(-?\d+)' % re.escape(TITLE), line)
        if m:
            wid = m.group(1)
            info = subprocess.run(["xwininfo", "-id", wid, "-display", display],
                                  capture_output=True, text=True).stdout
            if "IsViewable" in info:
                return wid, int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
    return None


def cdp_page(port, timeout):
    """The DevTools target of Heroic's own page (file://.../index.html)."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/json" % port, timeout=3) as r:
                for t in json.load(r):
                    if t.get("type") == "page" and "index.html" in t.get("url", ""):
                        return t
        except OSError:
            pass
        time.sleep(0.5)
    return None


def evaluate(ws, expr):
    r = ws.call("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True)
    return r.get("result", {}).get("result", {}).get("value")


def click(ws, x, y):
    for kind in ("mouseMoved", "mousePressed", "mouseReleased"):
        ws.call("Input.dispatchMouseEvent", type=kind, x=x, y=y,
                button="left" if kind != "mouseMoved" else "none", clickCount=1)


# The first start shows modal dialogs over the library: the release notes of
# this version (closed with its X) and the anonymous analytics question
# (answered Disable). Each is dismissed with a click, like a user would.
DIALOG_JS = """(() => {
    const vis = e => e && e.offsetParent !== null && e.getBoundingClientRect().width > 0;
    const dlg = [...document.querySelectorAll('dialog, .Dialog, [role=dialog]')].filter(vis).pop();
    if (!dlg) return null;
    const btns = [...dlg.querySelectorAll('button')].filter(vis);
    const no = btns.find(b => /^(no|disable|decline|don.?t)/i.test(b.innerText.trim()));
    const close = btns.find(b => /close/i.test((b.className || '') + ' ' + (b.getAttribute('aria-label') || '') + ' ' + (b.title || '')))
        || btns.find(b => !b.innerText.trim());
    const b = no || close || btns[0];
    if (!b) return {title: dlg.innerText.slice(0, 60), button: null};
    const r = b.getBoundingClientRect();
    return {title: (dlg.querySelector('h1,h2,h3,h4,.Dialog__headerTitle') || dlg).innerText.trim().slice(0, 60),
            button: b.innerText.trim() || b.className || 'close', x: r.x + r.width / 2, y: r.y + r.height / 2};
})()"""


def dismiss_dialogs(ws):
    seen = []
    for _ in range(6):
        d = evaluate(ws, DIALOG_JS)
        if not d:
            return seen
        if not d.get("button"):
            log("dialog %r has no button" % d.get("title"))
            return seen
        log("dialog %r: click %r" % (d["title"], d["button"]))
        seen.append(d["title"])
        click(ws, d["x"], d["y"])
        time.sleep(1.5)
    return seen


def interact(args, shots, cycle):
    """Click the sidebar's Settings link; True when the settings route shows."""
    os.environ["CDP_PORT"] = str(args.cdp_port)
    import importlib, cdp
    importlib.reload(cdp)
    t = cdp_page(args.cdp_port, 60)
    if not t:
        log("FAIL no DevTools page target on port %d" % args.cdp_port)
        return False
    log("DevTools page: %s" % t.get("url", "")[:120])
    ws = cdp.WS(t["webSocketDebuggerUrl"])
    link = None
    end = time.time() + 60
    while time.time() < end and not link:
        link = evaluate(ws, """(() => {
            const a = [...document.querySelectorAll('a[href*="settings"]')]
                .find(e => e.offsetParent !== null);
            if (!a) return null;
            const r = a.getBoundingClientRect();
            return {x: r.x + r.width / 2, y: r.y + r.height / 2,
                    text: a.innerText.trim(), href: a.getAttribute('href'),
                    hash: location.hash};
        })()""")
        if not link:
            time.sleep(0.5)
    if not link:
        log("FAIL no visible Settings link in the page")
        return False
    log("before: route %r; Settings link %r at (%d, %d)" % (link["hash"], link["text"] or link["href"],
                                                              link["x"], link["y"]))
    if shots:
        png = ws.call("Page.captureScreenshot", format="png")["result"]["data"]
        path = os.path.join(shots, "heroic-cycle%d-library.png" % cycle)
        open(path, "wb").write(base64.b64decode(png))
        log("screenshot %s (%d bytes)" % (path, os.path.getsize(path)))
    dismiss_dialogs(ws)
    click(ws, link["x"], link["y"])
    after = None
    end = time.time() + 20
    while time.time() < end:
        after = evaluate(ws, """(() => ({route: location.hash || location.pathname,
            heading: [...document.querySelectorAll('.headerTitle, h1, h2, h3, h4')]
                     .filter(e => e.offsetParent !== null).map(e => e.innerText.trim()).filter(Boolean).slice(0, 3),
            tabs: [...document.querySelectorAll('a[href*="/settings/"]')]
                  .filter(e => e.offsetParent !== null).map(e => e.innerText.trim()).filter(Boolean).slice(0, 8)}))()""")
        if after and "settings" in (after.get("route") or ""):
            break
        time.sleep(0.5)
    ok = bool(after and "settings" in (after.get("route") or ""))
    log("%s after click: route %r, headings %s, settings tabs %s" % (
        "ok" if ok else "FAIL", after and after.get("route"), after and after.get("heading"),
        after and after.get("tabs")))
    if shots and ok:
        time.sleep(1)
        png = ws.call("Page.captureScreenshot", format="png")["result"]["data"]
        path = os.path.join(shots, "heroic-cycle%d-settings.png" % cycle)
        open(path, "wb").write(base64.b64decode(png))
        log("screenshot %s (%d bytes)" % (path, os.path.getsize(path)))
    if ok and args.close == "quit":
        # Heroic's own "Quit" in its sidebar, as a user ends it.
        q = evaluate(ws, """(() => {
            const e = [...document.querySelectorAll('button, a, [role=button], .Sidebar__item')]
                .find(e => e.offsetParent !== null && e.innerText.trim() === 'Quit');
            if (!e) return null;
            e.scrollIntoView({block: 'center'});
            const r = e.getBoundingClientRect();
            return {x: r.x + r.width / 2, y: r.y + r.height / 2};
        })()""")
        if not q:
            log("FAIL no Quit item in the sidebar")
            ok = False
        else:
            log("click Heroic's sidebar Quit at (%d, %d)" % (q["x"], q["y"]))
            try:
                click(ws, q["x"], q["y"])
                time.sleep(1.5)
                d = evaluate(ws, DIALOG_JS.replace("/^(no|disable|decline|don.?t)/i", "/^(yes|quit|exit|ok)/i"))
                if d and d.get("button"):
                    log("dialog %r: click %r" % (d["title"], d["button"]))
                    click(ws, d["x"], d["y"])
            except (OSError, EOFError, ValueError):
                pass            # the page went away: Heroic is quitting
    try:
        ws.s.close()
    except OSError:
        pass
    return ok


def stop(sess, timeout, how="sigterm"):
    """SIGTERM to Heroic's main process only (or, after its own Quit, no
    signal at all); True when every process exits."""
    alive = sess.track()
    t = time.time()
    if how == "quit":
        log("close: Heroic's Quit clicked; waiting (%d processes, %d MB)" % (len(alive), sess.rss_mb()))
        tree = subprocess.run(["xwininfo", "-root", "-tree", "-display", sess.args.display],
                              capture_output=True, text=True).stdout
        for line in tree.splitlines():
            if '"heroic"' in line or "Heroic" in line:
                log("   X: " + line.strip()[:140])
    else:
        log("close: SIGTERM to main pid %d (%d processes, %d MB)" % (sess.p.pid, len(alive), sess.rss_mb()))
        try:
            os.kill(sess.p.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    while time.time() - t < timeout:
        sess.p.poll()
        if not sess.track():
            break
        time.sleep(0.25)
    left = sess.track()
    code = sess.p.poll()
    if not left:
        log("ok all processes gone %.1f s after the %s (main exit status %s)" % (
            time.time() - t, "Quit" if how == "quit" else "SIGTERM", code))
        return True
    log("FAIL %d processes left %.0f s after the %s:" % (len(left), timeout, "Quit" if how == "quit" else "SIGTERM"))
    for pid, (ppid, rss, cmd) in sorted(left.items()):
        log("   %d ppid %d %d MB %s" % (pid, ppid, rss // 1024, cmd[:140]))
        if os.environ.get("PHASE_B_LSOF"):
            out = subprocess.run(["lsof", "-a", "-p", str(pid), "-d", "0-200", "-U"],
                                 capture_output=True, text=True).stdout
            for line in out.splitlines()[1:]:
                log("      " + line[:160])
    for pid in left:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    time.sleep(5)
    for pid in sess.track():
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=2)
    ap.add_argument("--program", default=DEFAULT_PROGRAM)
    ap.add_argument("--root", default="/tmp/lxrt-armroot")
    ap.add_argument("--home", default="/tmp/heroichome")
    ap.add_argument("--display", default=":2")
    ap.add_argument("--cdp-port", type=int, default=9333)
    # The launcher entry's V8 flags (launcher/ApplicationCore.swift, HeroicARM64).
    ap.add_argument("--js-flags", default="--no-opt")
    # sigterm: SIGTERM to Heroic's main process; quit: Heroic's own sidebar Quit.
    ap.add_argument("--close", choices=("sigterm", "quit"), default="sigterm")
    ap.add_argument("--extra", action="append", default=[], help="another Heroic argument (repeatable)")
    ap.add_argument("--gpu", action="store_true", help="without --disable-gpu (a control)")
    ap.add_argument("--window-timeout", type=float, default=90)
    ap.add_argument("--stop-timeout", type=float, default=30)
    ap.add_argument("--shots", default="")
    ap.add_argument("--logdir", default=os.path.expanduser("~/SteamARM-roots/logs"))
    args = ap.parse_args()
    os.makedirs(args.logdir, exist_ok=True)
    if args.shots:
        os.makedirs(args.shots, exist_ok=True)
    if find_window(args.display):
        log("FAIL a %r window is already on %s: another session is running" % (TITLE, args.display))
        return 2
    passed = 0
    for cycle in range(1, args.cycles + 1):
        run_id = "heroic-phase-b.%d.%d.%d" % (os.getpid(), cycle, int(time.time()))
        logpath = os.path.join(args.logdir, "heroic-arm64-%s-c%d.log" % (time.strftime("%Y%m%d-%H%M%S"), cycle))
        log("cycle %d: start %s (log %s)" % (cycle, args.program, logpath))
        sess = Session(args, run_id, logpath)
        win = None
        while sess.elapsed() < args.window_timeout:
            if sess.p.poll() is not None and not sess.track():
                break
            win = find_window(args.display)
            if win:
                break
            time.sleep(0.25)
        ok = bool(win)
        if win:
            log("ok window %s %dx%d at +%d+%d mapped and viewable %.1f s after start; %d processes, %d MB"
                % (win[0], win[1], win[2], win[3], win[4], sess.elapsed(), len(sess.track()), sess.rss_mb()))
            ok = interact(args, args.shots, cycle) and ok
            log("memory after interaction: %d processes, %d MB" % (len(sess.track()), sess.rss_mb()))
        else:
            log("FAIL no %r window within %.0f s (main exit status %s)" % (TITLE, args.window_timeout, sess.p.poll()))
        ok = stop(sess, args.stop_timeout, args.close if win else "sigterm") and ok
        if find_window(args.display):
            log("FAIL the window is still mapped after the close")
            ok = False
        sess.logf.close()
        log("cycle %d: %s" % (cycle, "PASS" if ok else "FAIL"))
        passed += ok
    log("== %d of %d cycles passed" % (passed, args.cycles))
    return 0 if passed == args.cycles else 1


if __name__ == "__main__":
    sys.exit(main())
