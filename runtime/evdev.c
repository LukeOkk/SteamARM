// /dev/input/eventN for guest programs: game controllers without a kernel.
//
// The Mac side (tools/inputd, steamarm-inputd) reads the real controllers and
// publishes each player as a Unix stream socket /tmp/lxrt-input/eventN plus a
// description in /tmp/lxrt-input/meta/eventN (tools/inputd/PROTOCOL.md). The
// guest's /dev/input is that directory. Opening an event node connects to the
// socket; the descriptor then behaves like a Linux evdev node: the EVIOC*
// ioctls answer from the description and from the state seen in the stream,
// read() returns whole struct input_event records, and force feedback
// (EVIOCSFF + write of EV_FF) becomes a rumble record to the daemon.
//
// SDL (winebus in Proton, native games) finds these the way it does inside
// any container: scanning /dev/input and watching it with inotify.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "lxrt.h"

#define LERR(e) (-lxrt_errno_to_linux(e))

#define EVDEV_DIR "/tmp/lxrt-input"
#define EV_FDS 65536
#define KEY_BYTES 96            // KEY_MAX 0x2ff
#define ABS_N 64                // ABS_MAX 0x3f
#define FF_BYTES 16             // FF_MAX 0x7f
#define MAX_EFFECTS 16
#define EVENT_SIZE 24

struct absinfo { int32_t value, min, max, fuzz, flat, res; };

struct evdev {
    int num;                            // N of eventN
    uint64_t ino;                       // the socket's inode: the entry is stale once it differs
    char name[128], phys[64], uniq[64];
    uint16_t id[4];                     // bustype vendor product version
    uint8_t prop[8];
    uint8_t keybits[KEY_BYTES];
    uint8_t absbits[8];
    uint8_t ffbits[FF_BYTES];
    int effects;
    struct absinfo abs[ABS_N];
    uint8_t keystate[KEY_BYTES];
    int clock;                          // EVIOCSCLOCKID: 0 realtime, 1 monotonic
    struct { int used; uint16_t strong, weak, length; } fx[MAX_EFFECTS];
    uint32_t gain;                      // FF_GAIN, 0..0xffff
    int grabbed;
};

static _Atomic(struct evdev *) g_ev[EV_FDS];

static void set_bit(uint8_t *bits, size_t nbytes, unsigned b)
{
    if (b / 8 < nbytes)
        bits[b / 8] |= (uint8_t)(1u << (b % 8));
}
static int test_bit(const uint8_t *bits, size_t nbytes, unsigned b)
{
    return b / 8 < nbytes && (bits[b / 8] >> (b % 8)) & 1;
}

// The host path of the guest's /dev/input[/...]; NULL for other paths.
const char *lxrt_evdev_translate(const char *path, char *buf, size_t n)
{
    if (strncmp(path, "/dev/input", 10) != 0 || (path[10] != '\0' && path[10] != '/'))
        return NULL;
    int k = snprintf(buf, n, "%s%s", EVDEV_DIR, path + 10);
    return k > 0 && (size_t)k < n ? buf : NULL;
}

// "…/lxrt-input/eventN" -> N, else -1.
static int node_number(const char *host)
{
    size_t l = strlen(EVDEV_DIR);
    if (strncmp(host, EVDEV_DIR "/event", l + 6) != 0)
        return -1;
    const char *p = host + l + 6;
    if (!*p)
        return -1;
    int n = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9' || n > 1000)
            return -1;
        n = n * 10 + (*p - '0');
    }
    return n;
}

static int load_meta(struct evdev *e)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, EVDEV_DIR "/meta/event%d", e->num);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *rest = strchr(line, ' ');
        const char *arg = rest ? rest + 1 : "";
        if (rest)
            *rest = 0;
        if (!strcmp(line, "name")) {
            snprintf(e->name, sizeof e->name, "%s", arg);
        } else if (!strcmp(line, "phys")) {
            snprintf(e->phys, sizeof e->phys, "%s", arg);
        } else if (!strcmp(line, "uniq")) {
            snprintf(e->uniq, sizeof e->uniq, "%s", arg);
        } else if (!strcmp(line, "id")) {
            unsigned a, b, c, d;
            if (sscanf(arg, "%x %x %x %x", &a, &b, &c, &d) == 4) {
                e->id[0] = (uint16_t)a; e->id[1] = (uint16_t)b;
                e->id[2] = (uint16_t)c; e->id[3] = (uint16_t)d;
            }
        } else if (!strcmp(line, "effects")) {
            e->effects = atoi(arg);
        } else if (!strcmp(line, "abs")) {
            int c, mn, mx, fz, fl, rs;
            if (sscanf(arg, "%d %d %d %d %d %d", &c, &mn, &mx, &fz, &fl, &rs) == 6 &&
                c >= 0 && c < ABS_N) {
                set_bit(e->absbits, sizeof e->absbits, (unsigned)c);
                e->abs[c] = (struct absinfo){ mn <= 0 && mx >= 0 ? 0 : mn, mn, mx, fz, fl, rs };
            }
        } else if (!strcmp(line, "key") || !strcmp(line, "ff") || !strcmp(line, "prop")) {
            uint8_t *bits = line[0] == 'k' ? e->keybits : line[0] == 'f' ? e->ffbits : e->prop;
            size_t nb = line[0] == 'k' ? sizeof e->keybits : line[0] == 'f' ? sizeof e->ffbits : sizeof e->prop;
            for (char *t = strtok((char *)arg, " "); t; t = strtok(NULL, " "))
                set_bit(bits, nb, (unsigned)atoi(t));
        }
    }
    fclose(f);
    if (e->effects <= 0 || e->effects > MAX_EFFECTS)
        e->effects = MAX_EFFECTS;
    return 0;
}

static void apply(struct evdev *e, const uint8_t *rec)
{
    uint16_t type, code;
    int32_t value;
    memcpy(&type, rec + 16, 2);
    memcpy(&code, rec + 18, 2);
    memcpy(&value, rec + 20, 4);
    if (type == 1 && code / 8 < KEY_BYTES) {
        if (value)
            e->keystate[code / 8] |= (uint8_t)(1u << (code % 8));
        else
            e->keystate[code / 8] &= (uint8_t)~(1u << (code % 8));
    } else if (type == 3 && code < ABS_N) {
        e->abs[code].value = value;
    }
}

// The daemon's snapshot (state, then SYN_REPORT) right after connecting: it
// answers EVIOCGABS/EVIOCGKEY, and like a Linux open the guest gets no events.
static void take_snapshot(int fd, struct evdev *e)
{
    uint8_t rec[EVENT_SIZE];
    size_t have = 0;
    for (int spins = 0; spins < 400; ) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0)
            return;
        ssize_t r = recv(fd, rec + have, sizeof rec - have, 0);
        if (r <= 0)
            return;
        have += (size_t)r;
        if (have < sizeof rec)
            continue;
        have = 0;
        spins++;
        uint16_t type, code;
        memcpy(&type, rec + 16, 2);
        memcpy(&code, rec + 18, 2);
        if (type == 0 && code == 0)
            return;
        apply(e, rec);
    }
}

// openat of a /dev/input node, after Darwin refused to open the socket.
// Returns a descriptor, or -1 with errno.
int lxrt_evdev_open(const char *host, int lflags)
{
    int num = node_number(host);
    if (num < 0) {
        errno = EOPNOTSUPP;
        return -1;
    }
    struct evdev *e = calloc(1, sizeof *e);
    if (!e) {
        errno = ENOMEM;
        return -1;
    }
    e->num = num;
    e->gain = 0xffff;
    if (load_meta(e) != 0) {
        free(e);
        errno = ENODEV;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        free(e);
        return -1;
    }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", host);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0 || fd >= EV_FDS) {
        int err = errno == ECONNREFUSED ? ENODEV : errno;
        close(fd);
        free(e);
        errno = err;
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    struct stat sst;
    e->ino = fstat(fd, &sst) == 0 ? (uint64_t)sst.st_ino : 0;
    take_snapshot(fd, e);
    if (lflags & 02000000)                           // O_CLOEXEC
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (lflags & 04000)                              // O_NONBLOCK
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct evdev *old = atomic_exchange(&g_ev[fd], e);
    free(old);
    return fd;
}

// As in socket.c's netlink table: a number closed by close_range, dup2 or
// exec never reaches lxrt_evdev_close, and must not make an unrelated
// descriptor that later gets it behave like a controller.
static struct evdev *get(int fd)
{
    struct evdev *e = fd >= 0 && fd < EV_FDS ? atomic_load(&g_ev[fd]) : NULL;
    if (!e)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) == 0 && (uint64_t)st.st_ino == e->ino)
        return e;
    if (atomic_compare_exchange_strong(&g_ev[fd], &e, NULL))
        free(e);
    return NULL;
}

int lxrt_evdev_is(int fd) { return get(fd) != NULL; }

void lxrt_evdev_close(int fd)
{
    if (fd < 0 || fd >= EV_FDS)
        return;
    free(atomic_exchange(&g_ev[fd], NULL));
}

// stat of a node (or of an open one): a character device, input major 13,
// minor 64+N, as on Linux. SDL tells devices apart by st_rdev.
void lxrt_evdev_fix_stat(const char *host, int fd, struct stat *st)
{
    int num = -1;
    struct evdev *e = get(fd);
    if (e)
        num = e->num;
    else if (host && S_ISSOCK(st->st_mode))
        num = node_number(host);
    if (num < 0)
        return;
    st->st_mode = S_IFCHR | 0660;
    st->st_rdev = makedev(13, 64 + num);
    st->st_size = 0;
}

static void stamp(struct evdev *e, uint8_t *rec)
{
    struct timespec ts;
    clock_gettime(e->clock == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    int64_t sec = ts.tv_sec, usec = ts.tv_nsec / 1000;
    memcpy(rec, &sec, 8);
    memcpy(rec + 8, &usec, 8);
}

long lxrt_evdev_read(int fd, void *buf, size_t n)
{
    struct evdev *e = get(fd);
    if (n < EVENT_SIZE)
        return LERR(EINVAL);
    n -= n % EVENT_SIZE;
    ssize_t r = recv(fd, buf, n, 0);
    if (r < 0)
        return LERR(errno);
    if (r == 0)
        return LERR(ENODEV);                        // the device went away
    // A record is never split for the guest: wait for the rest of it.
    while (r % EVENT_SIZE) {
        ssize_t k = recv(fd, (uint8_t *)buf + r, EVENT_SIZE - (size_t)(r % EVENT_SIZE), MSG_WAITALL);
        if (k <= 0)
            return LERR(ENODEV);
        r += k;
    }
    for (ssize_t i = 0; i < r; i += EVENT_SIZE) {
        apply(e, (uint8_t *)buf + i);
        if (e->clock == 1)
            stamp(e, (uint8_t *)buf + i);
    }
    return r;
}

static void send_rumble(int fd, uint16_t strong, uint16_t weak, uint32_t ms)
{
    uint8_t rec[EVENT_SIZE] = {0};
    uint32_t magic = 0x4c424d52;
    memcpy(rec, &magic, 4);
    memcpy(rec + 4, &strong, 2);
    memcpy(rec + 6, &weak, 2);
    memcpy(rec + 8, &ms, 4);
    (void)send(fd, rec, sizeof rec, 0);
}

long lxrt_evdev_write(int fd, const void *buf, size_t n)
{
    struct evdev *e = get(fd);
    if (n < EVENT_SIZE)
        return LERR(EINVAL);
    n -= n % EVENT_SIZE;
    for (size_t i = 0; i < n; i += EVENT_SIZE) {
        const uint8_t *rec = (const uint8_t *)buf + i;
        uint16_t type, code;
        int32_t value;
        memcpy(&type, rec + 16, 2);
        memcpy(&code, rec + 18, 2);
        memcpy(&value, rec + 20, 4);
        if (type != 0x15)                            // EV_FF only; LEDs etc. are dropped
            continue;
        if (code == 96) {                            // FF_GAIN
            e->gain = (uint32_t)value & 0xffff;
            continue;
        }
        if (code >= MAX_EFFECTS || !e->fx[code].used)
            continue;
        if (value > 0) {
            uint16_t s = (uint16_t)((e->fx[code].strong * e->gain) / 0xffff);
            uint16_t w = (uint16_t)((e->fx[code].weak * e->gain) / 0xffff);
            send_rumble(fd, s, w, e->fx[code].length);
        } else {
            send_rumble(fd, 0, 0, 0);
        }
    }
    return (long)n;
}

static long copy_out(void *p, size_t len, const void *src, size_t have)
{
    if (!p)
        return LERR(EFAULT);
    size_t k = len < have ? len : have;
    memcpy(p, src, k);
    return (long)k;
}

static long copy_str(void *p, size_t len, const char *s)
{
    if (!len)
        return 0;
    size_t have = strlen(s) + 1;
    long k = copy_out(p, len, s, have);
    if (k > 0 && (size_t)k == len)
        ((char *)p)[len - 1] = 0;
    return k;
}

long lxrt_evdev_ioctl(int fd, unsigned long req, uint64_t arg)
{
    struct evdev *e = get(fd);
    void *p = (void *)arg;
    unsigned dir = (unsigned)(req >> 30) & 3, size = (unsigned)(req >> 16) & 0x3fff;
    unsigned type = (unsigned)(req >> 8) & 0xff, nr = (unsigned)req & 0xff;
    if (type != 'E')
        return LERR(ENOTTY);
    if (dir == 2) {                                  // _IOC_READ
        switch (nr) {
        case 0x01: { int v = 0x010001; return copy_out(p, size, &v, 4) < 0 ? LERR(EFAULT) : 0; }
        case 0x02: return copy_out(p, size, e->id, 8) < 0 ? LERR(EFAULT) : 0;
        case 0x03: { uint32_t rep[2] = { 250, 33 }; return copy_out(p, size, rep, 8) < 0 ? LERR(EFAULT) : 0; }
        case 0x06: return copy_str(p, size, e->name);
        case 0x07: return copy_str(p, size, e->phys);
        case 0x08: return copy_str(p, size, e->uniq);
        case 0x09: return copy_out(p, size, e->prop, sizeof e->prop);
        case 0x18: return copy_out(p, size, e->keystate, sizeof e->keystate);
        case 0x19: case 0x1a: case 0x1b: {           // LED, SND, SW state: none
            uint8_t z[16] = {0};
            return copy_out(p, size, z, size < sizeof z ? size : sizeof z);
        }
        case 0x84: return copy_out(p, size, &e->effects, 4) < 0 ? LERR(EFAULT) : 0;
        default:
            break;
        }
        if (nr >= 0x20 && nr < 0x40) {               // EVIOCGBIT(ev, len)
            unsigned ev = nr - 0x20;
            uint8_t bits[KEY_BYTES] = {0};
            switch (ev) {
            case 0: {                                // the event types
                set_bit(bits, sizeof bits, 0);
                int any = 0;
                for (size_t i = 0; i < sizeof e->keybits; i++) any |= e->keybits[i];
                if (any) set_bit(bits, sizeof bits, 1);
                any = 0;
                for (size_t i = 0; i < sizeof e->absbits; i++) any |= e->absbits[i];
                if (any) set_bit(bits, sizeof bits, 3);
                any = 0;
                for (size_t i = 0; i < sizeof e->ffbits; i++) any |= e->ffbits[i];
                if (any) set_bit(bits, sizeof bits, 0x15);
                return copy_out(p, size, bits, 4);
            }
            case 1: return copy_out(p, size, e->keybits, sizeof e->keybits);
            case 3: return copy_out(p, size, e->absbits, sizeof e->absbits);
            case 0x15: return copy_out(p, size, e->ffbits, sizeof e->ffbits);
            default: return copy_out(p, size, bits, size < 8 ? size : 8);  // none of that type
            }
        }
        if (nr >= 0x40 && nr < 0x40 + ABS_N) {       // EVIOCGABS(abs)
            unsigned c = nr - 0x40;
            if (!test_bit(e->absbits, sizeof e->absbits, c))
                return LERR(EINVAL);
            return copy_out(p, size, &e->abs[c], sizeof e->abs[c]) < 0 ? LERR(EFAULT) : 0;
        }
        return LERR(EINVAL);
    }
    if (dir == 1) {                                  // _IOC_WRITE
        if (nr >= 0xc0 && nr < 0xc0 + ABS_N) {       // EVIOCSABS: accepted, ranges kept
            unsigned c = nr - 0xc0;
            if (!p) return LERR(EFAULT);
            if (test_bit(e->absbits, sizeof e->absbits, c)) {
                struct absinfo a;
                memcpy(&a, p, sizeof a);
                e->abs[c].fuzz = a.fuzz;
                e->abs[c].flat = a.flat;
            }
            return 0;
        }
        switch (nr) {
        case 0x80: {                                 // EVIOCSFF(struct ff_effect)
            if (!p) return LERR(EFAULT);
            uint8_t *fx = p;
            uint16_t ftype;
            int16_t id;
            memcpy(&ftype, fx, 2);
            memcpy(&id, fx + 2, 2);
            // The union follows a pointer-aligned header: offset 16 in the
            // 64-bit struct (48 bytes), 12 in the i386 one (44 bytes).
            const uint8_t *u = fx + (size == 44 ? 12 : 16);
            uint16_t length;
            memcpy(&length, fx + 10, 2);             // replay.length
            uint16_t strong = 0, weak = 0;
            if (ftype == 80) {                       // FF_RUMBLE
                memcpy(&strong, u, 2);
                memcpy(&weak, u + 2, 2);
            } else if (ftype == 81) {                // FF_PERIODIC: magnitude
                int16_t m;
                memcpy(&m, u + 4, 2);
                strong = weak = (uint16_t)(m < 0 ? -m : m) * 2;
            } else if (ftype == 82) {                // FF_CONSTANT: level
                int16_t l;
                memcpy(&l, u, 2);
                strong = weak = (uint16_t)(l < 0 ? -l : l) * 2;
            } else {
                return LERR(EINVAL);
            }
            if (id < 0) {
                for (id = 0; id < e->effects && e->fx[id].used; id++)
                    ;
                if (id >= e->effects)
                    return LERR(ENOSPC);
                memcpy(fx + 2, &id, 2);
            } else if (id >= e->effects) {
                return LERR(EINVAL);
            }
            e->fx[id].used = 1;
            e->fx[id].strong = strong;
            e->fx[id].weak = weak;
            e->fx[id].length = length;
            return 0;
        }
        case 0x81: {                                 // EVIOCRMFF(int id)
            long id = (long)arg;
            if (id < 0 || id >= MAX_EFFECTS)
                return LERR(EINVAL);
            if (e->fx[id].used)
                send_rumble(fd, 0, 0, 0);
            e->fx[id].used = 0;
            return 0;
        }
        case 0x90:                                   // EVIOCGRAB: not exclusive here
            e->grabbed = arg != 0;
            return 0;
        case 0x91:                                   // EVIOCREVOKE
            return 0;
        case 0x93:                                   // EVIOCSMASK
            return 0;
        case 0xa0: {                                 // EVIOCSCLOCKID
            int id;
            if (!p) return LERR(EFAULT);
            memcpy(&id, p, 4);
            if (id != 0 && id != 1 && id != 7)       // REALTIME, MONOTONIC, BOOTTIME
                return LERR(EINVAL);
            e->clock = id == 0 ? 0 : 1;
            return 0;
        }
        default:
            break;
        }
    }
    return LERR(EINVAL);
}
