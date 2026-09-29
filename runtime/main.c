// lxrun -- run a Linux aarch64 ELF on macOS with no VM.
//
// Usage: lxrun [--trace] [--dry-run] <elf> [args...]

#include "lxrt.h"
#include "binder.h"
#include "props.h"
#include <dlfcn.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern char **environ;
void lxrt_dispatch_init_brk(uint64_t start);
void lxrt_window_pump(volatile bool *guest_running);
bool lxrt_absorb_runtime_fault(int dsig, siginfo_t *dinfo, void *uap);   // signal.c

// AppKit owns the main thread: a window is only created there and only behaves
// while a run loop is draining. So the guest runs on a second thread and the
// main thread stays in the pump. A guest that never asks for a window pays one
// idle thread for it.
static volatile bool g_guest_running = true;

struct guest_start { uint64_t entry; void *sp; };

static void *guest_thread(void *arg)
{
    struct guest_start *gs = arg;
    // Registered before any guest code runs, with tid == pid, so the thread
    // table always holds the initial thread (lxrt_thread_exit counts it).
    (void)lxrt_gettid();
    // A process-directed signal goes to any thread that is not blocking it,
    // and the main thread is sitting inside AppKit. Delivering a guest handler
    // there would rewrite AppKit's context into guest code. main() blocks
    // everything before spawning this thread; the guest thread is where
    // signals belong, so it unblocks them here. Threads the guest clones
    // inherit this mask, which is what Linux does too.
    sigset_t empty;
    sigemptyset(&empty);
    pthread_sigmask(SIG_SETMASK, &empty, NULL);
    // sigaltstack is per thread. Installed here, on the thread that runs the
    // guest -- not in main(), whose alt stack belongs to AppKit's thread.
    // Measured: with it in main(), every host signal frame for the guest
    // thread went on the guest thread's own stack, right below the
    // interrupted sp, where FEX's signal handler then built the guest frame
    // over it; the host handler's epilogue returned into zeroed memory (dash
    // under FEX: SIGILL/SIGSEGV at pc 0 in the parent after its first fork).
    lxrt_host_altstack_install();
    lxrt_enter(gs->entry, gs->sp);
}

// A guest that faults would otherwise die anonymously. Report the faulting pc,
// the instruction word there, and where it sits relative to the loaded image --
// enough to find it in a disassembly without attaching a debugger.
static struct lxrt_image *g_img;

static void fault_report(int sig, siginfo_t *info, void *uap)
{
    ucontext_t *uc = (ucontext_t *)uap;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;

    // Before reporting anything: a JIT mode transition is not a crash, and
    // neither is any other fault the runtime raises itself (W^X and sub-page
    // flips, copy-on-write, the trampolines' sp alignment). The same chain as
    // signal.c's host_handler, alignment filter included: this copy once
    // lacked it, and a misaligned store-release into a W^X page was taken for
    // a write flip and retried forever.
    if (sig == SIGTRAP && lxrt_jit_stub_trap(uap))
        return;
    if (lxrt_absorb_runtime_fault(sig, info, uap))
        return;
    const char *name = sig == SIGILL ? "SIGILL" : sig == SIGTRAP ? "SIGTRAP"
                     : sig == SIGBUS ? "SIGBUS" : sig == SIGSYS ? "SIGSYS (a live svc: x8/x16 below)"
                     : "SIGSEGV";

    char buf[2048];
    int n = snprintf(buf, sizeof buf, "\n[lxrt] %s at pc 0x%llx", name,
                     (unsigned long long)pc);
    // Which address, and the thread's sp and lr: a fault inside a host
    // routine (a zygote died once in _platform_memset, stage 23) says
    // nothing without them.
    if ((sig == SIGBUS || sig == SIGSEGV) && info)
        n += snprintf(buf + n, sizeof buf - n, " addr %p sp 0x%llx lr 0x%llx",
                      info->si_addr, (unsigned long long)uc->uc_mcontext->__ss.__sp,
                      (unsigned long long)uc->uc_mcontext->__ss.__lr);
    if (g_img) {
        uint64_t base = (uint64_t)g_img->base;
        if (pc >= base && pc < base + g_img->span)
            n += snprintf(buf + n, sizeof buf - n,
                          " = image+0x%llx", (unsigned long long)(pc - base));
        else if (lxrt_pool_contains(pc))
            n += snprintf(buf + n, sizeof buf - n, " = inside a trampoline pool");
        else
            n += snprintf(buf + n, sizeof buf - n, " = outside the guest image");
        // Reading the faulting word is safe: the page is mapped, it just does
        // not do what the guest expected.
        if (pc >= base && pc + 4 <= base + g_img->span)
            n += snprintf(buf + n, sizeof buf - n, ", insn 0x%08x",
                          *(uint32_t *)pc);
    }
    {
        // Which file the pc is in: the region that contains it, by name
        // (proc_regionfilename answers for the region AT the address given,
        // so ask with the region's own start).
        mach_vm_address_t ra = pc;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t ri;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        char fname[512] = "";
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&ri, &cnt, &obj) == KERN_SUCCESS && ra <= pc) {
            if (proc_regionfilename(getpid(), ra, fname, sizeof fname) <= 0)
                fname[0] = 0;
            n += snprintf(buf + n, sizeof buf - n, " [region 0x%llx+0x%llx%s%s]",
                          (unsigned long long)ra, (unsigned long long)rs,
                          fname[0] ? " " : "", fname);
        }
        // Outside the image (JIT output, a dlopened library): the word too,
        // read without trusting the page, and the hardware x18. Code the
        // runtime never rewrote (a JIT's) keeps using x18, which Darwin
        // zeroes on every exception; llvmpipe's JIT died reading through a
        // null base in stage 24 (benchmarks/stage24-minecraft-prism.txt).
        uint32_t word = 0;
        mach_vm_size_t got = 0;
        if ((!g_img || pc < (uint64_t)g_img->base || pc + 4 > (uint64_t)g_img->base + g_img->span) &&
            mach_vm_read_overwrite(mach_task_self(), pc, sizeof word, (mach_vm_address_t)(uintptr_t)&word,
                                   &got) == KERN_SUCCESS && got == sizeof word)
            n += snprintf(buf + n, sizeof buf - n, ", insn 0x%08x, x18 0x%llx", word,
                          (unsigned long long)uc->uc_mcontext->__ss.__x[18]);
    }
    // A trap or fault inside host code (a libsystem routine the runtime
    // called: a SIGTRAP in pthread_jit_write_protect_np once, 1 of 5 x86
    // Steam starts, stage 23): name it and the runtime frames that led
    // there. dladdr is not async-signal-safe; the process is dying anyway.
    if (!g_img || pc < (uint64_t)g_img->base || pc >= (uint64_t)g_img->base + g_img->span) {
        Dl_info di;
        uint64_t lr = uc->uc_mcontext->__ss.__lr, fp = uc->uc_mcontext->__ss.__fp;
        if (dladdr((void *)(uintptr_t)pc, &di) && di.dli_sname)
            n += snprintf(buf + n, sizeof buf - n, "\n[lxrt]   pc %s+%llu", di.dli_sname,
                          (unsigned long long)(pc - (uint64_t)(uintptr_t)di.dli_saddr));
        uint64_t frames[8] = { lr };
        int nf = 1;
        for (; nf < 8 && fp && !(fp & 7); nf++) {
            mach_vm_size_t got = 0;
            uint64_t pair[2];
            if (mach_vm_read_overwrite(mach_task_self(), fp, sizeof pair,
                                       (mach_vm_address_t)(uintptr_t)pair, &got) != KERN_SUCCESS ||
                got != sizeof pair)
                break;
            frames[nf] = pair[1];
            if (pair[0] <= fp) break;
            fp = pair[0];
        }
        for (int k = 0; k < nf && n < (int)sizeof buf - 160; k++) {
            if (dladdr((void *)(uintptr_t)frames[k], &di) && di.dli_sname)
                n += snprintf(buf + n, sizeof buf - n, "\n[lxrt]   %s %s+%llu", k ? "from" : "lr",
                              di.dli_sname,
                              (unsigned long long)(frames[k] - (uint64_t)(uintptr_t)di.dli_saddr));
            else
                n += snprintf(buf + n, sizeof buf - n, "\n[lxrt]   %s 0x%llx", k ? "from" : "lr",
                              (unsigned long long)frames[k]);
        }
    }
    if (n > (int)sizeof buf - 256)      // snprintf returns what it wanted to write
        n = (int)sizeof buf - 256;
    if (sig == SIGSYS)
        n += snprintf(buf + n, sizeof buf - n, ", x8=0x%llx x16=0x%llx x30=0x%llx",
                      (unsigned long long)uc->uc_mcontext->__ss.__x[8],
                      (unsigned long long)uc->uc_mcontext->__ss.__x[16],
                      (unsigned long long)uc->uc_mcontext->__ss.__lr);
    n += snprintf(buf + n, sizeof buf - n,
                  "\n[lxrt] last unimplemented syscall: %ld (%llu total)\n",
                  lxrt_dispatch_last_unimplemented(),
                  (unsigned long long)lxrt_dispatch_unimplemented_count());
    write(2, buf, (size_t)n);
    _exit(128 + sig);
}

static void install_fault_reporter(struct lxrt_image *img)
{
    g_img = img;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_report;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;   // nested faults must arrive
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGSYS, &sa, NULL);
}

// Resolve a path the guest named, the same way the dispatcher will once the
// guest is running: inside LXRT_ROOT if one is set.
static const char *resolve_guest_path(const char *path)
{
    static char buf[1024];
    const char *mnt = lxrt_mounts_translate(path, buf, sizeof buf);
    if (mnt)
        return mnt;
    const char *root = getenv("LXRT_ROOT");
    if (!root || !*root || path[0] != '/')
        return path;
    snprintf(buf, sizeof buf, "%s%s", root, path);
    return buf;
}

// The binary named on the command line may be either: a host path, when a
// person runs lxrun by hand, or a guest path, when an execve() from inside
// re-ran the runtime. Take it as given if it exists, and only then look inside
// LXRT_ROOT -- prefixing unconditionally turns a host path into
// $LXRT_ROOT$LXRT_ROOT/....
// Guest root first: an exec'd /bin/sh is the guest's, and the host has its
// own /bin/sh (a Mach-O) -- host-first resolution handed that to the ELF
// loader ("not an ELF", measured: Xvnc's popen of xkbcomp). A path that only
// exists on the host (the program named on lxrun's command line) still works.
static const char *resolve_program(const char *path)
{
    extern const char *lxrt_guest_resolve_follow(const char *, char *, size_t);
    static char followed[PATH_MAX];
    path = lxrt_guest_resolve_follow(path, followed, sizeof followed);
    const char *rooted = resolve_guest_path(path);
    if (rooted && rooted != path && access(rooted, F_OK) == 0)
        return rooted;
    // Then the x86 rootfs FEX overlays on "/" (FEX_ROOTFS, a guest path),
    // before the host: inside a bwrap sandbox /usr is the aarch64 root's, and
    // `true` fell through to macOS's /usr/bin/true ("not an ELF").
    const char *rootfs = getenv("FEX_ROOTFS");
    if (rootfs && rootfs[0] == '/' && path[0] == '/') {
        char cand[PATH_MAX];
        if (snprintf(cand, sizeof cand, "%s%s", rootfs, path) < (int)sizeof cand) {
            const char *r = resolve_guest_path(cand);
            if (r && access(r, F_OK) == 0)
                return strdup(r);
        }
    }
    return path;
}

static int rewrite_and_seal_image(struct lxrt_image *img, const char *what)
{
    char *err = NULL;
    struct lxrt_rewrite_report rep;
    if (lxrt_rewrite_image(img, &rep, &err) != 0) {
        fprintf(lxrt_trace_stream(), "lxrun: rewrite %s: %s\n", what, err ? err : "failed");
        return -1;
    }
    fprintf(lxrt_trace_stream(), "[lxrt] rewrite: %zu words scanned | svc %zu found, %zu "
                    "rewritten, %zu poisoned | tls %zu reads + %zu writes, "
                    "%zu rewritten, %zu poisoned | ctr %zu | sysreg %zu\n",
            rep.scanned_words, rep.sites_found, rep.sites_rewritten,
            rep.sites_unreachable, rep.tls_read_found, rep.tls_write_found,
            rep.tls_rewritten, rep.tls_unreachable, rep.ctr_rewritten,
            rep.sysreg_rewritten);
    fprintf(lxrt_trace_stream(), "[lxrt] rewrite: x18 %zu found in %d code windows, %zu rewritten, "
                    "%zu unsupported, %zu unreachable\n", rep.x18_found, img->ncode,
            rep.x18_rewritten, rep.x18_unsupported, rep.x18_unreachable);
    if (rep.tls_kept)
        fprintf(lxrt_trace_stream(), "[lxrt] rewrite: %zu TLS reads kept in place (a module that "
                        "hashes its own code, runtime/tls.c)\n", rep.tls_kept);
    if (rep.hvc_or_smc_found)
        fprintf(lxrt_trace_stream(), "[lxrt] WARNING: %zu hvc/smc encodings in a userspace "
                        "image -- almost certainly literal-pool data caught by "
                        "the linear scan, which means the svc count may include "
                        "data too\n", rep.hvc_or_smc_found);

    // Seal the executable segments: they were left writable only so the
    // rewriting pass could patch them.
    for (int s = 0; s < img->nexec; s++) {
        if (img->subpage) {
            uint64_t s4 = LXRT_ALIGN_DOWN(img->exec[s].start, 4096);
            uint64_t e4 = LXRT_ALIGN_UP(img->exec[s].end, 4096);
            if (lxrt_subpage_mprotect(s4, e4 - s4, PROT_READ | PROT_EXEC) != 0)
                fprintf(lxrt_trace_stream(), "[lxrt] warning: could not seal 0x%llx read-execute\n",
                        (unsigned long long)s4);
            continue;
        }
        uint64_t start = LXRT_ALIGN_DOWN(img->exec[s].start, LXRT_HOST_PAGE);
        uint64_t end = LXRT_ALIGN_UP(img->exec[s].end, LXRT_HOST_PAGE);
        if (mprotect((void *)start, (size_t)(end - start),
                     PROT_READ | PROT_EXEC) != 0)
            fprintf(lxrt_trace_stream(), "[lxrt] warning: could not seal 0x%llx read-execute\n",
                    (unsigned long long)start);
    }
    return 0;
}

// A pread of a plain file cannot block, and yet a guest's first pread of its
// own image does. This runs the identical call at three points -- before
// anything is set up, after the image is mapped and rewritten, and after the
// signal handlers and JIT entitlement are live -- to find which piece of
// process state is responsible.
static void probe_read(const char *label, const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(lxrt_trace_stream(), "[probe] %-10s open: %s\n", label, strerror(errno));
        return;
    }
    char buf[512];
    fprintf(lxrt_trace_stream(), "[probe] %-10s pread... ", label);
    fflush(stderr);
    ssize_t r = pread(fd, buf, sizeof buf, 0);
    fprintf(lxrt_trace_stream(), "%zd (%s)\n", r, r < 0 ? strerror(errno) : "ok");
    close(fd);
}

static void usage(void)
{
    fprintf(lxrt_trace_stream(),
            "usage: lxrun [--trace] [--dry-run] [--probe <file>] <linux-aarch64-elf> [args...]\n"
            "  --trace    log every syscall the guest makes\n"
            "  --dry-run  load and rewrite, report, then exit without running\n");
}

// Debugging switches that must reach processes deep inside a sandbox, where
// pressure-vessel decides the environment: LXRT_* lines of the HOST file
// /tmp/lxrt-debug.env (KEY=VALUE) are set before anything reads them. Only
// LXRT_* keys, and only when the variable is not already set.
static void load_debug_env(void)
{
    FILE *f = fopen("/tmp/lxrt-debug.env", "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (strncmp(line, "LXRT_", 5) != 0 || !eq)
            continue;
        *eq = '\0';
        if (!getenv(line))
            setenv(line, eq + 1, 0);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    // The binder driver's hub (runtime/binder_hub.c) is this same executable.
    if (argc >= 2 && !strcmp(argv[1], "--binder-hub"))
        return lxrt_binder_hub_main(argc, argv);
    // So is Android's property service (runtime/propsvc.c).
    if (argc >= 2 && !strcmp(argv[1], "--property-service"))
        return lxrt_property_service_main(argc, argv);
    load_debug_env();
    lxrt_mounts_load_env();   // a fake bwrap's binds, if we are its child
    // A traced runtime traces its exec'd children too: the guest's execve
    // re-runs lxrun without our flags, so the flag travels in the environment.
    bool env_trace = getenv("LXRT_TRACE") && *getenv("LXRT_TRACE") == '1';
    bool trace = env_trace, dry = false;
    const char *probe = NULL;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) trace = true;
        else if (!strcmp(argv[i], "--dry-run")) dry = true;
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc) probe = argv[++i];
        else break;
    }
    // LXRT_TRACE_MATCH=<text>: trace only the processes whose command line
    // contains <text> -- one helper deep inside a process tree (a traced
    // Steam run is ~10 GB, and the tracing itself changes its timing).
    const char *match = getenv("LXRT_TRACE_MATCH");
    if (match && *match && !trace)
        for (int k = i; k < argc; k++)
            if (strstr(argv[k], match)) { trace = true; break; }
    if (probe)
        probe_read("at-start", probe);
    if (i >= argc) {
        usage();
        return 2;
    }

    // The guest binary is named the way the guest names it, so it resolves
    // inside LXRT_ROOT like every other absolute path. Without this, an
    // execve() that re-runs the runtime on /usr/bin/whatever looks for it on
    // the host.
    const char *path = resolve_program(argv[i]);
    char *err = NULL;

    // binfmt_script, the kernel's "#!interpreter [arg]": the interpreter runs
    // with the script's path as its argument. FEX resolves shebangs itself
    // for the guests it runs -- but only through its own rootfs, and inside a
    // pressure-vessel container it passed "#!/usr/bin/env python3" on to the
    // kernel, which here is this runtime. Without this the exec failed with
    // ENOEXEC and the shell fell back to running Proton's Python script as a
    // shell script ("from: not found", MEASURED).
    for (int depth = 0; depth < 4; depth++) {
        char hb[256];
        int sfd = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t sg = sfd >= 0 ? pread(sfd, hb, sizeof hb - 1, 0) : -1;
        if (sfd >= 0) close(sfd);
        if (sg < 3 || hb[0] != '#' || hb[1] != '!')
            break;
        hb[sg] = '\0';
        char *nl = strchr(hb, '\n');
        if (!nl)
            break;                              // Linux: line too long -> ENOEXEC
        *nl = '\0';
        char *p = hb + 2;
        while (*p == ' ' || *p == '\t') p++;
        char *interp = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        char *arg = NULL;
        if (*p) {
            *p++ = '\0';
            while (*p == ' ' || *p == '\t') p++;
            char *e = p + strlen(p);
            while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = '\0';
            if (*p) arg = p;                    // the rest of the line is ONE argument
        } else {
            char *e = interp + strlen(interp);
            while (e > interp && e[-1] == '\r') *--e = '\0';
        }
        if (!*interp)
            break;
        char **nv = calloc((size_t)argc + 3, sizeof(char *));
        if (!nv)
            return 1;
        int n = 0;
        for (int k = 0; k < i; k++)
            nv[n++] = argv[k];
        nv[n++] = strdup(interp);
        if (arg)
            nv[n++] = strdup(arg);
        for (int k = i; k < argc; k++)
            nv[n++] = argv[k];                  // the script, as the caller named it, then its args
        nv[n] = NULL;
        if (trace)
            fprintf(lxrt_trace_stream(), "[lxrt] binfmt_script: %s -> %s%s%s\n",
                    argv[i], interp, arg ? " " : "", arg ? arg : "");
        argv = nv;
        argc = n;
        path = resolve_program(argv[i]);
    }

    // binfmt_misc, the part of it a FEX install registers on Linux: an x86 or
    // i386 ELF handed to exec runs under FEX. The bwrap interpreter and any
    // aarch64 program that execs an x86 one land here with the x86 image.
    {
        unsigned char eh[20];
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t got = fd >= 0 ? pread(fd, eh, sizeof eh, 0) : -1;
        if (fd >= 0) close(fd);
        uint16_t machine = got == (ssize_t)sizeof eh ? (uint16_t)(eh[18] | eh[19] << 8) : 0;
        if (got == (ssize_t)sizeof eh && !memcmp(eh, "\177ELF", 4) &&
            (machine == 3 || machine == 62)) {
            // Default: the emulator prefix, whose FEX finds its own loader
            // and libraries there (also inside a bwrap sandbox, which binds it).
            const char *fex = getenv("LXRT_FEX");
            if (!fex || !*fex)
                fex = "/usr/lib/lxrt-emu/FEX";
            char **nv = calloc((size_t)argc + 2, sizeof(char *));
            if (!nv)
                return 1;
            int n = 0;
            for (int k = 0; k < i; k++)
                nv[n++] = argv[k];
            nv[n++] = (char *)fex;
            for (int k = i; k < argc; k++)
                nv[n++] = argv[k];
            nv[n] = NULL;
            if (trace)
                fprintf(lxrt_trace_stream(), "[lxrt] binfmt: %s is x86 (e_machine %u), running it under %s\n",
                        argv[i], machine, fex);
            argv = nv;
            argc = n;
            path = resolve_program(argv[i]);
        }
    }

    // What the process is decides what a read-write-execute mprotect means:
    // x86 code under FEX (read-write is enough) or native code (wxsplit.c).
    lxrt_wx_set_program(path);

    // Must precede any rewriting: the TLS trampolines encode the slot offset.
    if (lxrt_tls_init() != 0) {
        fprintf(lxrt_trace_stream(), "lxrun: cannot allocate a TSD slot for guest TLS\n");
        return 1;
    }
    struct lxrt_image img;

    if (lxrt_load_elf(path, &img, &err) != 0) {
        fprintf(lxrt_trace_stream(), "lxrun: %s\n", err ? err : "load failed");
        return 1;
    }
    fprintf(lxrt_trace_stream(), "[lxrt] %s: %s, %zu bytes at %p, entry 0x%llx\n",
            path, img.is_pie ? "PIE" : "EXEC", img.span, (void *)img.base,
            (unsigned long long)img.entry);
    lxrt_main_image_base = (uint64_t)(uintptr_t)img.base;
    lxrt_main_image_span = img.span;

    if (rewrite_and_seal_image(&img, path) != 0)
        return 1;

    // A dynamic executable names its loader in PT_INTERP. The loader is what
    // actually runs: it maps the executable's libraries, relocates everything,
    // and only then jumps to the executable's entry point.
    struct lxrt_image interp;
    uint64_t entry = img.entry;
    if (img.interp[0]) {
        extern const char *lxrt_guest_resolve_follow(const char *, char *, size_t);
        static char ifollowed[PATH_MAX];
        const char *ipath = resolve_guest_path(lxrt_guest_resolve_follow(img.interp, ifollowed, sizeof ifollowed));
        fprintf(lxrt_trace_stream(), "[lxrt] PT_INTERP %s -> %s\n", img.interp, ipath);
        if (lxrt_load_elf(ipath, &interp, &err) != 0) {
            fprintf(lxrt_trace_stream(), "lxrun: interpreter: %s\n", err ? err : "load failed");
            return 1;
        }
        if (rewrite_and_seal_image(&interp, ipath) != 0)
            return 1;
        img.interp_base = (uint64_t)interp.base;
        entry = interp.entry;
        fprintf(lxrt_trace_stream(), "[lxrt] interpreter at %p, entry 0x%llx\n",
                (void *)interp.base, (unsigned long long)entry);
    }

    if (probe)
        probe_read("post-load", probe);

    if (dry)
        return 0;

    // argv[i] rather than the resolved host path: the guest must see the name
    // it used, so an execve on /proc/self/exe resolves the same way again.
    // But Linux's /proc/self/exe is always absolute: a program started by a
    // relative path gets its absolute path in guest terms (inside LXRT_ROOT
    // or a bind: the guest's spelling; otherwise the host's, which is then
    // the guest's too), symlinks resolved as the kernel does. Static glibc's
    // _dl_get_origin asserts on a relative one (MEASURED: SIGABRT, rc 134).
    // The procfs link itself points at the image's host path whenever the
    // guest's name is not one, so that open() reaches the file; readlink
    // still gives the guest's name (dispatch.c, LNR_readlinkat).
    const char *exe_name = argv[i], *exe_link = NULL;
    static char exe_host[PATH_MAX], exe_guest[PATH_MAX], exe_same[PATH_MAX];
    if (realpath(path, exe_host)) {
        if (exe_name[0] != '/') {
            const char *g = lxrt_mounts_untranslate(exe_host, exe_guest, sizeof exe_guest);
            exe_name = g ? g : exe_host;
            exe_link = exe_host;
        } else if (!realpath(exe_name, exe_same) || strcmp(exe_same, exe_host) != 0) {
            exe_link = exe_host;            // a guest path (LXRT_ROOT, binds)
        }
    }
    lxrt_proc_init(exe_name, exe_link, argc - i, &argv[i]);
    lxrt_sysfs_init();
    install_fault_reporter(&img);
    lxrt_dispatch_set_trace(trace);
    // From here on the guest's own loader maps code, and every page of it
    // arrives with raw `svc` in it.
    lxrt_dispatch_set_rewrite_mapped(true);
    lxrt_vdso_setup();
    void *sp = lxrt_build_stack(&img, argc - i, &argv[i], environ, &err);
    if (!sp) {
        fprintf(lxrt_trace_stream(), "lxrun: stack: %s\n", err ? err : "failed");
        return 1;
    }
    lxrt_start_stack = (uint64_t)(uintptr_t)sp;

    // After the stack, so the heap hole is chosen above everything already
    // mapped rather than next to it.
    lxrt_dispatch_init_brk(LXRT_ALIGN_UP(img.brk + LXRT_HOST_PAGE, LXRT_HOST_PAGE));

    fprintf(lxrt_trace_stream(), "[lxrt] entering guest at 0x%llx, sp %p\n",
            (unsigned long long)entry, sp);
    fflush(stderr);

    // Block everything here first: the guest thread inherits this mask and
    // then clears it, so the main thread never becomes a signal target.
    sigset_t all, saved;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &saved);

    static struct guest_start gs;
    gs.entry = entry;
    gs.sp = sp;
    pthread_t th;
    if (pthread_create(&th, NULL, guest_thread, &gs) != 0) {
        fprintf(lxrt_trace_stream(), "lxrun: cannot start the guest thread\n");
        return 1;
    }
    if (probe) {
        probe_read("post-entry", probe);
    }
    // Experiment knob: LXRT_NO_PUMP=1 keeps the main thread out of AppKit and
    // CoreFoundation entirely (a plain sleep loop) -- to tell whether a kill
    // of the process comes through the run loop.
    if (getenv("LXRT_NO_PUMP")) {
        while (g_guest_running)
            usleep(10000);
    } else
        lxrt_window_pump(&g_guest_running);
    pthread_join(th, NULL);
    return 0;
}
