// O_PATH names any file, including the ones Darwin will not open: a Unix
// socket (pressure-vessel checks the PulseAudio socket this way before it
// binds it into the container) and a FIFO (must not wait for a writer).
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int fails;
static void check(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

int main(void)
{
    const char *sp = "/tmp/opath_special.sock", *fp = "/tmp/opath_special.fifo";
    unlink(sp);
    unlink(fp);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, sp);
    check(bind(s, (struct sockaddr *)&a, sizeof a) == 0, "bind socket");

    int fd = open(sp, O_PATH | O_CLOEXEC);
    check(fd >= 0, "open(O_PATH) of a socket");
    struct stat st;
    check(fd >= 0 && fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode), "fstat says socket");
    check(fd >= 0 && fstatat(fd, "", &st, AT_EMPTY_PATH) == 0 && S_ISSOCK(st.st_mode),
          "fstatat(AT_EMPTY_PATH) says socket");
    char link[64], target[256] = "";
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n > 0) target[n] = 0;
    check(n > 0 && strcmp(target, sp) == 0, "/proc/self/fd names the socket");
    close(fd);
    check(open(sp, O_RDONLY) < 0, "plain open of a socket still fails");

    check(mkfifo(fp, 0600) == 0, "mkfifo");
    fd = open(fp, O_PATH);
    check(fd >= 0, "open(O_PATH) of a FIFO returns at once");
    check(fd >= 0 && fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode), "fstat says FIFO");
    close(fd);
    unlink(sp);
    unlink(fp);
    printf(fails ? "== opath special: FAIL\n" : "== opath special: ok\n");
    return fails != 0;
}
