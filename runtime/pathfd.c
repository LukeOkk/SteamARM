// O_PATH descriptors for files Darwin will not open.
//
// Linux opens anything with O_PATH -- the descriptor only names the file.
// Darwin's open(2) of a Unix socket fails with EOPNOTSUPP whatever the flags,
// so an O_PATH open of one failed here. pressure-vessel checks every path it
// shares that way (_srt_sysroot_test: openat O_PATH per component), and so it
// silently dropped the --ro-bind of the PulseAudio socket while still pointing
// PULSE_SERVER at /run/pressure-vessel/pulse/native: every game in Steam's
// container lost its sound (MEASURED: connect ENOENT there, /tmp/pulse/native
// fine).
//
// The stand-in is a descriptor of /dev/null, remembered with the host path it
// names. fstat, fstatat(AT_EMPTY_PATH), statx(AT_EMPTY_PATH) and
// /proc/self/fd/N answer for the path. A stale entry (the number closed some
// other way and reused) is caught by checking the descriptor still is that
// /dev/null open.
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "lxrt.h"

#define PF_MAX 65536

struct pathfd {
    char *path;
    dev_t dev;
    ino_t ino;
};
static _Atomic(struct pathfd *) g_pf[PF_MAX];

int lxrt_pathfd_open(int ddirfd, const char *hpath, int cloexec)
{
    struct stat st;
    if (fstatat(ddirfd, hpath, &st, 0) != 0)
        return -1;
    if (!S_ISSOCK(st.st_mode)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    char full[PATH_MAX];
    if (hpath[0] == '/') {
        snprintf(full, sizeof full, "%s", hpath);
    } else {
        char base[MAXPATHLEN];
        if (ddirfd == AT_FDCWD ? !getcwd(base, sizeof base) : fcntl(ddirfd, F_GETPATH, base) != 0)
            return -1;
        snprintf(full, sizeof full, "%s/%s", base, hpath);
    }
    int fd = open("/dev/null", O_RDONLY | (cloexec ? O_CLOEXEC : 0));
    if (fd < 0)
        return -1;
    if (fd >= PF_MAX || fstat(fd, &st) != 0) {
        close(fd);
        errno = EMFILE;
        return -1;
    }
    struct pathfd *e = malloc(sizeof *e);
    if (!e) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    e->path = strdup(full);
    e->dev = st.st_dev;
    e->ino = st.st_ino;
    struct pathfd *old = atomic_exchange(&g_pf[fd], e);
    if (old) {
        free(old->path);
        free(old);
    }
    return fd;
}

const char *lxrt_pathfd_path(int fd)
{
    if (fd < 0 || fd >= PF_MAX)
        return NULL;
    struct pathfd *e = atomic_load(&g_pf[fd]);
    if (!e)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_dev != e->dev || st.st_ino != e->ino)
        return NULL;
    return e->path;
}

void lxrt_pathfd_close(int fd)
{
    if (fd < 0 || fd >= PF_MAX)
        return;
    struct pathfd *old = atomic_exchange(&g_pf[fd], NULL);
    if (old) {
        free(old->path);
        free(old);
    }
}
