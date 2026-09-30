// readv, preadv, pwritev, preadv2 and pwritev2 were ENOSYS under lxrun.
// And read() on a SOCK_SEQPACKET pair (a Darwin datagram pair underneath)
// blocked for good when the peer died without writing: Steam's web helper
// waits for each zygote's hello that way, and Linux returns 0 there.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;

static void check(int good, const char *what)
{
    printf("  %s  %s\n", good ? "OK " : "MAL", what);
    if (!good)
        fails++;
}

static long mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(void)
{
    char path[] = "/tmp/lxrt-vector-io-XXXXXX";
    int f = mkstemp(path);
    unlink(path);

    struct iovec w[2] = { { "hola ", 5 }, { "mundo", 5 } };
    check(pwritev(f, w, 2, 0) == 10, "pwritev: 10 bytes at 0");
    char a[4] = {0}, b[8] = {0};
    struct iovec r[2] = { { a, 4 }, { b, 6 } };
    check(preadv(f, r, 2, 0) == 10 && !memcmp(a, "hola", 4) && !memcmp(b, " mundo", 6),
          "preadv: 10 bytes back, split 4 + 6");
    check(pwritev2(f, w, 1, 10, RWF_DSYNC) == 5, "pwritev2 RWF_DSYNC: 5 bytes at 10");
    memset(a, 0, sizeof a);
    struct iovec one = { a, 4 };
    check(preadv2(f, &one, 1, 11, 0) == 4 && !memcmp(a, "ola ", 4), "preadv2 at 11: \"ola \"");
    check(lseek(f, 0, SEEK_CUR) == 0, "the positional calls left the file position alone");
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    check(readv(f, r, 2) == 10 && lseek(f, 0, SEEK_CUR) == 10, "readv: 10 bytes, position now 10");
    check(preadv2(f, &one, 1, -1, 0) == 4 && !memcmp(a, "hola", 4) && lseek(f, 0, SEEK_CUR) == 14,
          "preadv2 offset -1 reads at the file position and moves it");
    errno = 0;
    check(preadv2(f, &one, 1, 0, RWF_NOWAIT) == -1 && errno == EOPNOTSUPP,
          "preadv2 RWF_NOWAIT: EOPNOTSUPP");
    close(f);

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp) != 0) {
        check(0, "socketpair SOCK_SEQPACKET");
    } else {
        // A message first: boundaries kept, through readv.
        pid_t c = fork();
        if (c == 0) {
            close(sp[0]);
            write(sp[1], "hello", 5);
            usleep(200 * 1000);
            _exit(0);               // then die without another word
        }
        close(sp[1]);
        char buf[64];
        struct iovec v = { buf, sizeof buf };
        long got = readv(sp[0], &v, 1);
        check(got == 5 && !memcmp(buf, "hello", 5), "readv on a seqpacket socket: one message of 5 bytes");
        long t0 = mono_ms();
        long n = read(sp[0], buf, sizeof buf);
        long ms = mono_ms() - t0;
        char what[128];
        snprintf(what, sizeof what, "read after the peer died: %ld (end of file) in %ld ms", n, ms);
        check(n == 0 && ms < 3000, what);
        waitpid(c, NULL, 0);
        close(sp[0]);
    }
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
