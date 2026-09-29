// A stand-in for build/lxrun, for tests/launcher/run_steam_arm64.sh: no
// guest, no runtime. argv[1] ending in "/steam" is a client: after 1.5 s (past
// the script's first poll) it leaves one orphaned "zygote" -- a grandchild
// whose parent exits at once, re-exec'd as argv[0] .../steamrtarm64/zygote and
// so reparented to launchd, as benchmarks/stage23-frame-root.txt C2 saw the
// webhelper's -- then waits. Anything else waits. Every process marks itself
// as $FAKE_DIR/<role>.<STEAMARM_RUN_ID or none>.<pid>.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void note(const char *role)
{
    const char *d = getenv("FAKE_DIR"), *id = getenv("STEAMARM_RUN_ID");
    char p[1024];
    int fd;
    if (!d)
        return;
    snprintf(p, sizeof p, "%s/%s.%s.%d", d, role, id ? id : "none", (int)getpid());
    fd = open(p, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0)
        close(fd);
}

int main(int argc, char **argv)
{
    size_t n = argc > 1 ? strlen(argv[1]) : 0;
    if (n >= 6 && !strcmp(argv[1] + n - 6, "/steam")) {
        pid_t c;
        note("client");
        usleep(1500 * 1000);
        c = fork();
        if (c == 0) {
            if (fork() == 0) {
                char *zargv[] = { argv[0], "/tmp/armhome/.local/share/Steam/steamrtarm64/zygote", NULL };
                usleep(200 * 1000);          // its parent is gone by now
                execv(argv[0], zargv);
                _exit(127);
            }
            _exit(0);
        }
        waitpid(c, NULL, 0);
    } else {
        note("zygote");
    }
    for (;;)
        pause();
}
