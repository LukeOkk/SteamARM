// The register state an x86-64 SIGSEGV handler sees under FEX, as ART's
// implicit null checks need it (benchmarks/stage28-android-apk.txt): ART's
// fault handler takes the faulting method from [RSP] and the faulting
// instruction from RIP (FaultManager::GetMethodAndReturnPcAndSp), then turns
// the fault into a NullPointerException. Freestanding static x86-64, raw
// syscalls, like x86_lowwin.c.
//
// The probe does what ART's compiled code does -- sub rsp, 8; mov [rsp], rdi
// (the ArtMethod*); mov eax, [rsi + 8] with rsi = 0 -- and the handler checks
// si_addr, RIP (the faulting mov), RSP (after the sub) and [RSP] (the stored
// value), then resumes after the mov. Last line: "== x86_segv_ctx: N ok, M mal".
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
#define sys4(n, a, b, c, d) sys6(n, a, b, c, d, 0, 0)

static void out(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    sys4(1, 1, (i64)s, (i64)n, 0);
}
static void hex(u64 v)
{
    char b[19] = "0x";
    for (int i = 0; i < 16; i++) {
        int d = (int)((v >> (60 - 4 * i)) & 15);
        b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    b[18] = 0;
    out(b);
}
static int oks, mals;
static void verdict(const char *name, int ok)
{
    out(ok ? "  ok   " : "  MAL  ");
    out(name);
    out("\n");
    if (ok) oks++; else mals++;
}

extern char fault_insn[], fault_after[];
__attribute__((used, visibility("hidden"), aligned(16))) char altstack[65536];
static volatile u64 seen_addr, seen_rip, seen_rsp, seen_top, expect_rsp;
static volatile int handled;

// struct ucontext (x86-64): uc_mcontext.gregs starts at byte 40.
enum { G_RSP = 15, G_RIP = 16 };
__attribute__((used, visibility("hidden"))) void handler(int sig, void *info, void *uc)
{
    (void)sig;
    u64 *g = (u64 *)((char *)uc + 40);
    seen_addr = *(u64 *)((char *)info + 16);   // si_addr
    seen_rip = g[G_RIP];
    seen_rsp = g[G_RSP];
    seen_top = *(u64 *)g[G_RSP];
    handled++;
    g[G_RIP] += 3;                             // skip the 3-byte mov, as ART's handler redirects
}

__asm__(".globl restorer\nrestorer:\n mov $15, %eax\n syscall\n");
extern void restorer(void);

struct ksa { void *h; u64 flags; void *restorer; u64 mask; };

// ART's compiled int read(NullCheck n) { return n.value; }, byte for byte
// (oatdump of a dex2oat64 --compiler-filter=speed odex): entered by a call,
// so it begins a translated block of its own, as ART's methods do.
__asm__(".globl artlike\nartlike:\n"
        " sub $8, %rsp\n"
        " mov %rdi, (%rsp)\n"
        ".globl fault_insn\nfault_insn:\n"
        " mov 8(%rsi), %eax\n"
        ".globl fault_after\nfault_after:\n"
        " add $8, %rsp\n"
        " ret\n");

// ART's compiled NullCheck.main, in outline (oatdump): the implicit stack
// check, six pushes, sub rsp 56, the method store, gs:[0] (thread flags),
// then the inlined field read in a later block, reached by a jump. The
// caller is laid out like art_quick_invoke_static_stub: it stores a null
// "method" at its own [rsp] before the call.
__asm__(".globl artmain\nartmain:\n"
        " testq %rax, -8192(%rsp)\n"
        " push %r15\n push %r14\n push %r13\n push %r12\n push %rbp\n push %rbx\n"
        " sub $56, %rsp\n"
        " mov %rdi, (%rsp)\n"
        " cmpw $0, %gs:0\n"
        " jne 2f\n"
        " mov %rsi, %rax\n"
        " jmp 1f\n"
        "2: xor %eax, %eax\n"
        "1:\n"
        ".globl main_fault\nmain_fault:\n"
        " mov 8(%rax), %eax\n"
        " add $56, %rsp\n"
        " pop %rbx\n pop %rbp\n pop %r12\n pop %r13\n pop %r14\n pop %r15\n"
        " ret\n");

__attribute__((noinline)) static void probe_main(void)
{
    __asm__ volatile(
        "sub $16, %%rsp\n"
        "movq $0, (%%rsp)\n"                   // the stub's null method slot
        "mov %%rsp, %%rax\n"
        "sub $112, %%rax\n"                    // return address, 6 pushes, 56
        "mov %%rax, %0\n"
        "movabs $0x1122334455667788, %%rdi\n"
        "xor %%esi, %%esi\n"
        "call artmain\n"
        "add $16, %%rsp\n"
        : "=m"(expect_rsp) : : "rax", "rcx", "rdx", "rdi", "rsi", "r8", "r9", "r10", "r11", "memory");
}

// A second thread, as every ART process has several: it registers its own
// alternate signal stack and sleeps.
__attribute__((used, visibility("hidden"), aligned(16))) char altstack2[65536];
__attribute__((used, visibility("hidden"), aligned(16))) char thread_stack[65536];
__attribute__((used, visibility("hidden"))) void child_main(void)
{
    void *alt;
    __asm__("lea altstack2(%%rip), %0" : "=r"(alt));
    struct { void *sp; int flags; u64 size; } ss = { alt, 0, 65536 };
    sys4(131, (i64)&ss, 0, 0, 0);
    struct { i64 s, ns; } t = { 3600, 0 };
    for (;;) sys4(35, (i64)&t, 0, 0, 0);       // nanosleep
}
__asm__(".globl spawn_thread\nspawn_thread:\n"   // spawn_thread(stack_top)
        " mov %rdi, %rsi\n"
        " mov $0x50f00, %edi\n"                  // VM|FS|FILES|SIGHAND|THREAD|SYSVSEM
        " xor %edx, %edx\n xor %r10d, %r10d\n xor %r8d, %r8d\n"
        " mov $56, %eax\n syscall\n"
        " test %rax, %rax\n jnz 1f\n"
        " and $-16, %rsp\n call child_main\n"
        "1: ret\n");
extern void spawn_thread(void *stack_top);

__attribute__((noinline)) static void probe(void)
{
    __asm__ volatile(
        "mov %%rsp, %%rax\n"
        "sub $16, %%rax\n"                     // the return address, then the method slot
        "mov %%rax, %0\n"
        "movabs $0x1122334455667788, %%rdi\n"
        "xor %%esi, %%esi\n"
        "call artlike\n"
        : "=m"(expect_rsp) : : "rax", "rcx", "rdx", "rdi", "rsi", "r8", "r9", "r10", "r11", "memory");
}

// The same method copied into anonymous executable memory, as ART's JIT
// code cache holds it.
__attribute__((used, visibility("hidden"))) const unsigned char artlike_bytes[] = {
    0x48, 0x83, 0xec, 0x08,        // sub rsp, 8
    0x48, 0x89, 0x3c, 0x24,        // mov [rsp], rdi
    0x8b, 0x46, 0x08,              // mov eax, [rsi + 8]
    0x48, 0x83, 0xc4, 0x08,        // add rsp, 8
    0xc3,                          // ret
};
static volatile u64 jit_code;
__attribute__((noinline)) static void probe_jit(void)
{
    __asm__ volatile(
        "mov %%rsp, %%rax\n"
        "sub $16, %%rax\n"
        "mov %%rax, %0\n"
        "mov %1, %%rax\n"
        "movabs $0x1122334455667788, %%rdi\n"
        "xor %%esi, %%esi\n"
        "call *%%rax\n"
        : "=m"(expect_rsp) : "m"(jit_code) : "rax", "rcx", "rdx", "rdi", "rsi", "r8", "r9", "r10", "r11", "memory");
}

void _start(void)
{
    // Code addresses taken PC-relative: a static-pie that nobody relocates
    // has link-time addresses in its data.
    void *h, *r;
    __asm__("lea handler(%%rip), %0" : "=r"(h));
    __asm__("lea restorer(%%rip), %0" : "=r"(r));
    // ART's way: an alternate signal stack per thread and SA_ONSTACK.
    void *alt;
    __asm__("lea altstack(%%rip), %0" : "=r"(alt));
    struct { void *sp; int flags; u64 size; } ss = { alt, 0, 65536 };
    sys4(131, (i64)&ss, 0, 0, 0);               // sigaltstack
    struct ksa sa = { h, 0x04000000 | 4 | 0x08000000 /* SA_RESTORER | SA_SIGINFO | SA_ONSTACK */, r, 0 };
    sys4(13, 11, (i64)&sa, 0, 8);               // rt_sigaction(SIGSEGV)
    // ART keeps its Thread* in the GS base (arch_prctl ARCH_SET_GS) and reads
    // gs:[0] in every method's prologue.
    static u64 fake_thread[16];
    u64 *ft;
    __asm__("lea %1, %0" : "=r"(ft) : "m"(fake_thread));
    sys4(158, 0x1001, (i64)ft, 0, 0);           // arch_prctl(ARCH_SET_GS)
    probe();
    verdict("the handler ran once and execution resumed after the faulting load", handled == 1);
    out("       si_addr "); hex(seen_addr); out(", RIP "); hex(seen_rip); out(" (the mov at "); hex((u64)fault_insn);
    out("), RSP "); hex(seen_rsp); out(" (expected "); hex(expect_rsp); out("), [RSP] "); hex(seen_top); out("\n");
    verdict("si_addr is the address read (0x8)", seen_addr == 8);
    void *fi;
    __asm__("lea fault_insn(%%rip), %0" : "=r"(fi));
    verdict("RIP is the faulting instruction", seen_rip == (u64)fi);
    verdict("RSP is the guest's stack pointer at the fault", seen_rsp == expect_rsp);
    verdict("[RSP] holds what the code stored there (ART's method pointer)", seen_top == 0x1122334455667788ul);

    // Again from anonymous executable memory (ART's JIT code cache).
    u64 m = (u64)sys6(9, 0, 4096, 7 /* RWX */, 0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
    const unsigned char *src;
    __asm__("lea artlike_bytes(%%rip), %0" : "=r"(src));
    for (u64 i = 0; i < sizeof artlike_bytes; i++) ((unsigned char *)m)[i] = src[i];
    jit_code = m;
    handled = 0;
    seen_top = 0;
    probe_jit();
    out("       from anonymous memory at "); hex(m); out(": RIP "); hex(seen_rip); out(", RSP "); hex(seen_rsp);
    out(" (expected "); hex(expect_rsp); out("), [RSP] "); hex(seen_top); out("\n");
    verdict("anonymous code: the handler ran, RIP is the mov, RSP and [RSP] are right",
            handled == 1 && seen_rip == m + 8 && seen_rsp == expect_rsp && seen_top == 0x1122334455667788ul);
    // ART's main, in outline.
    handled = 0;
    seen_top = 0;
    probe_main();
    void *mf;
    __asm__("lea main_fault(%%rip), %0" : "=r"(mf));
    out("       main-like method: RIP "); hex(seen_rip); out(" (the mov at "); hex((u64)mf); out("), RSP "); hex(seen_rsp);
    out(" (expected "); hex(expect_rsp); out("), [RSP] "); hex(seen_top); out("\n");
    verdict("a main-like method (pushes, sub, a later block): RIP, RSP and [RSP] are right",
            handled == 1 && seen_rip == (u64)mf && seen_rsp == expect_rsp && seen_top == 0x1122334455667788ul);

    // Again with a second thread in the process.
    char *ts;
    __asm__("lea thread_stack(%%rip), %0" : "=r"(ts));
    spawn_thread(ts + 65536);
    struct { i64 s, ns; } pause = { 0, 50 * 1000 * 1000 };
    sys4(35, (i64)&pause, 0, 0, 0);
    handled = 0;
    seen_top = 0;
    probe_main();
    out("       with a second thread: RIP "); hex(seen_rip); out(", RSP "); hex(seen_rsp);
    out(" (expected "); hex(expect_rsp); out("), [RSP] "); hex(seen_top); out("\n");
    verdict("with a second thread (its own altstack): RIP, RSP and [RSP] are right",
            handled == 1 && seen_rip == (u64)mf && seen_rsp == expect_rsp && seen_top == 0x1122334455667788ul);

    // And from a MAP_SHARED memfd mapped twice, writable and executable, as
    // ART's JIT code cache is (memfd_create 319, ftruncate 77).
    i64 mfd = sys4(319, (i64)"jit-cache", 0, 0, 0);
    sys4(77, mfd, 4096, 0, 0);
    u64 rw = (u64)sys6(9, 0, 4096, 3, 0x01 /* MAP_SHARED */, mfd, 0);
    u64 rx = (u64)sys6(9, 0, 4096, 5, 0x01, mfd, 0);
    for (u64 i = 0; i < sizeof artlike_bytes; i++) ((unsigned char *)rw)[i] = src[i];
    jit_code = rx;
    handled = 0;
    seen_top = 0;
    probe_jit();
    out("       from a shared memfd view at "); hex(rx); out(": RIP "); hex(seen_rip); out(", RSP "); hex(seen_rsp);
    out(" (expected "); hex(expect_rsp); out("), [RSP] "); hex(seen_top); out("\n");
    verdict("dual-mapped memfd code: the handler ran, RIP is the mov, RSP and [RSP] are right",
            handled == 1 && seen_rip == rx + 8 && seen_rsp == expect_rsp && seen_top == 0x1122334455667788ul);
    out("== x86_segv_ctx: ");
    char n[4] = { (char)('0' + oks), 0 };
    out(n); out(" ok, ");
    n[0] = (char)('0' + mals); out(n); out(" mal\n");
    sys4(231, mals ? 1 : 0, 0, 0, 0);
}
