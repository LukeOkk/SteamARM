// An i386 program linked against the x86_64 image's own 32-bit bionic
// (/apex/com.android.runtime/lib/bionic/libc.so, /system/bin/linker), run by
// FEX's 32-bit mode under lxrun. Each check is one thing i386 Android
// (zygote_secondary, dex2oat32, dalvikvm32, the 32-bit HALs) died of before
// stage 28 (benchmarks/stage28-android-reliability.txt):
//
//   start     the program runs at all: bionic makes every syscall through
//             AT_SYSINFO, FEX's fallback vsyscall page, which FEX did not
//             record as executable ("NoExec instruction in entry block")
//   requeue   FUTEX_CMP_REQUEUE with nr_requeue INT_MAX: the count slot was
//             given the guest base like a pointer and arrived negative (EINVAL,
//             ART: "futex requeue failed")
//   sigwait   rt_sigtimedwait with a NULL siginfo (sigwait): FEX asserted
//   rcvtimeo  SO_RCVTIMEO set and read back: FEX dereferenced the guest's
//             address without the base, then lxrun did not know
//             SO_RCVTIMEO_NEW (the zygote, on every connection)
//   domain    getsockopt SO_DOMAIN on an AF_UNIX socket (the zygote's accept)
//   hint      an mmap hint below 4 GiB is honoured when free (ART's boot image)
//   msync     msync(MS_SYNC) of a file mapping (dex2oat32's vdex)
//   rights    recvmsg of a message with SCM_RIGHTS, and of one without, with
//             the flags libbase's ReceiveFileDescriptorVector passes
//             (MSG_TRUNC | MSG_CTRUNC | MSG_CMSG_CLOEXEC | MSG_NOSIGNAL): the
//             fd arrives, and neither truncation flag comes back (XNU echoed
//             them: "message was truncated when receiving file descriptors")
// Last line: "== i386_bionic: N ok, M mal".
//
//   clang --target=i686-linux-android30 -O2 -fPIE -pie -nostdlib
//         -Wl,--dynamic-linker=/system/bin/linker i386_bionic.c <root>/apex/com.android.runtime/lib/bionic/libc.so
#include <stddef.h>
#include <stdint.h>

typedef int ssize_t;
typedef long off_t;
typedef uint32_t socklen_t;
struct iovec { void *iov_base; size_t iov_len; };
struct msghdr { void *msg_name; socklen_t msg_namelen; struct iovec *msg_iov; size_t msg_iovlen;
                void *msg_control; size_t msg_controllen; int msg_flags; };
struct cmsghdr { size_t cmsg_len; int cmsg_level; int cmsg_type; };
struct timeval { long tv_sec; long tv_usec; };
typedef struct { unsigned long sig[2]; } sigset64_t;

long syscall(long nr, ...);
int printf(const char *fmt, ...);
int socketpair(int d, int t, int p, int sv[2]);
int setsockopt(int fd, int level, int opt, const void *v, socklen_t len);
int getsockopt(int fd, int level, int opt, void *v, socklen_t *len);
ssize_t sendmsg(int fd, const struct msghdr *m, int flags);
ssize_t recvmsg(int fd, struct msghdr *m, int flags);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
int msync(void *addr, size_t len, int flags);
int open(const char *path, int flags, ...);
int close(int fd);
int unlink(const char *path);
int ftruncate(int fd, off_t len);
ssize_t read(int fd, void *buf, size_t n);
int fcntl(int fd, int cmd, ...);
int getpid(void);
int gettid(void);
int *__errno(void);
#define errno (*__errno())

enum { NR_futex = 240, NR_rt_sigprocmask = 175, NR_rt_sigtimedwait = 177, NR_tgkill = 270 };
#define SOL_SOCKET 1
#define SO_RCVTIMEO 20
#define SO_DOMAIN 39
#define SCM_RIGHTS 1
#define AF_UNIX 1
#define SOCK_STREAM 1
#define MSG_CTRUNC 8
#define MSG_TRUNC 0x20
#define MSG_RECV_FLAGS (MSG_TRUNC | MSG_CTRUNC | 0x40000000 | 0x4000)
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MS_SYNC 4
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define F_GETFD 1
#define SIGUSR1 10

static int oks, mals;
static void verdict(const char *name, int ok, const char *why)
{
    printf("  %s %s: %s\n", ok ? "ok  " : "MAL ", name, why);
    if (ok) oks++; else mals++;
}

int main(void)
{
    verdict("start", 1, "running (every syscall so far went through AT_SYSINFO)");

    // requeue: nobody waits, so 0 waiters moved; the point is the count.
    static int word = 7, word2;
    long r = syscall(NR_futex, &word, 4 /* FUTEX_CMP_REQUEUE */ | 128, 1, (void *)0x7fffffff, &word2, 7);
    printf("       futex CMP_REQUEUE -> %ld (errno %d)\n", r, r < 0 ? errno : 0);
    verdict("requeue", r >= 0, r >= 0 ? "CMP_REQUEUE with nr_requeue INT_MAX accepted" : "refused");

    // sigwait with NULL info.
    sigset64_t set = { { 1u << (SIGUSR1 - 1), 0 } };
    syscall(NR_rt_sigprocmask, 0 /* SIG_BLOCK */, &set, 0, 8);
    syscall(NR_tgkill, getpid(), gettid(), SIGUSR1);
    r = syscall(NR_rt_sigtimedwait, &set, 0, 0, 8);
    verdict("sigwait", r == SIGUSR1, r == SIGUSR1 ? "rt_sigtimedwait(set, NULL, NULL) returned SIGUSR1" : "wrong result");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        verdict("socketpair", 0, "failed");
    } else {
        struct timeval tv = { 1, 500000 }, got = { 0, 0 };
        socklen_t gl = sizeof got;
        int s1 = setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        int s2 = getsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &got, &gl);
        printf("       SO_RCVTIMEO set %d get %d: %ld.%06ld\n", s1, s2, got.tv_sec, got.tv_usec);
        verdict("rcvtimeo", s1 == 0 && s2 == 0 && got.tv_sec == 1 && got.tv_usec == 500000,
                "set to 1.5 s and read back");

        int dom = -1;
        socklen_t dl = sizeof dom;
        int s3 = getsockopt(sv[0], SOL_SOCKET, SO_DOMAIN, &dom, &dl);
        verdict("domain", s3 == 0 && dom == AF_UNIX, s3 == 0 ? "SO_DOMAIN is AF_UNIX" : "refused");

        // rights: one message with an fd, then one without.
        char byte = 'x', in = 0;
        struct iovec iov = { &byte, 1 };
        union { struct cmsghdr h; char b[64]; } ctl;
        struct msghdr m = { 0, 0, &iov, 1, ctl.b, 16, 0 };   // CMSG_SPACE(sizeof(int)) on i386
        ctl.h.cmsg_len = 16;                                  // CMSG_LEN(sizeof(int))
        ctl.h.cmsg_level = SOL_SOCKET;
        ctl.h.cmsg_type = SCM_RIGHTS;
        *(int *)(ctl.b + 12) = sv[0];
        ssize_t w = sendmsg(sv[1], &m, 0);
        union { struct cmsghdr h; char b[64]; } rc;
        struct iovec riov = { &in, 1 };
        struct msghdr rm = { 0, 0, &riov, 1, rc.b, sizeof rc.b, 0 };
        ssize_t n = recvmsg(sv[0], &rm, MSG_RECV_FLAGS);
        int fd = rm.msg_controllen >= 16 && rc.h.cmsg_type == SCM_RIGHTS ? *(int *)(rc.b + 12) : -1;
        printf("       with SCM_RIGHTS: sent %d, got %d byte(s), controllen %u, flags 0x%x, fd %d\n",
               (int)w, (int)n, (unsigned)rm.msg_controllen, rm.msg_flags, fd);
        int ok1 = n == 1 && in == 'x' && fd >= 0 && fcntl(fd, F_GETFD) >= 0 && !(rm.msg_flags & (MSG_CTRUNC | MSG_TRUNC));
        struct msghdr m2 = { 0, 0, &iov, 1, 0, 0, 0 };
        byte = 'y';
        w = sendmsg(sv[1], &m2, 0);
        struct msghdr rm2 = { 0, 0, &riov, 1, rc.b, sizeof rc.b, 0 };
        n = recvmsg(sv[0], &rm2, MSG_RECV_FLAGS);
        printf("       without: sent %d, got %d byte(s), controllen %u, flags 0x%x\n",
               (int)w, (int)n, (unsigned)rm2.msg_controllen, rm2.msg_flags);
        int ok2 = n == 1 && in == 'y' && !(rm2.msg_flags & (MSG_CTRUNC | MSG_TRUNC));
        verdict("rights", ok1 && ok2, "an fd over SCM_RIGHTS; no truncation reported, with and without one");
        if (fd >= 0) close(fd);
        close(sv[0]);
        close(sv[1]);
    }

    void *want = (void *)0x30000000;
    void *p = mmap(want, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("       mmap hint %p -> %p\n", want, p);
    verdict("hint", p == want, p == want ? "placed at the free hint" : "placed elsewhere");
    if (p != (void *)-1) munmap(p, 0x10000);

    const char *path = "/data/local/tmp/i386_bionic.msync";
    int f = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int ms = -1, e = 0;
    if (f >= 0 && ftruncate(f, 4096) == 0) {
        char *q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
        if (q != (void *)-1) {
            q[0] = 'm';
            ms = msync(q, 4096, MS_SYNC);
            e = ms ? errno : 0;
            munmap(q, 4096);
        }
    }
    char c = 0;
    int f2 = open(path, O_RDWR);
    if (f2 >= 0) { read(f2, &c, 1); close(f2); }
    if (f >= 0) close(f);
    unlink(path);
    printf("       msync -> %d (errno %d), file byte '%c'\n", ms, e, c ? c : '?');
    verdict("msync", ms == 0 && c == 'm', "MS_SYNC of a shared file mapping");

    printf("== i386_bionic: %d ok, %d mal\n", oks, mals);
    return mals ? 1 : 0;
}

// crtbegin's part, as tests/android/bionic_min.h does it for x86_64/aarch64.
typedef struct { void (**preinit_array)(void); void (**init_array)(void); void (**fini_array)(void); } structors_array_t;
__attribute__((noreturn)) void __libc_init(void *raw_args, void (*onexit)(void),
                                           int (*slingshot)(int, char **, char **),
                                           structors_array_t const *const structors);
static void (*fini[2])(void) = { (void (*)(void))-1, 0 };
static int slingshot(int argc, char **argv, char **envp) { (void)argc; (void)argv; (void)envp; return main(); }
__attribute__((used, regparm(0))) static void _start_main(void *raw_args)
{
    structors_array_t array = { 0, 0, fini };
    __libc_init(raw_args, 0, slingshot, &array);
}
__asm__(".globl _start\n_start:\n  mov %esp, %eax\n  and $-16, %esp\n  sub $12, %esp\n  push %eax\n  call _start_main\n  hlt\n");
