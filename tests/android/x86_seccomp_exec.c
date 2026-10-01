// exec from an x86-64 process with a seccomp filter (every Android app has
// one), under FEX's seccomp emulation: a shell script and an x86 program
// both start. FEX passed FEX_EXECVEFD=-100 (AT_FDCWD) with the inherited
// filter, and the next FEX read its program from that "descriptor":
// "Invalid or Unsupported elf file" for Termux's bootstrap
// (patches/fex-lxrt-execve-no-fd.patch). Freestanding: raw syscalls.
//   run-android-x86.sh /data/local/tmp/x86_seccomp_exec
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

struct sock_filter { unsigned short code; unsigned char jt, jf; u32 k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };

static int run(const char *path, char *const argv[], char *const envp[])
{
    long pid = sys(57, 0, 0, 0, 0);                 // fork
    if (pid == 0) {
        sys(59, (long)path, (long)argv, (long)envp, 0);
        sys(60, 127, 0, 0, 0);
    }
    int st = -1;
    sys(61, pid, (long)&st, 0, 0);                  // wait4
    return st;
}

void _start(void)
{
    int fails = 0;
    struct sock_filter f[] = { { 0x06, 0, 0, 0x7fff0000 } };   // ret ALLOW
    struct sock_fprog prog = { 1, f };
    if (sys(157, 38, 1, 0, 0) != 0) { out("  MAL  PR_SET_NO_NEW_PRIVS\n"); fails++; }
    long r = sys(317, 1, 0, (long)&prog, 0);
    if (r != 0) { out("  MAL  seccomp(SET_MODE_FILTER)\n"); fails++; }
    // A script, as Termux's bootstrap second stage runs.
    const char *script = "/data/local/tmp/x86_seccomp_exec.sh";
    long fd = sys(257, -100, (long)script, 01 | 0100 | 01000, 0755);   // O_WRONLY|O_CREAT|O_TRUNC
    const char body[] = "#!/system/bin/sh\nexit 7\n";
    sys(1, fd, (long)body, sizeof body - 1, 0);
    sys(3, fd, 0, 0, 0);
    // Arrays filled at run time: an initialiser of pointers can become a
    // constant table with relocations this freestanding binary never applies.
    char *volatile path_env = "PATH=/system/bin", *volatile sh = "/system/bin/sh",
         *volatile dash_c = "-c", *volatile code = "exit 9";
    char *envp[2], *sargv[2], *pargv[4];
    envp[0] = path_env; envp[1] = 0;
    sargv[0] = (char *)script; sargv[1] = 0;
    int st = run(script, sargv, envp);
    if (st == (7 << 8)) out("  OK   a script runs from a process with a seccomp filter (exit 7)\n");
    else { out("  MAL  the script did not run\n"); fails++; }
    sys(87, (long)script, 0, 0, 0);                 // unlink
    // An x86 program.
    pargv[0] = sh; pargv[1] = dash_c; pargv[2] = code; pargv[3] = 0;
    st = run(sh, pargv, envp);
    if (st == (9 << 8)) out("  OK   an x86 program runs from a process with a seccomp filter (exit 9)\n");
    else { out("  MAL  the x86 program did not run\n"); fails++; }
    out(fails ? "== x86_seccomp_exec: FAIL\n" : "== x86_seccomp_exec: PASS\n");
    sys(231, fails != 0, 0, 0, 0);
    for (;;) {}
}
