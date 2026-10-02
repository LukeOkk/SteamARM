/* Passive EWMH monitor for the SteamARM X server: which window-manager
 * requests does a client really send? Written to settle who (if anyone)
 * asks quartz-wm to move Steam's frameless window: SDL3 hit test
 * (_NET_WM_MOVERESIZE from the SDL toplevel, data.l[3] = Button1) or
 * Chromium/CEF (ui::DoWMMoveResize from its own X11Window, which in Steam's
 * "System" composer is a grandchild of the SDL toplevel, data.l[3] = 0).
 *
 * It creates NO window and grabs nothing: it only selects
 * SubstructureNotify on the root, which delivers every client message sent
 * to the root with that mask (EWMH requests) and the top-level
 * ConfigureNotify events (-c). Safe to run while the user works.
 *
 *   cc -o /tmp/wmspy tests/x11/wmspy.c $(pkg-config --cflags --libs x11)
 *   DISPLAY=:2 /tmp/wmspy [-c]      # Ctrl-C to stop
 *
 * For a client message it also prints the window's ancestry up to the root
 * (id, WM_CLASS) so a request from a child window is obvious. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static double now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static int ignore_errors(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    return 0;
}

static void print_ancestry(Display *d, Window w, Window root)
{
    int depth;

    for (depth = 0; w != None && w != root && depth < 8; depth++) {
        Window r, parent = None, *kids = NULL;
        unsigned int n = 0;
        XClassHint ch = { NULL, NULL };

        if (XGetClassHint(d, w, &ch)) {
            printf("    0x%lx WM_CLASS %s/%s\n", w, ch.res_name ? ch.res_name : "",
                   ch.res_class ? ch.res_class : "");
            if (ch.res_name)
                XFree(ch.res_name);
            if (ch.res_class)
                XFree(ch.res_class);
        } else {
            printf("    0x%lx\n", w);
        }
        if (!XQueryTree(d, w, &r, &parent, &kids, &n))
            break;
        if (kids)
            XFree(kids);
        w = parent;
    }
}

int main(int argc, char **argv)
{
    int show_configure = argc > 1 && strcmp(argv[1], "-c") == 0;
    Display *d = XOpenDisplay(NULL);
    Window root;

    if (d == NULL) {
        fprintf(stderr, "wmspy: cannot open display\n");
        return 1;
    }
    XSetErrorHandler(ignore_errors);
    root = DefaultRootWindow(d);
    XSelectInput(d, root, SubstructureNotifyMask);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("%.3f watching root 0x%lx\n", now(), root);

    for (;;) {
        XEvent e;

        XNextEvent(d, &e);
        if (e.type == ClientMessage) {
            char *name = XGetAtomName(d, e.xclient.message_type);

            printf("%.3f ClientMessage %s window 0x%lx format %d data %ld %ld %ld %ld %ld\n",
                   now(), name ? name : "?", e.xclient.window, e.xclient.format,
                   e.xclient.data.l[0], e.xclient.data.l[1], e.xclient.data.l[2],
                   e.xclient.data.l[3], e.xclient.data.l[4]);
            if (name)
                XFree(name);
            print_ancestry(d, e.xclient.window, root);
        } else if (e.type == ConfigureNotify && show_configure) {
            printf("%.3f ConfigureNotify 0x%lx %d,%d %dx%d%s\n", now(),
                   e.xconfigure.window, e.xconfigure.x, e.xconfigure.y,
                   e.xconfigure.width, e.xconfigure.height,
                   e.xconfigure.send_event ? " (synthetic)" : "");
        }
    }
}
