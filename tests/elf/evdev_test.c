// /dev/input/eventN through the runtime (evdev.c) against tests/elf/
// fake_inputd.py: what SDL and winebus do with a controller node.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int fails;
static void check(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}
#define TEST_BIT(b, a) ((a[(b) / 8] >> ((b) % 8)) & 1)

int main(void)
{
    DIR *d = opendir("/dev/input");
    int seen = 0;
    struct dirent *de;
    while (d && (de = readdir(d)))
        seen |= !strcmp(de->d_name, "event0");
    if (d) closedir(d);
    check(seen, "/dev/input lists event0");

    struct stat st;
    check(stat("/dev/input/event0", &st) == 0 && S_ISCHR(st.st_mode) &&
          major(st.st_rdev) == 13 && minor(st.st_rdev) == 64, "stat: char device 13:64");

    int fd = open("/dev/input/event0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    check(fd >= 0, "open O_RDWR|O_NONBLOCK");
    if (fd < 0) { printf("errno %d\n", errno); return 1; }

    char name[256] = "";
    check(ioctl(fd, EVIOCGNAME(sizeof name), name) > 0 && !strcmp(name, "Microsoft X-Box 360 pad"),
          "EVIOCGNAME");
    struct input_id id;
    check(ioctl(fd, EVIOCGID, &id) == 0 && id.vendor == 0x045e && id.product == 0x028e &&
          id.bustype == 3 && id.version == 0x114, "EVIOCGID 045e:028e");
    unsigned char evbits[4] = {0}, keys[KEY_MAX / 8 + 1] = {0}, absb[ABS_MAX / 8 + 1] = {0}, ff[FF_MAX / 8 + 1] = {0};
    check(ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) >= 0 && TEST_BIT(EV_KEY, evbits) &&
          TEST_BIT(EV_ABS, evbits) && TEST_BIT(EV_FF, evbits), "EVIOCGBIT(0): KEY ABS FF");
    check(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 && TEST_BIT(BTN_A, keys) &&
          TEST_BIT(BTN_MODE, keys) && !TEST_BIT(BTN_TL2, keys), "EVIOCGBIT(EV_KEY)");
    check(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb) >= 0 && TEST_BIT(ABS_RZ, absb) &&
          TEST_BIT(ABS_HAT0Y, absb), "EVIOCGBIT(EV_ABS)");
    check(ioctl(fd, EVIOCGBIT(EV_FF, sizeof ff), ff) >= 0 && TEST_BIT(FF_RUMBLE, ff), "EVIOCGBIT(EV_FF)");
    struct input_absinfo ai;
    check(ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0 && ai.minimum == -32768 && ai.maximum == 32767 &&
          ai.flat == 128 && ai.value == 1000, "EVIOCGABS(ABS_X) = snapshot 1000");
    check(ioctl(fd, EVIOCGABS(ABS_Z), &ai) == 0 && ai.maximum == 255 && ai.value == 7, "EVIOCGABS(ABS_Z) = 7");
    int n;
    check(ioctl(fd, EVIOCGEFFECTS, &n) == 0 && n == 16, "EVIOCGEFFECTS 16");
    check(ioctl(fd, EVIOCGRAB, 1) == 0 && ioctl(fd, EVIOCGRAB, 0) == 0, "EVIOCGRAB");
    check(fstat(fd, &st) == 0 && S_ISCHR(st.st_mode), "fstat: char device");

    // Events: A down, stick move, A up, each frame ending in SYN_REPORT.
    struct input_event evs[32];
    int got_a_down = 0, got_a_up = 0, got_x = 0, syn = 0, total = 0;
    for (int tries = 0; tries < 50 && !got_a_up; tries++) {
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, 100);
        ssize_t r = read(fd, evs, sizeof evs);
        if (r < 0) { if (errno == EAGAIN) continue; break; }
        if (r % sizeof evs[0]) { printf("partial read %zd\n", r); break; }
        for (int i = 0; i < r / (int)sizeof evs[0]; i++, total++) {
            struct input_event *e = &evs[i];
            if (e->type == EV_KEY && e->code == BTN_A) { if (e->value) got_a_down = 1; else got_a_up = 1; }
            if (e->type == EV_ABS && e->code == ABS_X && e->value == -20000) got_x = 1;
            if (e->type == EV_SYN && e->code == SYN_REPORT) syn++;
        }
    }
    check(got_a_down && got_a_up && got_x && syn >= 3, "read: A press/release, stick, SYN_REPORT");
    check(ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0 && ai.value == 12345, "EVIOCGABS follows the stream");
    char small[10];
    check(read(fd, small, sizeof small) < 0 && errno == EINVAL, "read < one event: EINVAL");

    // Force feedback: upload, play, stop.
    struct ff_effect fx;
    memset(&fx, 0, sizeof fx);
    fx.type = FF_RUMBLE;
    fx.id = -1;
    fx.u.rumble.strong_magnitude = 0x8000;
    fx.u.rumble.weak_magnitude = 0x4000;
    fx.replay.length = 250;
    check(ioctl(fd, EVIOCSFF, &fx) == 0 && fx.id >= 0, "EVIOCSFF assigns an id");
    struct input_event play = { .type = EV_FF, .code = (unsigned short)fx.id, .value = 1 };
    check(write(fd, &play, sizeof play) == sizeof play, "write EV_FF play");
    play.value = 0;
    check(write(fd, &play, sizeof play) == sizeof play, "write EV_FF stop");
    check(ioctl(fd, EVIOCRMFF, fx.id) == 0, "EVIOCRMFF");
    close(fd);
    printf(fails ? "== evdev: FAIL\n" : "== evdev: ok\n");
    return fails != 0;
}
