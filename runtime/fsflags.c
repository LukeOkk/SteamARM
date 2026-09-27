// Open flags and *at() flags are not the same numbers on Linux and Darwin.
//
// Same class of bug as errno (errno_map.c): the low bits agree, so the common
// case works and the divergence only shows up later. O_APPEND is 0x400 on Linux
// and 0x8 on Darwin; O_NONBLOCK is 0x800 and 0x4; O_CLOEXEC is 0x80000 and
// 0x1000000. Passing a Linux O_APPEND straight through asks Darwin for
// O_TRUNC|O_EXCL instead -- it truncates the file it was meant to append to.
//
// Found while getting FEX to start, before it had done any real damage.

#include "lxrt.h"

#include <fcntl.h>

// Linux asm-generic values.
#define L_O_CREAT      0x000040
#define L_O_EXCL       0x000080
#define L_O_NOCTTY     0x000100
#define L_O_TRUNC      0x000200
#define L_O_APPEND     0x000400
#define L_O_NONBLOCK   0x000800
#define L_O_DSYNC      0x001000
#define L_O_DIRECT     0x004000  /* aarch64: O_DIRECTORY is 0x4000 */
#define L_O_DIRECTORY  0x004000
#define L_O_NOFOLLOW   0x008000
#define L_O_SYNC       0x101000
#define L_O_CLOEXEC    0x080000
#define L_O_PATH       0x200000

int lxrt_open_flags_to_darwin(int lflags)
{
    // The access mode occupies the low two bits and agrees on both systems.
    int d = lflags & 3;
    if (lflags & L_O_CREAT)     d |= O_CREAT;
    if (lflags & L_O_EXCL)      d |= O_EXCL;
    if (lflags & L_O_NOCTTY)    d |= O_NOCTTY;
    if (lflags & L_O_TRUNC)     d |= O_TRUNC;
    if (lflags & L_O_APPEND)    d |= O_APPEND;
    if (lflags & L_O_NONBLOCK)  d |= O_NONBLOCK;
    if (lflags & L_O_DIRECTORY) d |= O_DIRECTORY;
    if (lflags & L_O_NOFOLLOW)  d |= O_NOFOLLOW;
    if (lflags & L_O_CLOEXEC)   d |= O_CLOEXEC;
    if ((lflags & L_O_SYNC) == L_O_SYNC) d |= O_SYNC;
    else if (lflags & L_O_DSYNC) d |= O_DSYNC;
    // O_PATH has no Darwin equivalent; O_RDONLY is the closest thing that does
    // not fail, and callers use it to name a file rather than read it.
    // Linux ignores every flag but O_CLOEXEC, O_DIRECTORY and O_NOFOLLOW with
    // O_PATH -- including the access mode. pressure-vessel opens its temporary
    // directories O_PATH|O_RDWR|O_DIRECTORY, and Darwin answers a directory
    // opened for writing with EISDIR.
    if (lflags & L_O_PATH) {
        // O_NONBLOCK: a FIFO opened for reading would otherwise wait for
        // a writer, and an O_PATH open never waits.
        d = O_RDONLY | O_NONBLOCK | (d & (O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        // O_PATH|O_NOFOLLOW on a symlink names the LINK (steam-runtime-tools
        // walks paths that way, then readlinkat(fd, "")); that is O_SYMLINK.
        if (lflags & L_O_NOFOLLOW)
            d = (d & ~O_NOFOLLOW) | O_SYMLINK;
    }
    return d;
}

int lxrt_open_flags_to_linux(int dflags)
{
    int l = dflags & 3;
    if (dflags & O_CREAT)     l |= L_O_CREAT;
    if (dflags & O_EXCL)      l |= L_O_EXCL;
    if (dflags & O_NOCTTY)    l |= L_O_NOCTTY;
    if (dflags & O_TRUNC)     l |= L_O_TRUNC;
    if (dflags & O_APPEND)    l |= L_O_APPEND;
    if (dflags & O_NONBLOCK)  l |= L_O_NONBLOCK;
    if (dflags & O_DIRECTORY) l |= L_O_DIRECTORY;
    if (dflags & O_NOFOLLOW)  l |= L_O_NOFOLLOW;
    if (dflags & O_CLOEXEC)   l |= L_O_CLOEXEC;
    return l;
}

// Linux AT_* against Darwin's.
#define L_AT_SYMLINK_NOFOLLOW 0x100
#define L_AT_REMOVEDIR        0x200
#define L_AT_SYMLINK_FOLLOW   0x400
#define L_AT_EMPTY_PATH       0x1000

int lxrt_at_flags_to_darwin(int lflags)
{
    int d = 0;
    if (lflags & L_AT_SYMLINK_NOFOLLOW) d |= AT_SYMLINK_NOFOLLOW;
    if (lflags & L_AT_SYMLINK_FOLLOW)   d |= AT_SYMLINK_FOLLOW;
    return d;
}

bool lxrt_at_is_removedir(int lflags) { return (lflags & L_AT_REMOVEDIR) != 0; }
bool lxrt_at_is_empty_path(int lflags) { return (lflags & L_AT_EMPTY_PATH) != 0; }

// Linux AT_FDCWD is -100, Darwin's is -2.
int lxrt_dirfd_to_darwin(int dirfd) { return dirfd == -100 ? AT_FDCWD : dirfd; }

// ---------------------------------------------------------------- file locks
//
// The fourth number space that differs, after errno, open flags and sockaddr.
// fcntl's lock commands, the flock struct's field ORDER, and the lock type
// constants are all different:
//
//   command   Linux  Darwin      l_type    Linux  Darwin
//   F_GETLK       5       7      F_RDLCK       0       1
//   F_SETLK       6       8      F_WRLCK       1       3
//   F_SETLKW      7       9      F_UNLCK       2       2
//
// Linux struct flock is {type, whence, start, len, pid}; Darwin's is
// {start, len, pid, type, whence}. Nothing about a pass-through survives.
//
// Found because FEXServer takes a lock to enforce a single instance, got an
// unrelated errno, and exited without a message.

#define L_F_GETLK   5
#define L_F_SETLK   6
#define L_F_SETLKW  7
#define L_F_OFD_GETLK  36
#define L_F_OFD_SETLK  37
#define L_F_OFD_SETLKW 38

struct linux_flock {
    int16_t l_type;
    int16_t l_whence;
    int32_t pad_;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
    int32_t pad2_;
};

static int16_t locktype_to_darwin(int16_t l)
{
    switch (l) {
    case 0: return F_RDLCK;
    case 1: return F_WRLCK;
    default: return F_UNLCK;
    }
}

static int16_t locktype_to_linux(int16_t d)
{
    if (d == F_RDLCK) return 0;
    if (d == F_WRLCK) return 1;
    return 2;
}

// Returns the Darwin command, or -1 when it has no equivalent.
int lxrt_fcntl_lock_cmd(int lcmd, bool *is_lock)
{
    *is_lock = true;
    switch (lcmd) {
    // Open-file-description locks are per-fd rather than per-process. Darwin
    // has no equivalent, and mapping them onto process locks is wrong in a way
    // that only shows up with threads. It is still much closer than failing,
    // and every caller so far uses them for single-instance guards.
    case L_F_GETLK: case L_F_OFD_GETLK:   return F_GETLK;
    case L_F_SETLK: case L_F_OFD_SETLK:   return F_SETLK;
    case L_F_SETLKW: case L_F_OFD_SETLKW: return F_SETLKW;
    default: *is_lock = false; return -1;
    }
}

void lxrt_flock_to_darwin(const void *lin, struct flock *out)
{
    const struct linux_flock *l = lin;
    out->l_start  = l->l_start;
    out->l_len    = l->l_len;
    out->l_pid    = l->l_pid;
    out->l_type   = locktype_to_darwin(l->l_type);
    out->l_whence = l->l_whence;
}

void lxrt_flock_to_linux(const struct flock *d, void *lout)
{
    struct linux_flock *l = lout;
    l->l_type   = locktype_to_linux(d->l_type);
    l->l_whence = d->l_whence;
    l->pad_     = 0;
    l->l_start  = d->l_start;
    l->l_len    = d->l_len;
    l->l_pid    = d->l_pid;
    l->pad2_    = 0;
}
