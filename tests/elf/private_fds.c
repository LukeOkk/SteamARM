// The runtime's own descriptors stay out of the guest's way. getdents64
// reads a directory through a duplicate of the guest's descriptor; that
// duplicate took the lowest free number (the guest's next open() got the one
// after it, where Linux gives the lowest free) and showed in /proc/self/fd as
// a directory the guest never opened -- Android's zygote aborts on exactly
// that ("Unsupported st_mode for FD 40: DIR").
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void check(int good, const char *what)
{
    printf("  %s  %s\n", good ? "OK " : "MAL", what);
    if (!good)
        fails++;
}

int main(void)
{
    int a = open("/", O_RDONLY | O_DIRECTORY);
    DIR *d = fdopendir(a);
    int entries = 0;
    while (readdir(d)) entries++;
    char what[160];
    snprintf(what, sizeof what, "read / through fd %d (%d entries)", a, entries);
    check(entries > 2, what);

    int f = open("/dev/null", O_RDONLY);
    snprintf(what, sizeof what, "the next open() is the lowest free number: %d after %d", f, a);
    check(f == a + 1, what);

    DIR *p = opendir("/proc/self/fd");
    int pfd = p ? dirfd(p) : -1;
    int dirs = 0, listed = 0;
    struct dirent *e;
    while (p && (e = readdir(p))) {
        if (e->d_name[0] == '.')
            continue;
        int n = atoi(e->d_name);
        listed++;
        struct stat st;
        if (n == pfd || n == a || fstat(n, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        char target[256] = "?";
        char link[64];
        snprintf(link, sizeof link, "/proc/self/fd/%d", n);
        ssize_t k = readlink(link, target, sizeof target - 1);
        if (k > 0) target[k] = 0;
        printf("        a directory the program never opened: fd %d -> %s\n", n, target);
        dirs++;
    }
    snprintf(what, sizeof what, "/proc/self/fd lists %d descriptors, no directory but the program's own", listed);
    check(p && dirs == 0, what);
    if (p) closedir(p);
    closedir(d);
    close(f);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
