// /proc/self/exe as Linux gives it: absolute, the same through readlink and
// realpath, and openable -- whatever path the program was started by.
// argv[1]: the path readlink must return.
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int is_elf(const char *path)
{
    unsigned char m[4] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    int ok = read(fd, m, 4) == 4 && memcmp(m, "\177ELF", 4) == 0;
    close(fd);
    return ok;
}

int main(int argc, char **argv)
{
    char link[PATH_MAX], real[PATH_MAX];
    int fails = 0;
    ssize_t n = readlink("/proc/self/exe", link, sizeof link - 1);
    if (n <= 0) {
        printf("FAIL readlink(/proc/self/exe)\n");
        return 1;
    }
    link[n] = '\0';
    printf("exe %s\n", link);
    if (link[0] != '/') {
        printf("FAIL not absolute: %s\n", link);
        fails++;
    }
    if (argc > 1 && strcmp(link, argv[1]) != 0) {
        printf("FAIL expected %s\n", argv[1]);
        fails++;
    }
    if (!realpath("/proc/self/exe", real) || strcmp(real, link) != 0) {
        printf("FAIL realpath(/proc/self/exe) = %s\n", real);
        fails++;
    }
    if (!is_elf("/proc/self/exe")) {
        printf("FAIL open(/proc/self/exe)\n");
        fails++;
    }
    if (!is_elf(link)) {
        printf("FAIL open(%s)\n", link);
        fails++;
    }
    if (fails)
        return 1;
    printf("== proc self exe: ok\n");
    return 0;
}
