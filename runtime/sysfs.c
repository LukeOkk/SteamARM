// A minimal /sys/devices/system/cpu inside the guest root.
//
// Linux programs count CPUs from sysfs, not from sched_getaffinity: FEX's
// CalculateNumberOfCPUs() stops at the first missing
// /sys/devices/system/cpu/cpu<N>, and its emulated /proc/cpuinfo and
// /sys/devices/system/cpu/online then describe ONE processor (MEASURED:
// "grep -c processor /proc/cpuinfo" = 1 on a 10-core M4). glibc's
// get_nprocs_conf and many engines read the same files.
//
// The tree is written into LXRT_ROOT once, and rewritten only when the CPU
// count differs -- nothing is touched on an ordinary start (a runtime that
// rewrote files on every exec kept fseventsd busy, see stage 13).
#include "lxrt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

static void mkdirs(const char *path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = '\0';
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

static void put(const char *path, const char *text)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    ssize_t w = write(fd, text, strlen(text));
    close(fd);
    if (w == (ssize_t)strlen(text))
        rename(tmp, path);
    else
        unlink(tmp);
}

void lxrt_sysfs_init(void)
{
    const char *root = getenv("LXRT_ROOT");
    if (!root || !*root)
        return;                     // host "/" is the guest's: nothing to add
    int n = 0;
    size_t sz = sizeof n;
    if (sysctlbyname("hw.logicalcpu", &n, &sz, NULL, 0) != 0 || n < 1)
        n = 1;
    char want[32], dir[900], path[1000], have[32] = "";
    snprintf(want, sizeof want, n > 1 ? "0-%d\n" : "0\n", n - 1);
    snprintf(dir, sizeof dir, "%s/sys/devices/system/cpu", root);
    snprintf(path, sizeof path, "%s/online", dir);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, have, sizeof have - 1);
        have[r > 0 ? r : 0] = '\0';
        close(fd);
        snprintf(path, sizeof path, "%s/cpu0/regs/identification/midr_el1", dir);
        if (strcmp(have, want) == 0 && access(path, F_OK) == 0)
            return;                 // already right: the usual case
    }
    mkdirs(dir);
    // cpu<N>/regs/identification/midr_el1, as Linux's arm64 kernel exports
    // it: Wine ARM64 reads it for HARDWARE\DESCRIPTION\System\
    // CentralProcessor\<N> "CP 4000", which FEX's ARM64EC JIT reads back
    // (the synthetic MIDR of sysreg.c, the one `mrs Xt, MIDR_EL1` returns).
    char midr[32];
    snprintf(midr, sizeof midr, "0x%016llx\n", (unsigned long long)lxrt_synthetic_sysreg(0xD5380000u));
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/cpu%d", dir, i);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/cpu%d/regs", dir, i);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/cpu%d/regs/identification", dir, i);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/cpu%d/regs/identification/midr_el1", dir, i);
        put(path, midr);
    }
    // Directories of CPUs a previous (larger) machine had: gone.
    for (int i = n; i < 1024; i++) {
        snprintf(path, sizeof path, "%s/cpu%d", dir, i);
        if (rmdir(path) != 0 && errno == ENOENT)
            break;
    }
    snprintf(path, sizeof path, "%s/present", dir);
    put(path, want);
    snprintf(path, sizeof path, "%s/possible", dir);
    put(path, want);
    snprintf(path, sizeof path, "%s/online", dir);  // last: it marks the tree complete
    put(path, want);
}
