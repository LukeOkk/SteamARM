// Pseudo-terminals the Linux way (Termux's terminal is one): posix_openpt on
// /dev/ptmx, grantpt/unlockpt, ptsname giving /dev/pts/N, the slave opened by
// that name and by TIOCGPTPEER, bytes both ways, the window size, ttyname,
// and a child that makes the slave its controlling terminal.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifndef TIOCGPTPEER
#define TIOCGPTPEER 0x5441
#endif

static int fails;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "OK " : "MAL", what);
    fails += !ok;
}

int main(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    check(m >= 0, "posix_openpt (/dev/ptmx)");
    if (m < 0) { printf("FAIL\n"); return 1; }
    check(grantpt(m) == 0 && unlockpt(m) == 0, "grantpt and unlockpt");
    char name[64] = "";
    check(ptsname_r(m, name, sizeof name) == 0 && !strncmp(name, "/dev/pts/", 9), "ptsname: a /dev/pts/N name");
    printf("       (%s)\n", name);
    // Before anyone opened the slave (apt, Termux): the master's modes and
    // window size can be set, and they are what the slave then has.
    struct termios mt;
    int got = tcgetattr(m, &mt);
    mt.c_lflag &= ~(tcflag_t)ECHO;
    mt.c_iflag |= IUTF8;
    struct winsize w0 = { .ws_row = 21, .ws_col = 77 };
    check(got == 0 && tcsetattr(m, TCSANOW, &mt) == 0 && ioctl(m, TIOCSWINSZ, &w0) == 0,
          "modes and window size set on the master before the slave is opened");
    int s = open(name, O_RDWR | O_NOCTTY);
    check(s >= 0 && isatty(s), "the slave opens by that name and is a terminal");
    struct termios st0;
    struct winsize w1 = { 0 };
    check(tcgetattr(s, &st0) == 0 && !(st0.c_lflag & ECHO) && (st0.c_iflag & IUTF8) &&
          ioctl(s, TIOCGWINSZ, &w1) == 0 && w1.ws_row == 21 && w1.ws_col == 77,
          "... and the slave has them (no echo, IUTF8, 21x77)");
    struct termios t;
    tcgetattr(s, &t);
    cfmakeraw(&t);
    check(tcsetattr(s, TCSANOW, &t) == 0, "raw mode on the slave");
    char buf[16] = "";
    write(s, "slave", 5);
    ssize_t n = read(m, buf, sizeof buf);
    check(n == 5 && !memcmp(buf, "slave", 5), "slave to master");
    write(m, "master", 6);
    n = read(s, buf, sizeof buf);
    check(n == 6 && !memcmp(buf, "master", 6), "master to slave");
    struct winsize w = { .ws_row = 33, .ws_col = 101 }, r = { 0 };
    ioctl(m, TIOCSWINSZ, &w);
    check(ioctl(s, TIOCGWINSZ, &r) == 0 && r.ws_row == 33 && r.ws_col == 101, "window size set on the master, read on the slave");
    char tn[64] = "";
    check(ttyname_r(s, tn, sizeof tn) == 0 && !strcmp(tn, name), "ttyname of the slave is its /dev/pts name");
    int peer = ioctl(m, TIOCGPTPEER, O_RDWR | O_NOCTTY);
    check(peer >= 0 && isatty(peer), "TIOCGPTPEER opens the slave");
    if (peer >= 0) close(peer);
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int c = open(name, O_RDWR);              // first terminal opened after setsid: the controlling one
        pid_t sid = -1;
        _exit(c >= 0 && ioctl(c, TIOCGSID, &sid) == 0 && sid == getpid() ? 0 : 1);
    }
    int st = 0;
    waitpid(p, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a new session's child gets the slave as its controlling terminal");
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
