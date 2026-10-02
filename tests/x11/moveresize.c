/* What quartz-wm does with _NET_WM_MOVERESIZE, the request every frameless
 * X client sends when the user drags its own title bar or edge: SDL3
 * hit-test windows (X11_TriggerHitTestAction) and Chromium/CEF custom frames
 * (ui::DoWMMoveResize), and with presses on the edges of frameless windows.
 * Checks patches/quartz-wm-netwm-moveresize.patch.
 *
 * It creates an undecorated window (_MOTIF_WM_HINTS decorations = 0, as SDL
 * and Chromium set them) holding a child and a grandchild the way Steam's
 * SDL3 toplevel holds CefWindowX11 and Chromium's X11Window. For each case
 * it presses button 1 through XTEST and either hands the implicit grab to
 * the WM with _NET_WM_MOVERESIZE (as SDL3 sends it from the toplevel, or as
 * Chromium sends it from the grandchild: button 0, source 0) or sends
 * nothing (edge presses, Cmd-Option-drag), drags the pointer and releases,
 * and reads the result back with XGetGeometry / XTranslateCoordinates. One
 * "ok" / "FAIL" / "skip" line per check; exit 0 when nothing failed.
 *
 * It MAPS A WINDOW, MOVES THE MAC POINTER AND PRESSES Cmd and Option: run it
 * only against the SteamARM X server (DISPLAY=:2) when nobody is using the
 * Mac.
 *
 *   cc -o moveresize tests/x11/moveresize.c $(pkg-config --cflags --libs x11 xtst)
 *   DISPLAY=:2 ./moveresize    # -d: title-bar double click, -m: minimize
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOVERESIZE_SIZE_TOP          1
#define MOVERESIZE_SIZE_BOTTOMRIGHT  4
#define MOVERESIZE_SIZE_LEFT         7
#define MOVERESIZE_MOVE              8
#define MOVERESIZE_CANCEL           11

/* quartz-wm defaults: wm_frameless_resize_border, top edge, corner */
#define EDGE   5
#define TOP    3
#define CORNER 16

typedef struct { int x, y, w, h; } rect;

static Display *dpy;
static Window root, win, child, grandchild;
static int failures;

/* ButtonPress events the test windows got since the last reset */
static int presses;
static XButtonEvent last_press;

static int on_x_error(Display *d, XErrorEvent *e)
{
    char text[128];

    XGetErrorText(d, e->error_code, text, sizeof text);
    printf("note X error: %s (request %d)\n", text, e->request_code);
    return 0;
}

static Atom atom(const char *name)
{
    return XInternAtom(dpy, name, False);
}

/* Let the server and the WM catch up, counting the presses that reached
 * the client and dropping the rest of our own events. */
static void settle(int ms)
{
    XEvent ev;

    XSync(dpy, False);
    usleep((useconds_t)ms * 1000);
    while (XPending(dpy)) {
        XNextEvent(dpy, &ev);
        if (ev.type == ButtonPress) {
            presses++;
            last_press = ev.xbutton;
        }
    }
}

static rect geometry(void)
{
    rect r;
    Window c, rt;
    unsigned int w, h, bw, depth;
    int gx, gy;

    XSync(dpy, False);
    XGetGeometry(dpy, win, &rt, &gx, &gy, &w, &h, &bw, &depth);
    XTranslateCoordinates(dpy, win, root, 0, 0, &r.x, &r.y, &c);
    r.w = (int)w;
    r.h = (int)h;
    return r;
}

static void check(const char *what, int got, int want, int tolerance)
{
    if (abs(got - want) <= tolerance) {
        printf("ok   %s: %d\n", what, got);
    } else {
        printf("FAIL %s: %d, want %d\n", what, got, want);
        failures++;
    }
}

static void check_rect(const char *what, rect got, rect want)
{
    char label[96];

    snprintf(label, sizeof label, "%s x", what);
    check(label, got.x, want.x, 2);
    snprintf(label, sizeof label, "%s y", what);
    check(label, got.y, want.y, 2);
    snprintf(label, sizeof label, "%s width", what);
    check(label, got.w, want.w, 2);
    snprintf(label, sizeof label, "%s height", what);
    check(label, got.h, want.h, 2);
}

static int atom_list_has(Window w, const char *prop, const char *name)
{
    Atom type, want = atom(name), *list;
    int format, found = 0;
    unsigned long n, after, i;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, w, atom(prop), 0, 4096, False, XA_ATOM,
                           &type, &format, &n, &after, &data) != Success)
        return 0;
    if (data != NULL && format == 32) {
        list = (Atom *)data;
        for (i = 0; i < n; i++)
            if (list[i] == want)
                found = 1;
    }
    if (data != NULL)
        XFree(data);
    return found;
}

static int cardinals(Window w, const char *prop, long *out, int max)
{
    Atom type;
    int format, i = 0;
    unsigned long n, after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, w, atom(prop), 0, max, False, XA_CARDINAL,
                           &type, &format, &n, &after, &data) != Success)
        return 0;
    if (data != NULL && format == 32)
        for (i = 0; i < (int)n && i < max; i++)
            out[i] = ((long *)data)[i];
    if (data != NULL)
        XFree(data);
    return i;
}

static void client_message_from(Window from, const char *type, long l0,
                                long l1, long l2, long l3, long l4)
{
    XEvent ev;

    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = from;
    ev.xclient.message_type = atom(type);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = l0;
    ev.xclient.data.l[1] = l1;
    ev.xclient.data.l[2] = l2;
    ev.xclient.data.l[3] = l3;
    ev.xclient.data.l[4] = l4;
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XSync(dpy, False);
}

static void client_message(const char *type, long l0, long l1, long l2,
                           long l3, long l4)
{
    client_message_from(win, type, l0, l1, l2, l3, l4);
}

/* What SDL3 DispatchWindowMove / InitiateWindowResize send (from the
 * toplevel, button 1, source 1) or what Chromium's ui::DoWMMoveResize sends
 * (from its own window, button 0, source 0). */
static void moveresize_from(Window from, int x_root, int y_root,
                            int direction, long button, long source)
{
    XUngrabPointer(dpy, CurrentTime);
    client_message_from(from, "_NET_WM_MOVERESIZE", x_root, y_root,
                        direction, button, source);
}

static void moveresize(int x_root, int y_root, int direction)
{
    moveresize_from(win, x_root, y_root, direction, Button1, 1);
}

static void pointer_to(int x, int y)
{
    XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime);
    settle(15);
}

/* Press at (x0, y0), ask the WM to take over from |from|, drag by
 * (dx, dy), release. */
static void drag_from(Window from, long button, long source, int x0, int y0,
                      int dx, int dy, int direction, int cancel_midway)
{
    int i, steps = 12;

    pointer_to(x0, y0);
    settle(60);
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    settle(60);
    moveresize_from(from, x0, y0, direction, button, source);
    settle(120);
    for (i = 1; i <= steps; i++) {
        if (cancel_midway && i == 1)
            client_message("_NET_WM_MOVERESIZE", x0, y0, MOVERESIZE_CANCEL, Button1, 1);
        pointer_to(x0 + dx * i / steps, y0 + dy * i / steps);
    }
    settle(60);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    settle(400);
}

static void drag(int x0, int y0, int dx, int dy, int direction, int cancel_midway)
{
    drag_from(win, Button1, 1, x0, y0, dx, dy, direction, cancel_midway);
}

/* Press at (x0, y0) with |keys| held (0-terminated keycodes), drag by
 * (dx, dy), release; no client message. Leaves the number of presses the
 * client saw in |presses|. */
static void drag_plain(int x0, int y0, int dx, int dy, const KeyCode *keys)
{
    int i, steps = 12;
    const KeyCode *k;

    pointer_to(x0, y0);
    settle(60);
    for (k = keys; k != NULL && *k != 0; k++)
        XTestFakeKeyEvent(dpy, *k, True, CurrentTime);
    settle(30);
    presses = 0;
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    settle(120);
    for (i = 1; i <= steps; i++)
        pointer_to(x0 + dx * i / steps, y0 + dy * i / steps);
    settle(60);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    settle(60);
    for (k = keys; k != NULL && *k != 0; k++)
        XTestFakeKeyEvent(dpy, *k, False, CurrentTime);
    settle(400);
}

static void click_title(int x, int y)
{
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    settle(30);
    moveresize(x, y, MOVERESIZE_MOVE);
    settle(40);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    settle(60);
}

static int wait_mapped(void)
{
    XEvent ev;
    int ms;

    for (ms = 0; ms < 3000; ms += 10) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            if (ev.type == MapNotify && ev.xmap.window == win)
                return 1;
        }
        usleep(10000);
    }
    return 0;
}

/* The nesting of Steam's window: toplevel -> CefWindowX11 -> X11Window,
 * each filling its parent; Chromium's selects the pointer events. */
static void make_children(int w, int h)
{
    XSetWindowAttributes attr;

    memset(&attr, 0, sizeof attr);
    attr.background_pixel = WhitePixel(dpy, DefaultScreen(dpy));
    child = XCreateWindow(dpy, win, 0, 0, (unsigned int)w, (unsigned int)h,
                          0, CopyFromParent, InputOutput, CopyFromParent,
                          CWBackPixel, &attr);
    attr.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    grandchild = XCreateWindow(dpy, child, 0, 0, (unsigned int)w,
                               (unsigned int)h, 0, CopyFromParent,
                               InputOutput, CopyFromParent,
                               CWBackPixel | CWEventMask, &attr);
    XMapWindow(dpy, grandchild);
    XMapWindow(dpy, child);
}

int main(int argc, char **argv)
{
    int event_base, error_base, major, minor;
    int test_double_click = 0, test_minimize = 0;
    int px, py, wx, wy, i, x, y;
    unsigned int mask;
    Window rr, cr;
    rect r0, r, want;
    long extents[4];
    KeyCode cmd_option[3];
    XSetWindowAttributes attr;
    XSizeHints hints;
    long motif[5] = { 2, 0, 0, 0, 0 }; /* flags: decorations; decorations: none */

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0)
            test_double_click = 1;
        else if (strcmp(argv[i], "-m") == 0)
            test_minimize = 1;
    }

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        printf("FAIL cannot open display %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    XSetErrorHandler(on_x_error);
    root = DefaultRootWindow(dpy);

    if (atom_list_has(root, "_NET_SUPPORTED", "_NET_WM_MOVERESIZE")) {
        printf("ok   _NET_SUPPORTED has _NET_WM_MOVERESIZE\n");
    } else {
        printf("FAIL _NET_SUPPORTED lacks _NET_WM_MOVERESIZE (unpatched quartz-wm?)\n");
        failures++;
    }
    check("_NET_SUPPORTED has _NET_WM_STATE_FOCUSED",
          atom_list_has(root, "_NET_SUPPORTED", "_NET_WM_STATE_FOCUSED"), 1, 0);

    if (!XTestQueryExtension(dpy, &event_base, &error_base, &major, &minor)) {
        printf("skip no XTEST on this server: drags not tested\n");
        XCloseDisplay(dpy);
        return failures ? 1 : 0;
    }

    XQueryPointer(dpy, root, &rr, &cr, &px, &py, &wx, &wy, &mask);

    memset(&attr, 0, sizeof attr);
    attr.background_pixel = WhitePixel(dpy, DefaultScreen(dpy));
    attr.event_mask = StructureNotifyMask | ButtonPressMask | ButtonReleaseMask
                      | PointerMotionMask | PropertyChangeMask;
    win = XCreateWindow(dpy, root, 300, 300, 480, 320, 0, CopyFromParent,
                        InputOutput, CopyFromParent, CWBackPixel | CWEventMask,
                        &attr);
    XStoreName(dpy, win, "moveresize-test");
    memset(&hints, 0, sizeof hints);
    hints.flags = USPosition | USSize;
    hints.x = 300;
    hints.y = 300;
    hints.width = 480;
    hints.height = 320;
    XSetWMNormalHints(dpy, win, &hints);
    XChangeProperty(dpy, win, atom("_MOTIF_WM_HINTS"), atom("_MOTIF_WM_HINTS"),
                    32, PropModeReplace, (unsigned char *)motif, 5);
    make_children(480, 320);
    XMapWindow(dpy, win);
    if (!wait_mapped()) {
        printf("FAIL window never mapped\n");
        return 1;
    }
    settle(600);
    client_message("_NET_ACTIVE_WINDOW", 1, CurrentTime, 0, 0, 0);
    settle(200);

    r0 = geometry();
    printf("note start %d,%d %dx%d\n", r0.x, r0.y, r0.w, r0.h);

    if (cardinals(win, "_NET_FRAME_EXTENTS", extents, 4) == 4) {
        check("_NET_FRAME_EXTENTS top (undecorated)", (int)extents[2], 0, 0);
    } else {
        printf("FAIL no _NET_FRAME_EXTENTS on the window\n");
        failures++;
    }
    check("_NET_WM_STATE_FOCUSED on the active window",
          atom_list_has(win, "_NET_WM_STATE", "_NET_WM_STATE_FOCUSED"), 1, 0);

    /* Move by the top strip, where frameless clients draw their title bar. */
    drag(r0.x + r0.w / 2, r0.y + 10, 100, 40, MOVERESIZE_MOVE, 0);
    r = geometry();
    want = r0; want.x += 100; want.y += 40;
    check_rect("move +100,+40", r, want);

    /* Bottom-right corner: size grows, origin stays. */
    r0 = r;
    drag(r0.x + r0.w - 3, r0.y + r0.h - 3, 80, 60, MOVERESIZE_SIZE_BOTTOMRIGHT, 0);
    r = geometry();
    want = r0; want.w += 80; want.h += 60;
    check_rect("resize bottom-right +80,+60", r, want);

    /* Left edge: right edge stays put. */
    r0 = r;
    drag(r0.x + 2, r0.y + r0.h / 2, -50, 0, MOVERESIZE_SIZE_LEFT, 0);
    r = geometry();
    want = r0; want.x -= 50; want.w += 50;
    check_rect("resize left -50", r, want);

    /* Top edge: bottom edge stays put. */
    r0 = r;
    drag(r0.x + r0.w / 2, r0.y + 2, 0, -30, MOVERESIZE_SIZE_TOP, 0);
    r = geometry();
    want = r0; want.y -= 30; want.h += 30;
    check_rect("resize top -30", r, want);

    /* _NET_WM_MOVERESIZE_CANCEL right after the request: nothing moves. */
    r0 = r;
    drag(r0.x + r0.w / 2, r0.y + 10, 60, 60, MOVERESIZE_MOVE, 1);
    check_rect("cancelled move", geometry(), r0);

    /* Request without a pressed button (released before the WM saw it). */
    pointer_to(r0.x + r0.w / 2, r0.y + 10);
    moveresize(r0.x + r0.w / 2, r0.y + 10, MOVERESIZE_MOVE);
    settle(100);
    pointer_to(r0.x + r0.w / 2 + 70, r0.y + 80);
    settle(300);
    check_rect("request with button up", geometry(), r0);

    /* From the grandchild (Steam: Chromium's X11Window two levels below the
     * managed SDL3 toplevel), SDL-style fields. */
    r0 = geometry();
    drag_from(grandchild, Button1, 1, r0.x + r0.w / 2, r0.y + 10, -60, 30,
              MOVERESIZE_MOVE, 0);
    r = geometry();
    want = r0; want.x -= 60; want.y += 30;
    check_rect("move requested from grandchild", r, want);

    /* What Chromium really sends: its own window, button 0, source 0. */
    r0 = r;
    drag_from(grandchild, 0, 0, r0.x + r0.w / 2, r0.y + 10, 70, -20,
              MOVERESIZE_MOVE, 0);
    r = geometry();
    want = r0; want.x += 70; want.y -= 20;
    check_rect("Chromium-style move (button 0, source 0)", r, want);

    r0 = r;
    drag_from(grandchild, 0, 0, r0.x + r0.w - 3, r0.y + r0.h - 3, 40, 30,
              MOVERESIZE_SIZE_BOTTOMRIGHT, 0);
    r = geometry();
    want = r0; want.w += 40; want.h += 30;
    check_rect("Chromium-style resize bottom-right", r, want);

    /* Presses on the edges of a frameless window resize it without any
     * client message, and never reach the client. */
    r0 = r;
    drag_plain(r0.x + r0.w - EDGE + 1, r0.y + r0.h / 2, 60, 0, NULL);
    r = geometry();
    want = r0; want.w += 60;
    check_rect("edge press: right +60", r, want);
    check("edge press: presses the client saw", presses, 0, 0);

    r0 = r;
    drag_plain(r0.x + r0.w / 2, r0.y + r0.h - 2, 0, 40, NULL);
    r = geometry();
    want = r0; want.h += 40;
    check_rect("edge press: bottom +40", r, want);
    check("edge press: presses the client saw", presses, 0, 0);

    r0 = r;
    drag_plain(r0.x + CORNER - 4, r0.y + r0.h - 2, -30, 20, NULL); /* past EDGE */
    r = geometry();
    want = r0; want.x -= 30; want.w += 30; want.h += 20;
    check_rect("edge press: bottom-left corner -30,+20", r, want);

    r0 = r;
    drag_plain(r0.x + r0.w / 2, r0.y + 1, 0, -20, NULL);
    r = geometry();
    want = r0; want.y -= 20; want.h += 20;
    check_rect("edge press: top (1 px in) -20", r, want);
    check("edge press: presses the client saw", presses, 0, 0);

    /* Just below the thin top edge is the client's (Steam's top bar menus):
     * the press goes through and nothing moves. */
    r0 = r;
    drag_plain(r0.x + r0.w / 2, r0.y + TOP + 1, 0, -20, NULL);
    check_rect("press below the top edge", geometry(), r0);
    check("press below the top edge reached the client", presses, 1, 0);

    /* Anywhere else: replayed to the client unchanged. */
    x = r0.x + r0.w / 2;
    y = r0.y + r0.h / 2;
    drag_plain(x, y, 25, 25, NULL);
    check_rect("press in the middle", geometry(), r0);
    check("press in the middle reached the client", presses, 1, 0);
    check("  ... button", (int)last_press.button, Button1, 0);
    check("  ... x_root", last_press.x_root, x, 0);
    check("  ... y_root", last_press.y_root, y, 0);

    /* Cmd-Option-drag moves from anywhere. */
    cmd_option[0] = XKeysymToKeycode(dpy, XK_Meta_L);
    cmd_option[1] = XKeysymToKeycode(dpy, XK_Alt_L);
    if (cmd_option[1] == 0)
        cmd_option[1] = XKeysymToKeycode(dpy, XK_Mode_switch);
    cmd_option[2] = 0;
    if (cmd_option[0] != 0 && cmd_option[1] != 0) {
        drag_plain(x, y, -80, 50, cmd_option);
        r = geometry();
        want = r0; want.x -= 80; want.y += 50;
        check_rect("Cmd-Option-drag", r, want);
        check("Cmd-Option-drag: presses the client saw", presses, 0, 0);
        r0 = r;
    } else {
        printf("skip Cmd-Option-drag: no Meta_L / Alt_L / Mode_switch keycode\n");
    }

    /* Keep-above. */
    client_message("_NET_WM_STATE", 1, (long)atom("_NET_WM_STATE_ABOVE"), 0, 1, 0);
    settle(200);
    check("_NET_WM_STATE_ABOVE added", atom_list_has(win, "_NET_WM_STATE", "_NET_WM_STATE_ABOVE"), 1, 0);
    client_message("_NET_WM_STATE", 0, (long)atom("_NET_WM_STATE_ABOVE"), 0, 1, 0);
    settle(200);
    check("_NET_WM_STATE_ABOVE removed", atom_list_has(win, "_NET_WM_STATE", "_NET_WM_STATE_ABOVE"), 0, 0);

    /* Maximize the way SDL3 / Chromium ask for it, then restore. */
    client_message("_NET_WM_STATE", 1, (long)atom("_NET_WM_STATE_MAXIMIZED_VERT"),
                   (long)atom("_NET_WM_STATE_MAXIMIZED_HORZ"), 1, 0);
    settle(400);
    check("maximized (_NET_WM_STATE_MAXIMIZED_HORZ)",
          atom_list_has(win, "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_HORZ"), 1, 0);
    client_message("_NET_WM_STATE", 0, (long)atom("_NET_WM_STATE_MAXIMIZED_VERT"),
                   (long)atom("_NET_WM_STATE_MAXIMIZED_HORZ"), 1, 0);
    settle(400);
    check_rect("restored after maximize", geometry(), r0);

    /* Chromium's caption buttons ask for its own window. */
    client_message_from(grandchild, "_NET_WM_STATE", 1,
                        (long)atom("_NET_WM_STATE_MAXIMIZED_VERT"),
                        (long)atom("_NET_WM_STATE_MAXIMIZED_HORZ"), 1, 0);
    settle(400);
    check("maximized from grandchild",
          atom_list_has(win, "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_HORZ"), 1, 0);
    client_message_from(grandchild, "_NET_WM_STATE", 0,
                        (long)atom("_NET_WM_STATE_MAXIMIZED_VERT"),
                        (long)atom("_NET_WM_STATE_MAXIMIZED_HORZ"), 1, 0);
    settle(400);
    check_rect("restored from grandchild", geometry(), r0);

    /* Double click on the title strip runs System Settings' title-bar action
     * (AppleActionOnDoubleClick); with Zoom/Fill it maximizes and back. */
    if (test_double_click) {
        x = r0.x + r0.w / 2;
        y = r0.y + 10;
        pointer_to(x, y);
        settle(600); /* stay clear of the previous click */
        click_title(x, y);
        click_title(x, y);
        settle(400);
        check("double click maximized",
              atom_list_has(win, "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_HORZ"), 1, 0);
        r = geometry();
        settle(600);
        click_title(r.x + r.w / 2, r.y + 10);
        click_title(r.x + r.w / 2, r.y + 10);
        settle(400);
        check_rect("double click restored", geometry(), r0);
    } else {
        printf("skip title-bar double click (run with -d)\n");
    }

    /* _NET_ACTIVE_WINDOW from the application (source 1, Steam's
     * SDL_RaiseWindow) leaves a minimized window in the Dock; from a pager
     * (source 2) it brings it back. */
    if (test_minimize) {
        client_message("WM_CHANGE_STATE", IconicState, 0, 0, 0, 0);
        settle(1500);
        check("minimized", atom_list_has(win, "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN"), 1, 0);
        client_message("_NET_ACTIVE_WINDOW", 1, CurrentTime, 0, 0, 0);
        settle(1500);
        check("still minimized after source 1",
              atom_list_has(win, "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN"), 1, 0);
        client_message("_NET_ACTIVE_WINDOW", 2, CurrentTime, 0, 0, 0);
        settle(1500);
        check("un-minimized by source 2",
              atom_list_has(win, "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN"), 0, 0);
    } else {
        printf("skip _NET_ACTIVE_WINDOW and minimized windows (run with -m)\n");
    }

    XDestroyWindow(dpy, win);
    pointer_to(px, py);
    XSync(dpy, False);
    XCloseDisplay(dpy);

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
    return failures ? 1 : 0;
}
