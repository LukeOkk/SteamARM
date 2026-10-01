// unlink of a directory is EISDIR on Linux (Darwin says EPERM), and remove()
// -- bionic's, glibc's, and so Java's File.delete() -- relies on it to fall
// back to rmdir. A file removed with rmdir is ENOTDIR.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "OK " : "MAL", what);
    fails += !ok;
}

int main(void)
{
    char d[64], f[80];
    snprintf(d, sizeof d, "/tmp/lxrt-unlink-dir-%d", getpid());
    snprintf(f, sizeof f, "%s.file", d);
    mkdir(d, 0755);
    errno = 0;
    check(unlink(d) == -1 && errno == EISDIR, "unlink of a directory: EISDIR");
    errno = 0;
    check(unlinkat(AT_FDCWD, d, 0) == -1 && errno == EISDIR, "unlinkat(.., 0) of a directory: EISDIR");
    check(remove(d) == 0 && access(d, F_OK) != 0, "remove() of an empty directory removes it");
    close(open(f, O_CREAT | O_WRONLY, 0644));
    errno = 0;
    check(rmdir(f) == -1 && errno == ENOTDIR, "rmdir of a file: ENOTDIR");
    check(remove(f) == 0 && access(f, F_OK) != 0, "remove() of a file removes it");
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
