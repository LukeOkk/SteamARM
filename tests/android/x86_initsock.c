// An init-style socket, seen from an x86-64 guest under FEX, as Android's
// code checks it (tests/android/run.sh; benchmarks/stage27-android-
// framework.txt). Freestanding and static: raw syscalls only.
//
//   x86_initsock <fd> <guest path>
//
// The socket was bound on the host at <root><guest path> by the boot script
// (scripts/android-boot.py) and inherited as <fd>, the way Android's init
// hands a service ANDROID_SOCKET_<name>. Checks:
//   name      getsockname() returns the GUEST path, and addrlen is Linux's
//             (offsetof(sun_path) + strlen + 1): libcutils'
//             android_get_control_socket() and the zygote's descriptor
//             whitelist (fd_utils.cpp GetSocketName) both compare the name
//             byte for byte and cut exactly one trailing NUL
//   listen    listen() on it works (the zygote's LocalServerSocket does it)
//   fdlist    /proc/self/fd lists <fd> and an eventfd this program made, and
//             no descriptor it did not get or open itself: no pipe (the
//             runtime's eventfd backing), directory or unnamed socket of
//             the runtime or of FEX (the zygote aborts on those)
//   attr      /proc/self/attr/current and /proc/thread-self/attr/current
//             read a context (keystore aborts when libselinux's getcon
//             cannot read one)
typedef unsigned long u64;
typedef long i64;

static i64 sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
    i64 r;
    register i64 r10 __asm__("r10") = d;
    register i64 r8 __asm__("r8") = e;
    register i64 r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
#define sys3(n, a, b, c) sys6(n, a, b, c, 0, 0, 0)

enum { NR_read = 0, NR_write = 1, NR_open = 2, NR_close = 3, NR_fstat = 5, NR_getsockname = 51, NR_listen = 50,
       NR_getdents64 = 217, NR_readlink = 89, NR_exit_group = 231, NR_eventfd2 = 290 };

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sys3(NR_write, 1, (i64)s, (i64)slen(s)); }
static void num(i64 v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    int neg = v < 0;
    u64 u = neg ? (u64)-v : (u64)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    out(b + i);
}
static int streq(const char *a, const char *b, u64 n)
{
    for (u64 i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int oks, mals;
static void verdict(const char *name, int ok)
{
    out(ok ? "  ok   " : "  MAL  ");
    out(name);
    out("\n");
    if (ok) oks++; else mals++;
}

static int attr_read(const char *path)
{
    char ab[128];
    i64 af = sys3(NR_open, (i64)path, 0, 0);
    i64 an = af >= 0 ? sys3(NR_read, af, (i64)ab, sizeof ab - 1) : -1;
    if (af >= 0) sys3(NR_close, af, 0, 0);
    ab[an > 0 ? an : 0] = 0;
    out("  ");
    out(path);
    out(": \"");
    out(ab);
    out("\"\n");
    return an > 4 && ab[0] == 'u' && ab[1] == ':';
}

static int main_(int argc, char **argv)
{
    if (argc < 3) { out("usage: x86_initsock <fd> <guest path>\n"); return 2; }
    i64 fd = 0;
    for (const char *p = argv[1]; *p; p++) fd = fd * 10 + (*p - '0');
    const char *want = argv[2];
    u64 wl = slen(want);

    struct { unsigned short fam; char path[126]; } sa;
    for (u64 i = 0; i < sizeof sa; i++) ((char *)&sa)[i] = 0x55;   // poison: a short write shows
    unsigned int len = sizeof sa;
    i64 r = sys3(NR_getsockname, fd, (i64)&sa, (i64)&len);
    out("  getsockname: rc ");
    num(r);
    out(", addrlen ");
    num(len);
    out(" (want ");
    num((i64)(2 + wl + 1));
    out("), path \"");
    if (r == 0 && len > 2 && len <= sizeof sa) sys3(NR_write, 1, (i64)sa.path, (i64)(len - 2));
    out("\"\n");
    verdict("name: the guest path, Linux's addrlen, NUL-terminated",
            r == 0 && sa.fam == 1 && len == 2 + wl + 1 && streq(sa.path, want, wl) && sa.path[wl] == 0);

    verdict("listen", sys3(NR_listen, fd, 16, 0) == 0);

    // /proc/self/fd: only 0-2, <fd>, the eventfd and the directory being read.
    i64 efd = sys3(NR_eventfd2, 0, 0, 0);
    i64 d = sys3(NR_open, (i64)"/proc/self/fd", 0x10000 /* O_DIRECTORY */, 0);
    char db[4096];
    int seen_fd = 0, strangers = 0;
    for (;;) {
        i64 n = sys3(NR_getdents64, d, (i64)db, sizeof db);
        if (n <= 0) break;
        for (i64 off = 0; off < n;) {
            unsigned short rl = *(unsigned short *)(db + off + 16);
            const char *nm = db + off + 19;
            off += rl;
            if (nm[0] == '.') continue;
            i64 v = 0;
            for (const char *p = nm; *p; p++) v = v * 10 + (*p - '0');
            if (v == fd) { seen_fd = 1; continue; }
            if (v <= 2 || v == d || v == efd) continue;
            char link[64] = "/proc/self/fd/", tgt[256];
            u64 k = 14;
            for (const char *p = nm; *p; p++) link[k++] = *p;
            link[k] = 0;
            i64 tl = sys3(NR_readlink, (i64)link, (i64)tgt, sizeof tgt - 1);
            tgt[tl > 0 ? tl : 0] = 0;
            out("  stranger fd ");
            out(nm);
            out(" -> ");
            out(tgt);
            out("\n");
            strangers++;
        }
    }
    sys3(NR_close, d, 0, 0);
    verdict("fdlist: the socket and the eventfd, and no runtime or FEX descriptor", efd > 2 && seen_fd && !strangers);

    // (No arrays of pointers: a freestanding static-pie is not relocated.)
    int attr_ok = attr_read("/proc/self/attr/current") & attr_read("/proc/thread-self/attr/current");
    verdict("attr: a context in /proc/self/attr/current and /proc/thread-self/attr/current", attr_ok);

    out("== x86_initsock: ");
    num(oks);
    out(" ok, ");
    num(mals);
    out(" mal\n");
    return mals ? 1 : 0;
}

__attribute__((used)) static void start_c(u64 *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    sys3(NR_exit_group, main_(argc, argv), 0, 0);
}
__asm__(".globl _start\n_start:\n  mov %rsp, %rdi\n  and $-16, %rsp\n  call start_c\n  hlt\n");
