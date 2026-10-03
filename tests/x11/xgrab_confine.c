/* What SDL 3 does for mouse-look on X11 (SDL_x11window.c X11_SetWindowMouseGrab,
   SDL_x11mouse.c X11_ShowCursor): grab the pointer with confine_to = its own
   window and set an empty pixmap cursor. Runs on the host against SteamARM's
   X server (tests/x11/run-confine.sh drives it):
     xgrab_confine SECONDS [visible]
   maps a 640x400 window at 200,200, grabs (confine_to = the window) with an
   empty cursor, or with the default cursor if "visible" is given, holds the
   grab for SECONDS and releases it. Prints "grabbed" and "released". */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 3;
    int visible = argc > 2 && strcmp(argv[2], "visible") == 0;
    Display *d = XOpenDisplay(NULL);
    if (!d) { printf("no display\n"); return 1; }
    Window root = DefaultRootWindow(d);
    XSetWindowAttributes a = { .background_pixel = 0x204060, .event_mask = StructureNotifyMask | ExposureMask };
    Window w = XCreateWindow(d, root, 200, 200, 640, 400, 0, CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWEventMask, &a);
    XStoreName(d, w, "xgrab_confine");
    XMapRaised(d, w);
    for (;;) { XEvent e; XNextEvent(d, &e); if (e.type == MapNotify) break; }
    XSetInputFocus(d, w, RevertToParent, CurrentTime);
    if (!visible) {
        static char zero[1] = { 0 };
        Pixmap p = XCreateBitmapFromData(d, w, zero, 1, 1);
        XColor black = { 0 };
        Cursor empty = XCreatePixmapCursor(d, p, p, &black, &black, 0, 0);
        XDefineCursor(d, w, empty);
    }
    int r = GrabNotViewable;
    for (int i = 0; i < 40 && r != GrabSuccess; i++) {
        r = XGrabPointer(d, w, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, w, None, CurrentTime);
        if (r != GrabSuccess) usleep(50000);
    }
    if (r != GrabSuccess) { printf("grab failed: %d\n", r); return 1; }
    XSync(d, False);
    printf("grabbed\n"); fflush(stdout);
    usleep((useconds_t)(secs * 1e6));
    XUngrabPointer(d, CurrentTime);
    XUndefineCursor(d, w);
    XSync(d, False);
    printf("released\n");
    XDestroyWindow(d, w);
    XCloseDisplay(d);
    return 0;
}
