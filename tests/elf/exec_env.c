// A guest that execs with an environment of its own, and a TMPDIR the host
// does not have (Termux does both): the runtime's settings survive the exec
// (LXRT_* carried over), its /proc works (/proc/self/exe), and an abstract
// socket can still be bound -- the runtime's own files do not follow the
// guest's TMPDIR.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) {
        int fails = 0;
        const char *mark = getenv("LXRT_TEST_EXEC_ENV");
        int ok = mark && !strcmp(mark, "kept");
        printf("  %s  the runtime's setting survived an exec with the guest's own environment\n", ok ? "OK " : "MAL");
        fails += !ok;
        char exe[512];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        ok = n > 0;
        if (n > 0) exe[n] = 0;
        printf("  %s  /proc/self/exe with TMPDIR=%s: %s\n", ok ? "OK " : "MAL", getenv("TMPDIR"), ok ? exe : "missing");
        fails += !ok;
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path + 1, sizeof a.sun_path - 1, "lxrt-exec-env-%d", getpid());
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        ok = bind(s, (struct sockaddr *)&a, sizeof(sa_family_t) + 1 + strlen(a.sun_path + 1)) == 0;
        printf("  %s  an abstract socket binds\n", ok ? "OK " : "MAL");
        fails += !ok;
        printf("%s\n", fails ? "FAIL" : "PASS");
        return fails != 0;
    }
    char *envp[] = { "TMPDIR=/data/data/com.example/files/usr/tmp", "HOME=/nowhere", NULL };
    char *args[] = { argv[0], "child", NULL };
    execve("/proc/self/exe", args, envp);
    perror("execve");
    return 2;
}
