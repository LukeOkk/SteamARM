// getdents64.
//
// Darwin has no getdents; it has readdir on a DIR*. The Linux caller has a file
// descriptor and expects packed struct linux_dirent64 records, so a DIR* is
// opened over a dup of the fd on first use and kept for that fd's lifetime.
//
// Needed because C++'s std::filesystem::directory_iterator uses it, which is
// how FEXServer enumerates /proc/self/fd.

#include "lxrt.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

// Linux d_type values; Darwin's DT_* happen to use the same numbers, but the
// mapping is written out rather than assumed.
static uint8_t dtype_to_linux(uint8_t d)
{
    switch (d) {
    case DT_FIFO: return 1;
    case DT_CHR:  return 2;
    case DT_DIR:  return 4;
    case DT_BLK:  return 6;
    case DT_REG:  return 8;
    case DT_LNK:  return 10;
    case DT_SOCK: return 12;
    default:      return 0;   // DT_UNKNOWN
    }
}

#define MAX_DIRS 128
static struct { int fd; DIR *dir; } g_dirs[MAX_DIRS];
static pthread_mutex_t g_dirs_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(dirents_g_dirs_lock, g_dirs_lock)

static DIR *dir_for(int fd)
{
    pthread_mutex_lock(&g_dirs_lock);
    for (int i = 0; i < MAX_DIRS; i++)
        if (g_dirs[i].dir && g_dirs[i].fd == fd) {
            DIR *d = g_dirs[i].dir;
            pthread_mutex_unlock(&g_dirs_lock);
            return d;
        }
    // fdopendir takes ownership of what it is given, and the guest still owns
    // its fd, so hand it a duplicate.
    int dup_fd = dup(fd);
    DIR *d = dup_fd < 0 ? NULL : fdopendir(dup_fd);
    if (!d) {
        if (dup_fd >= 0)
            close(dup_fd);
        pthread_mutex_unlock(&g_dirs_lock);
        return NULL;
    }
    for (int i = 0; i < MAX_DIRS; i++)
        if (!g_dirs[i].dir) {
            g_dirs[i].fd = fd;
            g_dirs[i].dir = d;
            pthread_mutex_unlock(&g_dirs_lock);
            return d;
        }
    closedir(d);
    pthread_mutex_unlock(&g_dirs_lock);
    return NULL;
}

void lxrt_dirents_close(int fd)
{
    pthread_mutex_lock(&g_dirs_lock);
    for (int i = 0; i < MAX_DIRS; i++)
        if (g_dirs[i].dir && g_dirs[i].fd == fd) {
            closedir(g_dirs[i].dir);
            g_dirs[i].dir = NULL;
            break;
        }
    pthread_mutex_unlock(&g_dirs_lock);
}

long lxrt_getdents64(int fd, void *buf, unsigned count)
{
    if (!buf)
        return LERR(EFAULT);
    DIR *d = dir_for(fd);
    if (!d)
        return LERR(ENOTDIR);

    unsigned used = 0;
    for (;;) {
        long pos = telldir(d);
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e)
            return errno ? LERR(errno) : (long)used;   // 0 at end of directory

        size_t namelen = strlen(e->d_name);
        size_t reclen = (sizeof(struct linux_dirent64) + namelen + 1 + 7) & ~7ul;
        if (used + reclen > count) {
            // Does not fit: rewind so the next call returns it, and report what
            // did fit. An empty first record means the buffer is too small.
            seekdir(d, pos);
            return used ? (long)used : LERR(EINVAL);
        }

        struct linux_dirent64 *out = (struct linux_dirent64 *)((char *)buf + used);
        out->d_ino = e->d_ino;
        out->d_off = (int64_t)telldir(d);
        out->d_reclen = (uint16_t)reclen;
        out->d_type = dtype_to_linux(e->d_type);
        memcpy(out->d_name, e->d_name, namelen + 1);
        used += (unsigned)reclen;
    }
}
