// A bwrap plan, interpreted. As the parent, exec /usr/bin/bwrap with a plan
// that uses every option pressure-vessel's real plan uses (benchmarks/
// stage6-bwrap-plan.txt) and ourselves as the command; as the child, check
// the view. On Linux the real bwrap runs the plan; under the runtime,
// runtime/mounts.c does. Same binary, same checks.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int ok, bad;
#define CHECK(c, ...) do { if (c) { ok++; printf("  OK   " __VA_ARGS__); } else { bad++; printf("  MAL  " __VA_ARGS__); } printf("\n"); fflush(stdout); } while (0)

static int child(void)
{
    char buf[256];
    int fd = open("/opt/x/marker.txt", O_RDONLY);
    ssize_t r = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (r > 0) buf[r] = 0;
    CHECK(fd >= 0 && r > 0 && strcmp(buf, "bound-source\n") == 0, "ro-bind: /opt/x/marker.txt reads the source (%s)", fd < 0 ? strerror(errno) : "ok");
    if (fd >= 0) close(fd);
    fd = open("/opt/x/new.txt", O_WRONLY | O_CREAT, 0644);
    CHECK(fd < 0, "ro-bind: writing into /opt/x is refused (%s)", fd < 0 ? strerror(errno) : "ALLOWED");
    if (fd >= 0) { close(fd); unlink("/opt/x/new.txt"); }
    fd = open("/data/w.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "rw\n", 3) == 3, "bind: /data is writable (%s)", fd < 0 ? strerror(errno) : "ok");
    if (fd >= 0) close(fd);
    ssize_t l = readlink("/lib", buf, sizeof buf - 1);
    if (l > 0) buf[l] = 0;
    CHECK(l > 0 && strcmp(buf, "usr/lib") == 0, "symlink: /lib -> %s", l > 0 ? buf : strerror(errno));
    struct stat st;
    CHECK(stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode), "tmpfs: /tmp is a directory");
    fd = open("/tmp/t.txt", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0, "tmpfs: /tmp is writable (%s)", fd < 0 ? strerror(errno) : "ok");
    if (fd >= 0) close(fd);
    fd = open("/etc/x", O_RDONLY);
    r = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (r > 0) buf[r] = 0;
    CHECK(fd >= 0 && r > 0 && strcmp(buf, "data-from-fd\n") == 0, "ro-bind-data: /etc/x holds the fd's bytes (%s)", fd < 0 ? strerror(errno) : "ok");
    if (fd >= 0) close(fd);
    const char *a = getenv("BWRAP_TEST_A");
    CHECK(a && strcmp(a, "B") == 0, "setenv: BWRAP_TEST_A=%s", a ? a : "(unset)");
    CHECK(getenv("BWRAP_TEST_GONE") == NULL, "unsetenv: BWRAP_TEST_GONE is gone");
    char cwd[256];
    CHECK(getcwd(cwd, sizeof cwd) && strcmp(cwd, "/data") == 0, "chdir: cwd is %s", cwd);
    CHECK(stat("/proc/self", &st) == 0, "proc: /proc/self exists");
    printf("== %d ok, %d mal\n", ok, bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child") == 0)
        return child();
    // Parent: prepare sources.
    char src[] = "/tmp/bwrap-src-XXXXXX", data[] = "/tmp/bwrap-data-XXXXXX";
    if (!mkdtemp(src) || !mkdtemp(data)) { perror("mkdtemp"); return 2; }
    char path[512]; snprintf(path, sizeof path, "%s/marker.txt", src);
    FILE *f = fopen(path, "w"); fputs("bound-source\n", f); fclose(f);
    int p[2]; if (pipe(p) != 0) { perror("pipe"); return 2; }
    write(p[1], "data-from-fd\n", 13); close(p[1]);
    char fdstr[16]; snprintf(fdstr, sizeof fdstr, "%d", p[0]);
    char self[512]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) { perror("readlink"); return 2; }
    self[n] = 0;
    // The command is this binary; it must be reachable inside the sandbox, so
    // bind its directory too.
    char selfdir[512]; snprintf(selfdir, sizeof selfdir, "%s", self); *strrchr(selfdir, '/') = 0;
    setenv("BWRAP_TEST_GONE", "1", 1);
    char *args[] = {
        "/usr/bin/bwrap",
        "--dir", "/",
        "--ro-bind", "/usr", "/usr",
        "--symlink", "usr/lib", "/lib",
        "--symlink", "usr/bin", "/bin",
        "--ro-bind", src, "/opt/x",
        "--bind", data, "/data",
        "--ro-bind", selfdir, "/self",
        "--tmpfs", "/tmp",
        "--proc", "/proc",
        "--dev", "/dev",
        "--ro-bind-data", fdstr, "/etc/x",
        "--setenv", "BWRAP_TEST_A", "B",
        "--unsetenv", "BWRAP_TEST_GONE",
        "--chdir", "/data",
        "--new-session",
        "--", NULL, "child", NULL };
    char cmd[600]; snprintf(cmd, sizeof cmd, "/self/%s", strrchr(self, '/') + 1);
    args[sizeof args / sizeof args[0] - 3] = cmd;
    pid_t pid = fork();
    if (pid == 0) { execv(args[0], args); perror("execv bwrap"); _exit(127); }
    int status = 0; waitpid(pid, &status, 0);
    printf("bwrap child: exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
