// SteamARM pointer tests (tests/x11/run-confine.sh). xiraw SECONDS: XI2 raw motion on :2's root; sums of the Rel X/Rel Y raw valuators (2,3), count, and abs X/Y range.
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 3;
    Display *d = XOpenDisplay(":2"); if (!d) return 1;
    int op, ev, er; if (!XQueryExtension(d, "XInputExtension", &op, &ev, &er)) return 2;
    int maj = 2, min = 2; XIQueryVersion(d, &maj, &min);
    XIEventMask m; unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0}; XISetMask(bits, XI_RawMotion);
    m.deviceid = XIAllMasterDevices; m.mask_len = sizeof bits; m.mask = bits;
    XISelectEvents(d, DefaultRootWindow(d), &m, 1); XFlush(d);
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    long n = 0; double sx = 0, sy = 0, ax0 = -1, ax1 = -1, ay0 = -1, ay1 = -1;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 > secs) break;
        while (XPending(d)) {
            XEvent e; XNextEvent(d, &e);
            if (e.xcookie.type == GenericEvent && e.xcookie.extension == op && XGetEventData(d, &e.xcookie)) {
                XIRawEvent *r = e.xcookie.data; int vi = 0;
                for (int i = 0; i < r->valuators.mask_len * 8; i++) if (XIMaskIsSet(r->valuators.mask, i)) {
                    double v = r->raw_values[vi++];
                    if (i == 2) sx += v; if (i == 3) sy += v;
                    if (i == 0) { if (ax0 < 0) ax0 = v; ax1 = v; } if (i == 1) { if (ay0 < 0) ay0 = v; ay1 = v; }
                }
                n++; XFreeEventData(d, &e.xcookie);
            }
        }
        struct timespec nap = {0, 2000000}; nanosleep(&nap, 0);
    }
    printf("raw motion events %ld; rel X sum %.1f, rel Y sum %.1f; abs X %.0f->%.0f, abs Y %.0f->%.0f\n", n, sx, sy, ax0, ax1, ay0, ay1);
    return 0;
}
