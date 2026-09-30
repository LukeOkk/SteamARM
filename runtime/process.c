// Processes.
//
// Everything else in this runtime lives in one address space, and that is where
// most of its advantage comes from -- the graphics bridge is a `blr` precisely
// because there is no boundary. FEX is the first thing that does not fit: it
// spawns FEXServer as a separate process to own the rootfs mount and shared
// config. Steam will be worse, since it runs a client, several web helpers and
// a game.
//
// So the runtime gets a real process model rather than a workaround. Two
// decisions, both deliberate:
//
//   * fork() is Darwin's fork(). The child inherits the whole address space,
//     including the guest image, its rewritten code and its trampolines, which
//     is exactly what Linux promises. Only the calling thread survives, which
//     is also what Linux promises.
//
//   * execve() re-executes THIS runtime on the target binary. A Linux ELF
//     cannot be handed to Darwin's execve; what can is lxrun itself, with the
//     guest path as its first argument. The child is then a fresh runtime with
//     a fresh rewriting pass, which is the only way an arbitrary new image
//     gets its `svc` sites handled.
//
// The cost is stated rather than hidden: a second process is a second runtime.
// None of what the single address space buys applies across that boundary, and
// anything the two processes share has to go through a real IPC mechanism, as
// it would on Linux.

#include "lxrt.h"
#include "android_ids.h"
bool lxrt_trace_on(void);

#include <errno.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

extern char **environ;

static const char *self_path(void)
{
    static char path[4096];
    static bool ready, tried;
    if (tried)
        return ready ? path : NULL;
    tried = true;
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0)
        return NULL;
    ready = true;
    return path;
}

long lxrt_fork(void)
{
    // Darwin's fork in a process with AppKit initialised is only safe if the
    // child execs promptly, which is the pattern every caller here uses. A
    // child that does not exec has no run loop and no pump thread, so window
    // and dispatch calls will not work in it.
    pid_t pid = fork();
    if (pid < 0)
        return LERR(errno);
    if (pid == 0) {
        // The child is a new process: new pid, one thread, its own /proc.
        // Without this the child's /proc regeneration landed in the PARENT's
        // directory and the parent read the child's maps and fds as its own
        // (measured: bash and dash under FEX both died in the parent after
        // their children ran, tests/elf/jit_fork.c never did).
        extern void lxrt_trace_after_fork(void);
        lxrt_trace_after_fork();
        lxrt_host_altstack_after_fork();
        lxrt_thread_after_fork();
        lxrt_proc_after_fork();
    }
    return pid;
}

long lxrt_execve(const char *path, char *const argv[], char *const envp[])
{
    if (!path || !argv)
        return LERR(EFAULT);
    // pressure-vessel "replaces itself with bwrap". There is no bwrap here and
    // could not be (no mount namespaces); the plan is interpreted instead.
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!strcmp(base, "bwrap") || !strcmp(base, "srt-bwrap"))
        return lxrt_bwrap_exec(argv, envp, lxrt_execve);
    // FEX runs the next x86-64 program by re-executing itself as
    // /proc/self/exe. That name means nothing to a fresh runtime, so it is
    // replaced with the image this runtime actually loaded.
    {
        char mine[64];
        snprintf(mine, sizeof mine, "/proc/%d/exe", getpid());
        if (!strcmp(path, "/proc/self/exe") || !strcmp(path, mine)) {
            // FEX starting an x86 bwrap (argv: FEX, guest program, args):
            // steam-runtime-check-requirements does this to test the
            // sandbox, and the real bwrap stops at capget/unshare. Interpret
            // it like pressure-vessel's own bwrap exec above.
            if (argv[0] && argv[1]) {
                const char *gb = strrchr(argv[1], '/');
                gb = gb ? gb + 1 : argv[1];
                if (!strcmp(gb, "bwrap") || !strcmp(gb, "srt-bwrap"))
                    return lxrt_bwrap_exec(argv + 1, envp, lxrt_execve);
            }
            const char *exe = lxrt_proc_exe_path();
            if (exe)
                path = exe;
        }
    }
    const char *self = self_path();
    if (!self)
        return LERR(ENOEXEC);

    int argc = 0;
    while (argv[argc])
        argc++;

    // lxrun <guest-binary> <original argv[1..]>. argv[0] of the guest is
    // dropped: the runtime rebuilds it from the path, which is what the guest
    // would see for a normal exec anyway.
    char **newargv = calloc((size_t)argc + 3, sizeof(char *));
    if (!newargv)
        return LERR(ENOMEM);
    int n = 0;
    newargv[n++] = (char *)self;
    newargv[n++] = (char *)path;
    for (int i = 1; i < argc; i++)
        newargv[n++] = argv[i];
    newargv[n] = NULL;

    // Carry tracing into the child (see main.c): append LXRT_TRACE=1 to the
    // environment the guest asked for.
    char *const *use_env = envp && envp[0] ? (char *const *)envp : environ;
    // Every runtime process gets OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES: a
    // guest that brought up Objective-C through a host library (MoltenVK,
    // behind the Vulkan shim) and then forks -- Chromium's zygote, anything
    // using fork after Vulkan -- is otherwise killed by libobjc in the child
    // ("... initialize may have been in progress in another thread when
    // fork() was called", MEASURED). libobjc reads it at start-up, so it has
    // to be in the environment the next runtime image starts with.
    {
        int ec = 0;
        bool have = false;
        while (use_env[ec]) {
            if (!strncmp(use_env[ec], "OBJC_DISABLE_INITIALIZE_FORK_SAFETY=", 36)) have = true;
            ec++;
        }
        if (!have) {
            char **withobjc = calloc((size_t)ec + 2, sizeof(char *));
            if (withobjc) {
                for (int k = 0; k < ec; k++) withobjc[k] = use_env[k];
                withobjc[ec] = "OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES";
                withobjc[ec + 1] = NULL;
                use_env = withobjc;
            }
        }
    }
    // Android ids (runtime/android_ids.h) follow execve, recomputed by
    // Linux's rules for the new image; whatever the guest's environment
    // said about them is replaced.
    const char *aids = lxrt_aids_exec_env();
    if (aids) {
        // ... and so do the binds its zygote made (mounts.c: a mount
        // namespace outlives exec).
        const char *mnt = lxrt_mounts_exec_env();
        int ec = 0;
        while (use_env[ec]) ec++;
        char **withids = calloc((size_t)ec + 3, sizeof(char *));
        if (withids) {
            int k = 0;
            for (int j = 0; j < ec; j++)
                if (strncmp(use_env[j], "LXRT_ANDROID_IDS=", 17) &&
                    strncmp(use_env[j], "LXRT_MOUNTS=", 12))
                    withids[k++] = use_env[j];
            withids[k++] = (char *)aids;
            if (mnt) withids[k++] = (char *)mnt;
            withids[k] = NULL;
            use_env = withids;
        }
    }
    if (lxrt_trace_on()) {
        int ec = 0;
        while (use_env[ec]) ec++;
        char **traced = calloc((size_t)ec + 3, sizeof(char *));
        if (traced) {
            bool have = false, have_fd = false;
            for (int k = 0; k < ec; k++) {
                traced[k] = use_env[k];
                if (!strncmp(use_env[k], "LXRT_TRACE=", 11)) have = true;
                if (!strncmp(use_env[k], "LXRT_TRACE_FD=", 14)) have_fd = true;
            }
            if (!have) traced[ec++] = "LXRT_TRACE=1";
            static char fdenv[32];
            if (!have_fd && lxrt_trace_fd() >= 0) {
                snprintf(fdenv, sizeof fdenv, "LXRT_TRACE_FD=%d", lxrt_trace_fd());
                traced[ec++] = fdenv;
            }
            traced[ec] = NULL;
            use_env = traced;
        }
    }
    // exec detaches every SysV segment: the IPC_RMIDs deferred until this
    // process's last detach (sysv_ipc.c) are due now.
    lxrt_sysv_exit();
    execve(self, newargv, use_env);
    // Only reached on failure; Linux's execve does not return on success.
    int e = errno;
    free(newargv);
    return LERR(e);
}

// Linux wait status and Darwin's share the layout -- the low 7 bits are the
// terminating signal (bit 7 the core flag), 0x7f means stopped with the signal
// in bits 8-15, the exit code sits in bits 8-15 -- but not the signal NUMBERS
// (SIGBUS is 10 on Darwin, SIGUSR1 on Linux), so those are mapped. The rusage
// is converted too: Darwin's timeval has a 32-bit tv_usec and padding, and a
// struct left unwritten showed bash's `time` printing garbage.
long lxrt_wait4(int pid, int *status, int loptions, void *rusage)
{
    int doptions = 0;
    if (loptions & 1) doptions |= WNOHANG;
    if (loptions & 2) doptions |= WUNTRACED;
    if (loptions & 8) doptions |= WCONTINUED;

    int st = 0;
    struct rusage ru;
    pid_t r = wait4(pid, &st, doptions, rusage ? &ru : NULL);
    if (r < 0)
        return LERR(errno);
    if (r > 0) {
        if (WIFSIGNALED(st)) {
            int l = lxrt_signo_to_linux(WTERMSIG(st));
            st = (st & ~0x7f) | ((l > 0 ? l : WTERMSIG(st)) & 0x7f);
        } else if (WIFSTOPPED(st)) {
            int l = lxrt_signo_to_linux(WSTOPSIG(st));
            st = (st & ~0xff00) | (((l > 0 ? l : WSTOPSIG(st)) & 0xff) << 8);
        }
        if (status)
            *status = st;
        if (rusage)
            lxrt_rusage_to_linux(&ru, rusage);
    } else if (rusage) {
        memset(rusage, 0, 144);   // WNOHANG, nothing reaped: Linux zeroes it
    }
    return r;
}
