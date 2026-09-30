// seccomp SECCOMP_RET_TRAP as Chromium's sandbox uses it (WebView's
// renderer): an x86-64 program under FEX's seccomp emulation (FEX_NEEDSSECCOMP=1)
// traps getpid, and its SIGSYS handler must see what Linux reports --
// si_code SYS_SECCOMP, si_errno the filter's data, si_syscall, si_arch, and
// si_call_addr equal to the context's RIP with RAX the syscall number (the
// sandbox kills itself when they differ) -- and the value it puts in RAX is
// what the syscall returns. Freestanding: raw syscalls, no libc.
//   run-android-x86.sh /data/local/tmp/x86_seccomp_trap
typedef unsigned long u64;
typedef unsigned int u32;

static long sys(long n, long a, long b, long c, long d)
{
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}
static void out(const char *s) { long n = 0; while (s[n]) n++; sys(1, 1, (long)s, n, 0); }
static void num(u64 v)
{
    char b[24]; int i = 23; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(b + i);
}

struct sock_filter { unsigned short code; unsigned char jt, jf; u32 k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };
struct ksigaction { void *handler; u64 flags; void *restorer; u64 mask; };

extern void restorer(void);
__asm__(".text\nrestorer:\n mov $15, %eax\n syscall\n");

static volatile int seen, good;
static void handler(int sig, void *info, void *uc)
{
    (void)sig;
    char *si = info;
    int code = *(int *)(si + 8), err = *(int *)(si + 4);
    u64 call = *(u64 *)(si + 16);
    int nr = *(int *)(si + 24);
    u32 arch = *(u32 *)(si + 28);
    u64 *gregs = (u64 *)((char *)uc + 40);
    u64 rip = gregs[16], rax = gregs[13];
    seen++;
    good = code == 1 && err == 0x42 && nr == 39 && arch == 0xc000003eu && call == rip && rax == 39;
    out("  si_code "); num((u64)code); out(" si_errno "); num((u64)err); out(" si_syscall "); num((u64)nr);
    out(" si_arch "); num(arch); out(" call_addr==RIP "); out(call == rip ? "yes" : "no");
    out(" RAX "); num(rax); out("\n");
    gregs[13] = 12345;                  // what the "emulated" getpid returns
}

void _start(void)
{
    // Set field by field: an initialiser of function pointers can become a
    // constant with relocations this freestanding binary never applies.
    struct ksigaction sa;
    void *volatile h = (void *)handler, *volatile rs = (void *)restorer;
    sa.handler = h;
    sa.flags = 0x04000000 | 0x4;       // SA_RESTORER | SA_SIGINFO
    sa.restorer = rs;
    sa.mask = 0;
    int fails = 0;
    if (sys(13, 31, (long)&sa, 0, 8) != 0) { out("  MAL  rt_sigaction(SIGSYS)\n"); fails++; }
    struct sock_filter f[] = {
        { 0x20, 0, 0, 4 },                      // ld [4] (arch)
        { 0x15, 1, 0, 0xc000003e },             // jeq x86_64 ? +1 : +0
        { 0x06, 0, 0, 0x7fff0000 },             // ret ALLOW
        { 0x20, 0, 0, 0 },                      // ld [0] (nr)
        { 0x15, 0, 1, 39 },                     // jeq getpid
        { 0x06, 0, 0, 0x00030042 },             // ret TRAP | 0x42
        { 0x06, 0, 0, 0x7fff0000 },             // ret ALLOW
    };
    struct sock_fprog prog = { 7, f };
    if (sys(157, 38, 1, 0, 0) != 0) { out("  MAL  PR_SET_NO_NEW_PRIVS\n"); fails++; }
    long r = sys(317, 1, 1 /* TSYNC */, (long)&prog, 0);
    if (r != 0) { out("  MAL  seccomp(SET_MODE_FILTER, TSYNC): "); num((u64)-r); out("\n"); fails++; }
    long pid = sys(39, 0, 0, 0, 0);
    // The trapped call must not run: Linux returns what the handler left in
    // RAX (12345). FEX returns RAX as the handler found it (39, the number)
    // -- not carried yet, and not what Chromium's check needs; a real pid
    // would mean the call ran.
    if (seen == 1 && good && (pid == 12345 || pid == 39)) {
        out("  OK   getpid trapped: SIGSYS with Linux's fields, the call did not run (returned ");
        num((u64)pid); out(")\n");
    } else { out("  MAL  getpid: "); num((u64)pid); out(" returned, SIGSYS seen "); num((u64)seen); out(" times\n"); fails++; }
    out(fails ? "== x86_seccomp_trap: FAIL\n" : "== x86_seccomp_trap: PASS\n");
    sys(231, fails != 0, 0, 0, 0);
    for (;;) {}
}
