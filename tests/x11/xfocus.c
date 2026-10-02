/* Give an X window the input focus without touching the Mac's pointer or
 * its frontmost application: XSetInputFocus on the first window whose name
 * contains the argument, mapped first if it is not (a fullscreen SDL window
 * that never had the focus minimizes itself). A game that sleeps while
 * unfocused (Counter-Strike 2, iconic, waits 100 ms per frame in
 * SDL_WaitEventTimeout: 9.5 fps, MEASURED, benchmarks/stage51) then runs
 * unthrottled for a frame-rate measurement, as it does when the user clicks
 * it.
 *
 *   cc -o xfocus tests/x11/xfocus.c $(pkg-config --cflags --libs x11)
 *   DISPLAY=:2 ./xfocus "Counter-Strike"     # prints the window and the focus owner
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static Window find(Display *d, Window w, const char *part, int depth)
{
    XTextProperty name = {0};
    XWindowAttributes a;
    /* Not XFetchName: SDL sets WM_NAME as UTF8_STRING, which it will not read. */
    if (XGetWMName(d, w, &name) && name.value) {
        int hit = strstr((const char *)name.value, part) != NULL;
        XFree(name.value);
        if (hit && XGetWindowAttributes(d, w, &a) && a.class == InputOutput && a.width > 1)
            return w;
    }
    Window root, parent, *kids = NULL, found = None;
    unsigned n = 0;
    if (depth < 6 && XQueryTree(d, w, &root, &parent, &kids, &n)) {
        for (unsigned i = 0; i < n && !found; i++)
            found = find(d, kids[i], part, depth + 1);
        if (kids)
            XFree(kids);
    }
    return found;
}

int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    if (!d || argc < 2) {
        fprintf(stderr, "usage: xfocus NAME-PART (with DISPLAY set)\n");
        return 2;
    }
    Window w = find(d, DefaultRootWindow(d), argv[1], 0);
    if (!w) {
        printf("no window named *%s*\n", argv[1]);
        return 1;
    }
    XWindowAttributes a;
    XGetWindowAttributes(d, w, &a);
    if (a.map_state != IsViewable) {
        XMapRaised(d, w);
        for (int i = 0; i < 100 && a.map_state != IsViewable; i++) {
            XSync(d, False);
            usleep(50000);
            XGetWindowAttributes(d, w, &a);
        }
        printf("window 0x%lx was not mapped: %s\n", w, a.map_state == IsViewable ? "mapped now" : "still not viewable");
        if (a.map_state != IsViewable)
            return 1;
    }
    XSetInputFocus(d, w, RevertToParent, CurrentTime);
    XSync(d, False);
    Window owner = None;
    int revert = 0;
    XGetInputFocus(d, &owner, &revert);
    printf("window 0x%lx focus owner 0x%lx%s\n", w, owner, owner == w ? " (focused)" : "");
    XCloseDisplay(d);
    return owner == w ? 0 : 1;
}
