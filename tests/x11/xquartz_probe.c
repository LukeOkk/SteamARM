// xquartz_probe (tests/x11/run.sh): what Chromium's X11 output surfaces see
// on DISPLAY, from an aarch64 guest under lxrun:
//  - the refresh rate of RandR's current mode, by Chromium's own formula
//    (ui/base/x/x11_display_util.cc GetRefreshRateFromXRRModeInfo). Every
//    X11 output surface paces viz by it (XrandrIntervalOnlyVSyncProvider):
//    stock XQuartz's rootless mode said 1 Hz, so the Steam window drew once
//    a second (patches/xquartz-randr-real-refresh.patch);
//  - MIT-SHM the way XShmImagePool::Resize uses it (x11_shm_image_pool.cc):
//    shmget(size * 1.5), IPC_RMID before XShmAttach, ShmPutImage with
//    send_event and the wait for ShmCompletion, plus the XPutImage fallback.
// No window is created or mapped: drawing goes to an off-screen pixmap.
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

static int g_err;
static int on_error(Display *d, XErrorEvent *e)
{
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "  X error: code %d (%s) major %d minor %d\n", e->error_code, buf,
            e->request_code, e->minor_code);
    g_err = e->error_code ? e->error_code : -1;
    return 0;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void randr_report(Display *dpy)
{
    int ev, er, maj = 0, min = 0;
    if (!XRRQueryExtension(dpy, &ev, &er) || !XRRQueryVersion(dpy, &maj, &min)) {
        printf("RandR: absent\n");
        return;
    }
    printf("RandR: %d.%d\n", maj, min);
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res)
        return;
    for (int c = 0; c < res->ncrtc; c++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, res->crtcs[c]);
        if (!ci)
            continue;
        for (int m = 0; m < res->nmode; m++) {
            XRRModeInfo *mi = &res->modes[m];
            if (mi->id != ci->mode)
                continue;
            // Same formula as Chromium's GetRefreshRateFromXRRModeInfo.
            double hz = (mi->hTotal && mi->vTotal)
                            ? mi->dotClock / ((double)mi->hTotal * mi->vTotal) : 0;
            printf("RandR crtc %d current mode %ux%u dotClock %lu hTotal %u vTotal %u "
                   "-> refresh %.2f Hz -> Chromium vsync interval %.1f ms\n",
                   c, mi->width, mi->height, mi->dotClock, mi->hTotal, mi->vTotal, hz,
                   hz > 0 ? 1000.0 / hz : 1000.0 / 60);
        }
        XRRFreeCrtcInfo(ci);
    }
    XRRFreeScreenResources(res);
}

// One Chromium-shaped frame buffer: shmget/shmat, IPC_RMID before attach (the
// Linux order in XShmImagePool::Resize), XShmAttach + sync, then ShmPutImage
// with send_event to a pixmap and the wait for ShmCompletion.
static void shm_trial(Display *dpy, int w, int h, int shm_event_base)
{
    size_t need = (size_t)w * h * 4;
    size_t bytes = (size_t)(need * 1.5f);        // kShmResizeThreshold
    printf("\n-- %dx%d: %zu bytes (x1.5 as Chromium asks)\n", w, h, bytes);
    int id = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0606);
    if (id < 0) {
        printf("  shmget: %s\n", strerror(errno));
        return;
    }
    char *addr = shmat(id, NULL, 0);
    if (addr == (char *)-1) {
        printf("  shmat: %s\n", strerror(errno));
        shmctl(id, IPC_RMID, NULL);
        return;
    }
    shmctl(id, IPC_RMID, NULL);
    printf("  shmid %d (0x%x) attached locally at %p\n", id, id, (void *)addr);

    XShmSegmentInfo si = { .shmid = id, .shmaddr = addr, .readOnly = True };
    g_err = 0;
    Bool ok = XShmAttach(dpy, &si);
    XSync(dpy, False);
    printf("  XShmAttach: %s%s\n", ok && !g_err ? "OK" : "FAILED",
           g_err ? " (Chromium: Resize() returns false -> XPutImage path)" : "");
    if (!ok || g_err) {
        shmdt(addr);
        Window r0 = DefaultRootWindow(dpy);
        int d0 = DefaultDepth(dpy, DefaultScreen(dpy));
        Visual *v0 = DefaultVisual(dpy, DefaultScreen(dpy));
        Pixmap p0 = XCreatePixmap(dpy, r0, w, h, d0);
        GC g0 = XCreateGC(dpy, p0, 0, NULL);
        char *hp = calloc(1, need);
        XImage *pi = XCreateImage(dpy, v0, d0, ZPixmap, 0, hp, w, h, 32, 0);
        double t1 = now_ms();
        for (int t = 0; t < 10; t++)
            XPutImage(dpy, p0, g0, pi, 0, 0, 0, 0, w, h);
        XSync(dpy, False);
        printf("  XPutImage fallback: %.2f ms per full frame (mean of 10, synced)\n",
               (now_ms() - t1) / 10);
        XDestroyImage(pi);
        XFreeGC(dpy, g0);
        XFreePixmap(dpy, p0);
        XSync(dpy, False);
        return;
    }

    Window root = DefaultRootWindow(dpy);
    int depth = DefaultDepth(dpy, DefaultScreen(dpy));
    Visual *vis = DefaultVisual(dpy, DefaultScreen(dpy));
    Pixmap pm = XCreatePixmap(dpy, root, w, h, depth);
    GC gc = XCreateGC(dpy, pm, 0, NULL);
    XImage *img = XShmCreateImage(dpy, vis, depth, ZPixmap, addr, &si, w, h);
    double worst = 0, sum = 0;
    int got = 0, trials = 20;
    for (int t = 0; t < trials; t++) {
        memset(addr, t * 9, need);
        double t0 = now_ms();
        XShmPutImage(dpy, pm, gc, img, 0, 0, 0, 0, w, h, True);
        XFlush(dpy);
        int arrived = 0;
        while (!arrived && now_ms() - t0 < 3000) {
            while (XPending(dpy)) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == shm_event_base + ShmCompletion)
                    arrived = 1;
            }
            if (arrived)
                break;
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ConnectionNumber(dpy), &fds);
            struct timeval tv = { 0, 50000 };
            select(ConnectionNumber(dpy) + 1, &fds, NULL, NULL, &tv);
        }
        double dt = now_ms() - t0;
        if (arrived) {
            got++;
            sum += dt;
            if (dt > worst)
                worst = dt;
        }
    }
    printf("  ShmCompletion: %d/%d arrived, mean %.2f ms, worst %.2f ms\n", got, trials,
           got ? sum / got : 0, worst);
    XShmDetach(dpy, &si);
    img->data = NULL;
    XDestroyImage(img);

    // The non-SHM fallback (DrawPixmap -> XPutImage) for the same frame.
    char *heap = calloc(1, need);
    XImage *pimg = XCreateImage(dpy, vis, depth, ZPixmap, 0, heap, w, h, 32, 0);
    double t0 = now_ms();
    for (int t = 0; t < 10; t++)
        XPutImage(dpy, pm, gc, pimg, 0, 0, 0, 0, w, h);
    XSync(dpy, False);
    printf("  XPutImage fallback: %.2f ms per full frame (mean of 10, synced)\n",
           (now_ms() - t0) / 10);
    XDestroyImage(pimg);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, pm);
    XSync(dpy, False);
    shmdt(addr);
}

int main(int argc, char **argv)
{
    alarm(40);
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        printf("XOpenDisplay failed\n");
        return 1;
    }
    XSetErrorHandler(on_error);
    printf("display %s vendor \"%s\" release %d\n", DisplayString(dpy), ServerVendor(dpy),
           VendorRelease(dpy));

    struct shminfo info;
    memset(&info, 0, sizeof info);
    int r = shmctl(0, IPC_INFO, (struct shmid_ds *)&info);
    printf("shmctl(0, IPC_INFO): %d%s%s shmmax %lu (Chromium MaxShmSegmentSize: %lu)\n", r,
           r < 0 ? " errno " : "", r < 0 ? strerror(errno) : "", r < 0 ? 0 : info.shmmax,
           r < 0 ? 0 : info.shmmax);

    int maj = 0, min = 0;
    Bool pix = False;
    int op, shm_ev, shm_er;
    if (!XQueryExtension(dpy, "MIT-SHM", &op, &shm_ev, &shm_er) ||
        !XShmQueryVersion(dpy, &maj, &min, &pix)) {
        printf("MIT-SHM: absent\n");
    } else {
        printf("MIT-SHM: %d.%d shared pixmaps %d (ShmAttachFd needs 1.2: %s)\n", maj, min,
               pix, (maj > 1 || (maj == 1 && min >= 2)) ? "yes" : "no");
    }
    randr_report(dpy);

    if (maj) {
        shm_trial(dpy, 700, 440, shm_ev);     // Steam's sign-in window
        shm_trial(dpy, 1280, 800, shm_ev);    // Steam's main window, as logged
        shm_trial(dpy, 1920, 1050, shm_ev);   // the whole rootless screen
    }
    XCloseDisplay(dpy);
    return 0;
}
