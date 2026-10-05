// SteamARM pointer tests (tests/x11/run-confine.sh). xiwine SECONDS: XI2 raw
// motion on :2's root read the two ways games read it, following the master
// pointer's axes through slave switches (XI_DeviceChanged):
//   wine: axes 0 and 1 whatever their labels (Wine 11 winex11.drv mouse.c
//         update_relative_valuators / map_raw_event_coords); relative axes
//         are summed, absolute ones only give a position;
//   sdl:  Rel X / Rel Y by label first, then Abs X / Abs Y, then the first two
//         axes (SDL 3 SDL_x11xinput2.c xinput2_update_device_info).
// Prints both sums and the modes the last motion was read with.
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct axes { int num[2]; int rel[2]; };

static Atom rel_x, rel_y, abs_x, abs_y;

static void pick(XIAnyClassInfo **classes, int n, struct axes *wine, struct axes *sdl)
{
    int have_rel_x = 0, have_rel_y = 0, have_abs_x = 0, have_abs_y = 0, idx = 0;
    wine->num[0] = wine->num[1] = sdl->num[0] = sdl->num[1] = -1;
    for (int i = 0; i < n; i++) {
        if (classes[i]->type != XIValuatorClass) continue;
        XIValuatorClassInfo *v = (XIValuatorClassInfo *)classes[i];
        int rel = v->mode == XIModeRelative;
        if (v->number == 0 || v->number == 1) { wine->num[v->number] = v->number; wine->rel[v->number] = rel; }
        if (v->label == rel_x || (v->label == abs_x && !have_rel_x) || (idx == 0 && !have_rel_x && !have_abs_x)) {
            sdl->num[0] = v->number; sdl->rel[0] = rel;
            if (v->label == rel_x) have_rel_x = 1; else if (v->label == abs_x) have_abs_x = 1;
        } else if (v->label == rel_y || (v->label == abs_y && !have_rel_y) || (idx == 1 && !have_rel_y && !have_abs_y)) {
            sdl->num[1] = v->number; sdl->rel[1] = rel;
            if (v->label == rel_y) have_rel_y = 1; else if (v->label == abs_y) have_abs_y = 1;
        }
        idx++;
    }
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 3;
    Display *d = XOpenDisplay(":2"); if (!d) return 1;
    int op, ev, er; if (!XQueryExtension(d, "XInputExtension", &op, &ev, &er)) return 2;
    int maj = 2, min = 2; XIQueryVersion(d, &maj, &min);
    rel_x = XInternAtom(d, "Rel X", False); rel_y = XInternAtom(d, "Rel Y", False);
    abs_x = XInternAtom(d, "Abs X", False); abs_y = XInternAtom(d, "Abs Y", False);

    int master = -1, ndev;
    XIDeviceInfo *info = XIQueryDevice(d, XIAllMasterDevices, &ndev);
    struct axes wine = {{-1, -1}, {0, 0}}, sdl = {{-1, -1}, {0, 0}};
    for (int i = 0; i < ndev; i++)
        if (info[i].use == XIMasterPointer) { master = info[i].deviceid; pick(info[i].classes, info[i].num_classes, &wine, &sdl); break; }
    XIFreeDeviceInfo(info);

    XIEventMask m; unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(bits, XI_RawMotion); XISetMask(bits, XI_DeviceChanged);
    m.deviceid = XIAllMasterDevices; m.mask_len = sizeof bits; m.mask = bits;
    XISelectEvents(d, DefaultRootWindow(d), &m, 1); XFlush(d);

    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    long n = 0, switches = 0; double wx = 0, wy = 0, sx = 0, sy = 0;
    int last_wine_rel = -1, last_sdl_rel = -1;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 > secs) break;
        while (XPending(d)) {
            XEvent e; XNextEvent(d, &e);
            if (e.xcookie.type != GenericEvent || e.xcookie.extension != op || !XGetEventData(d, &e.xcookie)) continue;
            if (e.xcookie.evtype == XI_DeviceChanged) {
                XIDeviceChangedEvent *c = e.xcookie.data;
                if (c->deviceid == master && c->reason == XISlaveSwitch) { pick(c->classes, c->num_classes, &wine, &sdl); switches++; }
            } else if (e.xcookie.evtype == XI_RawMotion) {
                XIRawEvent *r = e.xcookie.data; int vi = 0;
                for (int i = 0; i < r->valuators.mask_len * 8; i++) if (XIMaskIsSet(r->valuators.mask, i)) {
                    double v = r->raw_values[vi++];
                    if (i == wine.num[0] && wine.rel[0]) wx += v;
                    if (i == wine.num[1] && wine.rel[1]) wy += v;
                    if (i == sdl.num[0] && sdl.rel[0]) sx += v;
                    if (i == sdl.num[1] && sdl.rel[1]) sy += v;
                }
                last_wine_rel = wine.rel[0] && wine.rel[1];
                last_sdl_rel = sdl.rel[0] && sdl.rel[1];
                n++;
            }
            XFreeEventData(d, &e.xcookie);
        }
        struct timespec nap = {0, 2000000}; nanosleep(&nap, 0);
    }
    printf("raw motion events %ld, slave switches %ld; wine %s X sum %.1f Y sum %.1f; sdl %s X sum %.1f Y sum %.1f\n",
           n, switches, last_wine_rel == 1 ? "relative" : last_wine_rel == 0 ? "absolute" : "none", wx, wy,
           last_sdl_rel == 1 ? "relative" : last_sdl_rel == 0 ? "absolute" : "none", sx, sy);
    return 0;
}
