// See ioctl_tty.h. Every Linux constant here is the aarch64 value (the
// generic asm-generic set); Darwin's come from its headers.
//
// Why this matters: FEX's glibc calls tcgetattr() at start-up, which is
// TCGETS2 (0x802c542a) on a modern glibc; bash does the same and then reads
// the result. With ioctl answering -ENOSYS both saw an "error" that Linux
// never produces for a terminal, and bash dereferenced a null pointer 0x514
// bytes in during start-up (benchmarks/stage5-fex.txt).
#include "lxrt.h"
#include "ioctl_tty.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/filio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define LERR(e) (-(long)lxrt_errno_to_linux(e))
bool lxrt_trace_on(void);

// ---- Linux requests (asm-generic / aarch64)
enum {
    L_TCGETS = 0x5401, L_TCSETS = 0x5402, L_TCSETSW = 0x5403, L_TCSETSF = 0x5404,
    L_TCFLSH = 0x540b, L_TIOCEXCL = 0x540c, L_TIOCNXCL = 0x540d, L_TIOCSCTTY = 0x540e,
    L_TIOCGPGRP = 0x540f, L_TIOCSPGRP = 0x5410, L_TIOCOUTQ = 0x5411,
    L_TIOCGWINSZ = 0x5413, L_TIOCSWINSZ = 0x5414, L_FIONREAD = 0x541b,
    L_FIONBIO = 0x5421, L_TIOCNOTTY = 0x5422, L_TIOCGSID = 0x5429,
    L_FIONCLEX = 0x5450, L_FIOCLEX = 0x5451,
    L_TCGETS2 = 0x802c542a, L_TCSETS2 = 0x402c542b, L_TCSETSW2 = 0x402c542c,
    L_TCSETSF2 = 0x402c542d,
    L_TIOCGPTN = 0x80045430, L_TIOCSPTLCK = 0x40045431,
};

// ---- Linux struct termios (36 bytes) / termios2 (44 bytes), NCCS = 19
struct linux_termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
    // termios2 adds:
    uint32_t c_ispeed, c_ospeed;
} __attribute__((packed));
enum { L_TERMIOS_SIZE = 36, L_TERMIOS2_SIZE = 44 };

// Flag tables: {linux, darwin}. Only bits that exist on both are carried.
struct bit { uint32_t l; unsigned long d; };
static const struct bit iflag_bits[] = {
    {0x0001, IGNBRK}, {0x0002, BRKINT}, {0x0004, IGNPAR}, {0x0008, PARMRK},
    {0x0010, INPCK},  {0x0020, ISTRIP}, {0x0040, INLCR},  {0x0080, IGNCR},
    {0x0100, ICRNL},  {0x0400, IXON},   {0x0800, IXANY},  {0x1000, IXOFF},
    {0x2000, IMAXBEL}, {0x4000, IUTF8},
};
static const struct bit oflag_bits[] = {
    {0x0001, OPOST}, {0x0004, ONLCR}, {0x0008, OCRNL}, {0x0010, ONOCR},
    {0x0020, ONLRET},
};
static const struct bit cflag_bits[] = {
    {0x0080, CREAD}, {0x0100, PARENB}, {0x0200, PARODD}, {0x0400, HUPCL},
    {0x0800, CLOCAL}, {0x0040, CSTOPB},
};
static const struct bit lflag_bits[] = {
    {0x0001, ISIG},   {0x0002, ICANON}, {0x0008, ECHO},   {0x0010, ECHOE},
    {0x0020, ECHOK},  {0x0040, ECHONL}, {0x0080, NOFLSH}, {0x0100, TOSTOP},
    {0x0200, ECHOCTL}, {0x0400, ECHOPRT}, {0x0800, ECHOKE}, {0x4000, PENDIN},
    {0x8000, IEXTEN},
};
// c_cc: Linux index -> Darwin index (-1 = no equivalent)
static const int cc_map[19] = {
    VINTR, VQUIT, VERASE, VKILL, VEOF, VTIME, VMIN, -1 /* VSWTC */,
    VSTART, VSTOP, VSUSP, VEOL, VREPRINT, VDISCARD, VWERASE, VLNEXT, VEOL2,
    -1, -1,
};

static unsigned long to_darwin(uint32_t v, const struct bit *t, size_t n)
{
    unsigned long r = 0;
    for (size_t i = 0; i < n; i++) if (v & t[i].l) r |= t[i].d;
    return r;
}
static uint32_t to_linux(unsigned long v, const struct bit *t, size_t n)
{
    uint32_t r = 0;
    for (size_t i = 0; i < n; i++) if (v & t[i].d) r |= t[i].l;
    return r;
}
#define N(a) (sizeof(a) / sizeof((a)[0]))

// Linux packs the baud rate into c_cflag (CBAUD = 0x100f); Darwin keeps it in
// c_ispeed/c_ospeed as a plain number.
static uint32_t speed_to_linux(speed_t s)
{
    static const struct { speed_t d; uint32_t l; } tab[] = {
        {0,1}, {50,1}, {75,2}, {110,3}, {134,4}, {150,5}, {200,6}, {300,7},
        {600,8}, {1200,9}, {1800,10}, {2400,11}, {4800,12}, {9600,13},
        {19200,14}, {38400,15}, {57600,0x1001}, {115200,0x1002}, {230400,0x1003},
    };
    for (size_t i = 0; i < N(tab); i++) if (tab[i].d == s) return tab[i].l;
    return 15; // B38400, the traditional default
}
static speed_t speed_to_darwin(uint32_t cbaud)
{
    static const struct { uint32_t l; speed_t d; } tab[] = {
        {0,0}, {1,50}, {2,75}, {3,110}, {4,134}, {5,150}, {6,200}, {7,300},
        {8,600}, {9,1200}, {10,1800}, {11,2400}, {12,4800}, {13,9600},
        {14,19200}, {15,38400}, {0x1001,57600}, {0x1002,115200}, {0x1003,230400},
    };
    for (size_t i = 0; i < N(tab); i++) if (tab[i].l == cbaud) return tab[i].d;
    return 38400;
}

static void termios_to_linux(const struct termios *d, struct linux_termios *l)
{
    memset(l, 0, sizeof *l);
    l->c_iflag = to_linux(d->c_iflag, iflag_bits, N(iflag_bits));
    l->c_oflag = to_linux(d->c_oflag, oflag_bits, N(oflag_bits));
    l->c_cflag = to_linux(d->c_cflag, cflag_bits, N(cflag_bits));
    switch (d->c_cflag & CSIZE) {       // CSIZE: Linux 0x30 = CS8 .. Darwin 0x300
    case CS5: break; case CS6: l->c_cflag |= 0x10; break;
    case CS7: l->c_cflag |= 0x20; break; default: l->c_cflag |= 0x30; break;
    }
    l->c_cflag |= speed_to_linux(d->c_ospeed);
    l->c_lflag = to_linux(d->c_lflag, lflag_bits, N(lflag_bits));
    for (int i = 0; i < 19; i++)
        l->c_cc[i] = cc_map[i] >= 0 ? d->c_cc[cc_map[i]] : 0;
    l->c_ispeed = (uint32_t)d->c_ispeed;
    l->c_ospeed = (uint32_t)d->c_ospeed;
}

static void termios_to_darwin(const struct linux_termios *l, struct termios *d)
{
    // Start from the current settings so bits Linux cannot express survive.
    d->c_iflag = to_darwin(l->c_iflag, iflag_bits, N(iflag_bits));
    d->c_oflag = to_darwin(l->c_oflag, oflag_bits, N(oflag_bits));
    d->c_cflag = to_darwin(l->c_cflag, cflag_bits, N(cflag_bits));
    switch (l->c_cflag & 0x30) {
    case 0x00: d->c_cflag |= CS5; break; case 0x10: d->c_cflag |= CS6; break;
    case 0x20: d->c_cflag |= CS7; break; default: d->c_cflag |= CS8; break;
    }
    d->c_lflag = to_darwin(l->c_lflag, lflag_bits, N(lflag_bits));
    for (int i = 0; i < 19; i++)
        if (cc_map[i] >= 0) d->c_cc[cc_map[i]] = l->c_cc[i];
    speed_t sp = speed_to_darwin(l->c_cflag & 0x100f);
    d->c_ispeed = sp; d->c_ospeed = sp;
}

long lxrt_ioctl(int fd, unsigned long lreq, uint64_t arg)
{
    void *p = (void *)arg;
    if (lxrt_evdev_is(fd))
        return lxrt_evdev_ioctl(fd, lreq, arg);
    switch ((uint32_t)lreq) {
    case L_TCGETS: case L_TCGETS2: {
        struct termios d;
        if (tcgetattr(fd, &d) != 0) return LERR(errno);
        if (!p) return LERR(EFAULT);
        struct linux_termios l;
        termios_to_linux(&d, &l);
        memcpy(p, &l, lreq == L_TCGETS2 ? L_TERMIOS2_SIZE : L_TERMIOS_SIZE);
        return 0;
    }
    case L_TCSETS: case L_TCSETSW: case L_TCSETSF:
    case L_TCSETS2: case L_TCSETSW2: case L_TCSETSF2: {
        if (!p) return LERR(EFAULT);
        struct termios d;
        if (tcgetattr(fd, &d) != 0) return LERR(errno);
        struct linux_termios l;
        memset(&l, 0, sizeof l);
        bool two = lreq == L_TCSETS2 || lreq == L_TCSETSW2 || lreq == L_TCSETSF2;
        memcpy(&l, p, two ? L_TERMIOS2_SIZE : L_TERMIOS_SIZE);
        termios_to_darwin(&l, &d);
        int act = (lreq == L_TCSETS || lreq == L_TCSETS2) ? TCSANOW
                : (lreq == L_TCSETSW || lreq == L_TCSETSW2) ? TCSADRAIN : TCSAFLUSH;
        return tcsetattr(fd, act, &d) == 0 ? 0 : LERR(errno);
    }
    case L_TIOCGWINSZ: {
        struct winsize w;                   // same layout on both: 4 x u16
        if (ioctl(fd, TIOCGWINSZ, &w) != 0) return LERR(errno);
        if (!p) return LERR(EFAULT);
        memcpy(p, &w, sizeof w);
        return 0;
    }
    case L_TIOCSWINSZ: {
        if (!p) return LERR(EFAULT);
        struct winsize w; memcpy(&w, p, sizeof w);
        return ioctl(fd, TIOCSWINSZ, &w) == 0 ? 0 : LERR(errno);
    }
    case L_TIOCGPGRP: {
        pid_t g = tcgetpgrp(fd);
        if (g < 0) return LERR(errno);
        if (!p) return LERR(EFAULT);
        *(int32_t *)p = (int32_t)g;
        return 0;
    }
    case L_TIOCSPGRP:
        if (!p) return LERR(EFAULT);
        return tcsetpgrp(fd, *(int32_t *)p) == 0 ? 0 : LERR(errno);
    case L_TIOCGSID: {
        pid_t s = tcgetsid(fd);
        if (s < 0) return LERR(errno);
        if (!p) return LERR(EFAULT);
        *(int32_t *)p = (int32_t)s;
        return 0;
    }
    case L_TIOCSCTTY: return ioctl(fd, TIOCSCTTY, 0) == 0 ? 0 : LERR(errno);
    case L_TIOCNOTTY: return ioctl(fd, TIOCNOTTY, 0) == 0 ? 0 : LERR(errno);
    case L_TIOCEXCL:  return ioctl(fd, TIOCEXCL, 0) == 0 ? 0 : LERR(errno);
    case L_TIOCNXCL:  return ioctl(fd, TIOCNXCL, 0) == 0 ? 0 : LERR(errno);
    case L_TIOCOUTQ: {
        int n = 0;
        if (ioctl(fd, TIOCOUTQ, &n) != 0) return LERR(errno);
        if (!p) return LERR(EFAULT);
        *(int32_t *)p = n;
        return 0;
    }
    case L_TCFLSH: {
        // arg: TCIFLUSH 0, TCOFLUSH 1, TCIOFLUSH 2 -- same numbering.
        int q = (int)arg == 0 ? TCIFLUSH : (int)arg == 1 ? TCOFLUSH : TCIOFLUSH;
        return tcflush(fd, q) == 0 ? 0 : LERR(errno);
    }
    case L_FIONREAD: {
        int n = 0;
        if (ioctl(fd, FIONREAD, &n) != 0) return LERR(errno);
        if (!p) return LERR(EFAULT);
        *(int32_t *)p = n;
        return 0;
    }
    case L_FIONBIO: {
        if (!p) return LERR(EFAULT);
        int on = *(int32_t *)p;
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0) return LERR(errno);
        fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        return fcntl(fd, F_SETFL, fl) == 0 ? 0 : LERR(errno);
    }
    case L_FIOCLEX:  return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 ? 0 : LERR(errno);
    case L_FIONCLEX: return fcntl(fd, F_SETFD, 0) == 0 ? 0 : LERR(errno);
    case L_TIOCGPTN: case L_TIOCSPTLCK:
        // Pseudo-terminal management: Darwin has posix_openpt/grantpt/ptsname
        // but no pts number ioctl. Not a terminal request a program can
        // recover from silently -- say so.
        if (lxrt_trace_on())
            fprintf(lxrt_trace_stream(), "[lxrt] ioctl: pty request 0x%lx not supported\n", lreq);
        return LERR(ENOTTY);
    default:
        if (lxrt_trace_on())
            fprintf(lxrt_trace_stream(), "[lxrt] ioctl: unknown request 0x%lx on fd %d -> ENOTTY\n",
                    lreq, fd);
        return LERR(ENOTTY);
    }
}
