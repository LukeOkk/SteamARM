// /proc/<pid>/root of another guest process, as PipeWire's module-access opens
// it to tell Flatpak apps from programs on the host: the directory must open
// (O_DIRECTORY), be the guest's "/" (a file made here is seen through it),
// and .flatpak-info in it must be ENOENT. It was ENOENT itself, and PipeWire
// then took every client for a Flatpak app.
// Usage: proc_pid_root <guest path of a file to make>
// Prints "== proc_pid_root: ok" or a FAIL line.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *marker = argc > 1 ? argv[1] : "/tmp/proc_pid_root.marker";
    int fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("FAIL create %s: %s\n", marker, strerror(errno)); return 1; }
    close(fd);
    int pipefd[2];
    if (pipe(pipefd)) return 1;
    pid_t child = fork();
    if (child == 0) {                     // waits until the parent is done
        char c;
        close(pipefd[1]);
        read(pipefd[0], &c, 1);
        _exit(0);
    }
    close(pipefd[0]);
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/root", (int)child);
    int ok = 1;
    int root = open(path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
    if (root < 0) {
        printf("FAIL open %s: %s\n", path, strerror(errno));
        ok = 0;
    } else {
        int m = openat(root, marker + 1, O_RDONLY | O_CLOEXEC);
        if (m < 0) { printf("FAIL %s%s through it: %s\n", path, marker, strerror(errno)); ok = 0; }
        else close(m);
        int f = openat(root, ".flatpak-info", O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (f >= 0 || errno != ENOENT) { printf("FAIL .flatpak-info: %s\n", f >= 0 ? "exists" : strerror(errno)); ok = 0; }
        close(root);
    }
    close(pipefd[1]);
    waitpid(child, NULL, 0);
    unlink(marker);
    printf("== proc_pid_root: %s\n", ok ? "ok" : "FAIL");
    return !ok;
}
