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
//
// Ported from process.c. The translation is deliberately literal: the same
// libc calls in the same order, the same C strings and raw pointer tables (the
// argument and environment vectors are the guest's and are handed to execve
// as they are), the same statics without locks where the C had none, and the
// same allocations left to the exec that follows.

use std::ffi::{c_char, c_int, c_long, c_void, CStr};
use std::ptr;
use std::sync::atomic::{AtomicI32, Ordering};

use crate::errno_map::lxrt_errno_to_linux;
use crate::ids::{
    lxrt_ids_cancel_child, lxrt_ids_child_after_fork, lxrt_ids_note_child, lxrt_ids_on,
    lxrt_ids_pid, lxrt_ids_publish_child, lxrt_ids_reaped_child, lxrt_ids_reserve_child,
    lxrt_ids_target_pid,
};

// Darwin values (<sys/errno.h>, <fcntl.h>, <sys/wait.h>, <dlfcn.h>).
const D_ENOEXEC: c_int = 8;
const D_ECHILD: c_int = 10;
const D_ENOMEM: c_int = 12;
const D_EFAULT: c_int = 14;
const D_EINVAL: c_int = 22;
const D_EAGAIN: c_int = 35;
const D_O_RDONLY: c_int = 0;
const D_O_CLOEXEC: c_int = 0x0100_0000;
const D_WNOHANG: c_int = 0x1;
const D_WUNTRACED: c_int = 0x2;
const D_WCONTINUED: c_int = 0x10;
const D_RTLD_DEFAULT: *mut c_void = -2isize as *mut c_void;

// pthread_mutex_t and PTHREAD_MUTEX_INITIALIZER (<sys/_pthread/_pthread_types.h>,
// <pthread/pthread_impl.h>).
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}
const _: [(); 64] = [(); std::mem::size_of::<PthreadMutex>()];
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

// struct rusage: two timevals and fourteen longs. Only ever filled by wait4
// and read by lxrt_rusage_to_linux.
#[repr(C)]
struct DarwinRusage([c_long; 18]);
const _: [(); 144] = [(); std::mem::size_of::<DarwinRusage>()];

type ExecGuest = unsafe extern "C" fn(
    path: *const c_char,
    argv: *const *mut c_char,
    envp: *const *mut c_char,
) -> c_long;

#[link(name = "System")]
unsafe extern "C" {
    static mut environ: *mut *mut c_char;
    static mut __stderrp: *mut c_void;

    fn __error() -> *mut c_int;
    fn _NSGetExecutablePath(buf: *mut c_char, bufsize: *mut u32) -> c_int;
    fn getpid() -> c_int;
    fn fork() -> c_int;
    fn execve(path: *const c_char, argv: *const *mut c_char, envp: *const *mut c_char) -> c_int;
    fn wait4(pid: c_int, status: *mut c_int, options: c_int, rusage: *mut DarwinRusage) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn pread(fd: c_int, buf: *mut c_void, n: usize, offset: i64) -> isize;
    fn close(fd: c_int) -> c_int;
    fn write(fd: c_int, buf: *const c_void, n: usize) -> isize;
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn snprintf(buf: *mut c_char, n: usize, format: *const c_char, ...) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn strerror(error: c_int) -> *const c_char;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn calloc(count: usize, size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn strlen(s: *const c_char) -> usize;
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    fn strncmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    fn strchr(s: *const c_char, c: c_int) -> *mut c_char;
    fn strrchr(s: *const c_char, c: c_int) -> *mut c_char;
    fn strtok_r(s: *mut c_char, sep: *const c_char, save: *mut *mut c_char) -> *mut c_char;
    fn memcmp(a: *const c_void, b: *const c_void, n: usize) -> c_int;

    // The rest of the runtime, still C.
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_after_fork();
    fn lxrt_trace_fd() -> c_int;
    fn lxrt_futex_park_reset();
    fn lxrt_host_altstack_after_fork();
    fn lxrt_thread_after_fork();
    fn lxrt_proc_after_fork();
    fn lxrt_proc_exe_path() -> *const c_char;
    fn lxrt_translate_guest_path(path: *const c_char, out: *mut c_char, n: usize) -> *const c_char; // dispatch.c
    fn lxrt_bwrap_exec(
        argv: *const *mut c_char,
        envp: *const *mut c_char,
        exec_guest: ExecGuest,
    ) -> c_long;
    fn lxrt_aids_exec_env() -> *const c_char;
    fn lxrt_mounts_exec_env() -> *const c_char;
    fn lxrt_sysv_exit();
    fn lxrt_signo_to_linux(darwin_sig: c_int) -> c_int;
    fn lxrt_rusage_to_linux(d: *const DarwinRusage, out: *mut c_void);
}

#[inline(always)]
fn lerr(e: c_int) -> c_long {
    -(lxrt_errno_to_linux(e) as c_long)
}

#[inline(always)]
fn cs(s: &'static CStr) -> *const c_char {
    s.as_ptr()
}

// calloc(count, sizeof(char *)) as a string table.
#[inline(always)]
unsafe fn calloc_table(count: usize) -> *mut *mut c_char {
    calloc(count, std::mem::size_of::<*mut c_char>()).cast()
}

static mut SELF_PATH: [c_char; 4096] = [0; 4096];
static mut SELF_READY: bool = false;
static mut SELF_TRIED: bool = false;

unsafe fn self_path() -> *const c_char {
    let path = ptr::addr_of_mut!(SELF_PATH).cast::<c_char>();
    if SELF_TRIED {
        return if SELF_READY { path } else { ptr::null() };
    }
    SELF_TRIED = true;
    let mut sz: u32 = 4096;
    if _NSGetExecutablePath(path, &mut sz) != 0 {
        return ptr::null();
    }
    SELF_READY = true;
    path
}

// Metal compiles shaders in MTLCompilerService, an XPC service inside
// Metal.framework that launchd keeps per process: libxpc registers a
// framework's services for the pid that loads it. The runtime links Metal
// (AppKit, QuartzCore), so that happens once, when an image starts; a fork
// child that does not exec has Metal loaded but nothing registered for its
// pid, and every compile fails: "Unable to reach MTLCompilerService ...
// Connection init failed at lookup with error 3 - No such process". Chromium
// starts its GPU process that way, from its unsandboxed zygote, and Steam's
// web helper drew a black window: vkCreateGraphicsPipelines -> -3 on
// MoltenVK, VK_ERROR_INVALID_SHADER_NV on KosmicKrisp (MEASURED;
// tests/elf/vk_pipeline.c "zygote" reproduces it). The Vulkan shim calls
// this before it creates an instance: in a process that has forked since its
// image started, it registers Metal's services again (xpc_add_bundle, flags
// 0: what fixed it, MEASURED in a plain macOS fork as well). About 2.6 ms,
// once per process -- not in every fork, which would tax every shell.
static mut G_XPC_PID: c_int = 0;

extern "C" fn xpc_pid_at_start() {
    unsafe {
        G_XPC_PID = getpid();
    }
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static XPC_PID_AT_START_INIT: extern "C" fn() = xpc_pid_at_start;

static mut METAL_ATTACH_MU: PthreadMutex = PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
};

#[no_mangle]
pub unsafe extern "C" fn lxrt_metal_attach() -> c_long {
    let mu = ptr::addr_of_mut!(METAL_ATTACH_MU);
    let me = getpid();
    pthread_mutex_lock(mu);
    if G_XPC_PID != me {
        let add = dlsym(D_RTLD_DEFAULT, cs(c"xpc_add_bundle"));
        if !add.is_null() {
            let add: unsafe extern "C" fn(*const c_char, c_int) = std::mem::transmute(add);
            add(cs(c"/System/Library/Frameworks/Metal.framework"), 0);
        }
        G_XPC_PID = me;
    }
    pthread_mutex_unlock(mu);
    0
}

static FORK_FAILED_SAID: AtomicI32 = AtomicI32::new(0);

#[no_mangle]
pub unsafe extern "C" fn lxrt_fork() -> c_long {
    // Darwin's fork in a process with AppKit initialised is only safe if the
    // child execs promptly, which is the pattern every caller here uses. A
    // child that does not exec has no run loop and no pump thread, so window
    // and dispatch calls will not work in it.
    let child_id = lxrt_ids_reserve_child();
    if lxrt_ids_on() && child_id < 2 {
        return lerr(D_EAGAIN);
    }
    let pid = fork();
    if pid < 0 {
        let e = *__error();
        lxrt_ids_cancel_child(child_id);
        // Said once per process: a failed fork is otherwise silent in most
        // guests (Chromium's GPU process launch reports only "error_code=1002",
        // its success code, and gives up after nine).
        if FORK_FAILED_SAID.fetch_add(1, Ordering::SeqCst) < 3 {
            let mut b = [0 as c_char; 160];
            let n = snprintf(
                b.as_mut_ptr(),
                160,
                cs(c"[lxrt] pid %d: fork failed: %s (errno %d)\n"),
                getpid(),
                strerror(e),
                e,
            );
            write(2, b.as_ptr().cast(), n as usize);
        }
        return lerr(e);
    }
    if pid == 0 {
        lxrt_futex_park_reset(); // the parent's other threads are not here
        lxrt_ids_child_after_fork(child_id);
        // The child is a new process: new pid, one thread, its own /proc.
        // Without this the child's /proc regeneration landed in the PARENT's
        // directory and the parent read the child's maps and fds as its own
        // (measured: bash and dash under FEX both died in the parent after
        // their children ran, tests/elf/jit_fork.c never did).
        lxrt_trace_after_fork();
        lxrt_host_altstack_after_fork();
        lxrt_thread_after_fork();
        lxrt_proc_after_fork();
    } else if lxrt_ids_on() {
        lxrt_ids_publish_child(child_id, pid);
        lxrt_ids_note_child(pid, child_id);
    }
    if lxrt_ids_on() && pid > 0 {
        child_id as c_long
    } else {
        pid as c_long
    }
}

// An aarch64 ELF linked at a fixed address (ET_EXEC): it would sit inside
// Darwin's 4 GiB __PAGEZERO and cannot be loaded (elf.rs).
unsafe fn unloadable_arm64(gpath: *const c_char) -> bool {
    let mut hp = [0 as c_char; 1024];
    let h = lxrt_translate_guest_path(gpath, hp.as_mut_ptr(), 1024);
    let fd = open(h, D_O_RDONLY | D_O_CLOEXEC);
    if fd < 0 {
        return false;
    }
    let mut e = [0u8; 20];
    let r = pread(fd, e.as_mut_ptr().cast(), 20, 0) == 20
        && memcmp(e.as_ptr().cast(), cs(c"\x7fELF").cast(), 4) == 0
        && e[4] == 2
        && (e[16] as c_int | (e[17] as c_int) << 8) == 2
        && (e[18] as c_int | (e[19] as c_int) << 8) == 183;
    close(fd);
    r
}

// Valve's arm64 Steam Linux Runtime (SteamLinuxRuntime_4-arm64) ships every
// pressure-vessel program non-PIE, linked at 0x400000: none can run here
// ("non-PIE (ET_EXEC) images cannot be loaded", MEASURED with the 4.0
// runtime of 2026-08-05). pressure-vessel-wrap's job -- start the game's
// command, after "--" -- is done the way it does it without a container:
// in this root, the root of the arm64 client (a SteamOS userspace), with
// its --env-if-host=VAR=VALUE settings applied. Its other options choose
// the container and are ignored.
unsafe fn pv_wrap_stand_in(argv: *const *mut c_char, envp: *const *mut c_char) -> c_long {
    let mut argc: c_int = 0;
    let mut dd: c_int = -1;
    while !(*argv.offset(argc as isize)).is_null() {
        argc += 1;
    }
    let mut i: c_int = 1;
    while i < argc && dd < 0 {
        if strcmp(*argv.offset(i as isize), cs(c"--")) == 0 {
            dd = i;
        }
        i += 1;
    }
    if dd < 0 || dd + 1 >= argc {
        return lerr(D_EINVAL);
    }
    let src: *const *mut c_char = if !envp.is_null() && !(*envp).is_null() {
        envp
    } else {
        environ
    };
    let mut ec: c_int = 0;
    while !(*src.offset(ec as isize)).is_null() {
        ec += 1;
    }
    let env = calloc_table(ec.wrapping_add(dd).wrapping_add(2) as usize);
    if env.is_null() {
        return lerr(D_ENOMEM);
    }
    let mut n: c_int = 0;
    let mut i: c_int = 0;
    while i < ec {
        *env.offset(n as isize) = *src.offset(i as isize);
        n += 1;
        i += 1;
    }
    let mut i: c_int = 1;
    while i < dd {
        let arg = *argv.offset(i as isize);
        i += 1;
        if strncmp(arg, cs(c"--env-if-host="), 14) != 0 {
            continue;
        }
        let kv = arg.add(14);
        let eq = strchr(kv, b'=' as c_int);
        if eq.is_null() {
            continue;
        }
        let kl = (eq.offset_from(kv) as usize).wrapping_add(1);
        let mut j: c_int = 0;
        while j < n {
            if strncmp(*env.offset(j as isize), kv, kl) == 0 {
                break;
            }
            j += 1;
        }
        *env.offset(j as isize) = kv;
        if j == n {
            n += 1;
        }
    }
    *env.offset(n as isize) = cs(c"STEAMARM_PRESSURE_VESSEL=stand-in").cast_mut();
    n += 1;
    *env.offset(n as isize) = ptr::null_mut();
    let cmd: *const c_char = *argv.offset((dd + 1) as isize);
    fprintf(
        __stderrp,
        cs(c"lxrun: pressure-vessel-wrap (non-PIE, cannot load here) stood in for: %s\n"),
        cmd,
    );
    lxrt_execve(cmd, argv.offset((dd + 1) as isize), env)
}

static mut EXEC_ARGS: [c_char; 2048] = [0; 2048];
static mut ENV_CARRY: c_int = -1; // LXRT_ENV_CARRY=0: bisecting aid
static mut TRACE_FDENV: [c_char; 32] = [0; 32];

#[no_mangle]
pub unsafe extern "C" fn lxrt_execve(
    path: *const c_char,
    argv: *const *mut c_char,
    envp: *const *mut c_char,
) -> c_long {
    let mut path = path;
    if path.is_null() || argv.is_null() {
        return lerr(D_EFAULT);
    }
    // pressure-vessel "replaces itself with bwrap". There is no bwrap here and
    // could not be (no mount namespaces); the plan is interpreted instead.
    let slash = strrchr(path, b'/' as c_int);
    let base: *const c_char = if !slash.is_null() { slash.add(1) } else { path };
    if strcmp(base, cs(c"bwrap")) == 0 || strcmp(base, cs(c"srt-bwrap")) == 0 {
        return lxrt_bwrap_exec(argv, envp, lxrt_execve);
    }
    if strcmp(base, cs(c"pressure-vessel-wrap")) == 0 && unloadable_arm64(path) {
        return pv_wrap_stand_in(argv, envp);
    }
    // FEX runs the next x86-64 program by re-executing itself as
    // /proc/self/exe. That name means nothing to a fresh runtime, so it is
    // replaced with the image this runtime actually loaded.
    {
        let mut mine = [0 as c_char; 64];
        snprintf(mine.as_mut_ptr(), 64, cs(c"/proc/%d/exe"), lxrt_ids_pid());
        if strcmp(path, cs(c"/proc/self/exe")) == 0 || strcmp(path, mine.as_ptr()) == 0 {
            // FEX starting an x86 bwrap (argv: FEX, guest program, args):
            // steam-runtime-check-requirements does this to test the
            // sandbox, and the real bwrap stops at capget/unshare. Interpret
            // it like pressure-vessel's own bwrap exec above.
            if !(*argv).is_null() && !(*argv.add(1)).is_null() {
                let a1: *const c_char = *argv.add(1);
                let gs = strrchr(a1, b'/' as c_int);
                let gb: *const c_char = if !gs.is_null() { gs.add(1) } else { a1 };
                if strcmp(gb, cs(c"bwrap")) == 0 || strcmp(gb, cs(c"srt-bwrap")) == 0 {
                    return lxrt_bwrap_exec(argv.add(1), envp, lxrt_execve);
                }
            }
            let exe = lxrt_proc_exe_path();
            if !exe.is_null() {
                path = exe;
            }
        }
    }
    let this = self_path();
    if this.is_null() {
        return lerr(D_ENOEXEC);
    }

    let mut argc: c_int = 0;
    while !(*argv.offset(argc as isize)).is_null() {
        argc += 1;
    }

    // lxrun <guest-binary> <original argv[1..]>. argv[0] of the guest is
    // dropped: the runtime rebuilds it from the path, which is what the guest
    // would see for a normal exec anyway.
    //
    // LXRT_EXEC_ARGS=NAME[!PREFIX]:ARG ARG...[;...] appends arguments when a
    // program named NAME is executed (space-separated, no quoting), unless
    // one of its arguments starts with PREFIX: the launcher gives Steam's
    // steamwebhelper Chromium switches this way (steamarm-settings-env) instead of
    // editing Valve's files, which the client would check and download
    // again -- to the browser process only ("steamwebhelper!--type="):
    // Chromium hands its children the switches they need, and appended to
    // every renderer and utility process they killed them at start, 3,200
    // relaunches in 3 minutes and a black window (MEASURED, stage 49).
    let mut extra_args: [*mut c_char; 32] = [ptr::null_mut(); 32];
    let extra = extra_args.as_mut_ptr();
    let mut nextra: c_int = 0;
    let exec_args = ptr::addr_of_mut!(EXEC_ARGS).cast::<c_char>();
    let ea = getenv(cs(c"LXRT_EXEC_ARGS"));
    if !ea.is_null() && *ea != 0 && strlen(ea) < 2048 {
        snprintf(exec_args, 2048, cs(c"%s"), ea);
        let mut save: *mut c_char = ptr::null_mut();
        let mut ent = strtok_r(exec_args, cs(c";"), &mut save);
        while !ent.is_null() && nextra < 32 {
            'entry: {
                let colon = strchr(ent, b':' as c_int);
                if colon.is_null() {
                    break 'entry;
                }
                *colon = 0;
                let mut bang = strchr(ent, b'!' as c_int);
                if !bang.is_null() {
                    *bang = 0;
                    bang = bang.add(1);
                }
                if strcmp(ent, base) != 0 {
                    break 'entry;
                }
                let mut excluded = false;
                let mut i: c_int = 1;
                while !bang.is_null() && *bang != 0 && i < argc && !excluded {
                    excluded = strncmp(*argv.offset(i as isize), bang, strlen(bang)) == 0;
                    i += 1;
                }
                if excluded {
                    break 'entry;
                }
                let mut save2: *mut c_char = ptr::null_mut();
                let mut a = strtok_r(colon.add(1), cs(c" "), &mut save2);
                while !a.is_null() && nextra < 32 {
                    *extra.offset(nextra as isize) = a;
                    nextra += 1;
                    a = strtok_r(ptr::null_mut(), cs(c" "), &mut save2);
                }
            }
            ent = strtok_r(ptr::null_mut(), cs(c";"), &mut save);
        }
    }
    let newargv = calloc_table(
        (argc as usize)
            .wrapping_add(3)
            .wrapping_add(nextra as usize),
    );
    if newargv.is_null() {
        return lerr(D_ENOMEM);
    }
    let mut n: c_int = 0;
    *newargv.offset(n as isize) = this.cast_mut();
    n += 1;
    *newargv.offset(n as isize) = path.cast_mut();
    n += 1;
    let mut i: c_int = 1;
    while i < argc {
        *newargv.offset(n as isize) = *argv.offset(i as isize);
        n += 1;
        i += 1;
    }
    let mut i: c_int = 0;
    while i < nextra {
        *newargv.offset(n as isize) = *extra.offset(i as isize);
        n += 1;
        i += 1;
    }
    *newargv.offset(n as isize) = ptr::null_mut();

    // Carry tracing into the child (see main.c): append LXRT_TRACE=1 to the
    // environment the guest asked for.
    let mut use_env: *const *mut c_char = if !envp.is_null() && !(*envp).is_null() {
        envp
    } else {
        environ
    };
    // Every runtime process gets OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES: a
    // guest that brought up Objective-C through a host library (MoltenVK,
    // behind the Vulkan shim) and then forks -- Chromium's zygote, anything
    // using fork after Vulkan -- is otherwise killed by libobjc in the child
    // ("... initialize may have been in progress in another thread when
    // fork() was called", MEASURED). libobjc reads it at start-up, so it has
    // to be in the environment the next runtime image starts with.
    {
        let mut ec: c_int = 0;
        let mut have = false;
        while !(*use_env.offset(ec as isize)).is_null() {
            if strncmp(
                *use_env.offset(ec as isize),
                cs(c"OBJC_DISABLE_INITIALIZE_FORK_SAFETY="),
                36,
            ) == 0
            {
                have = true;
            }
            ec += 1;
        }
        if !have {
            let withobjc = calloc_table((ec as usize).wrapping_add(2));
            if !withobjc.is_null() {
                let mut k: c_int = 0;
                while k < ec {
                    *withobjc.offset(k as isize) = *use_env.offset(k as isize);
                    k += 1;
                }
                *withobjc.offset(ec as isize) =
                    cs(c"OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES").cast_mut();
                *withobjc.offset(ec as isize + 1) = ptr::null_mut();
                use_env = withobjc;
            }
        }
    }
    // The runtime's own settings (LXRT_*: the root, the binder, property
    // and input directories, ...) and the emulator's (FEX_*: its rootfs,
    // server socket, guest base, ...) are not the guest's to drop: a program
    // that execs with an environment of its own -- Termux runs its bootstrap
    // that way -- started the next runtime without its root ("lxrun: open
    // /usr/lib/lxrt-emu/FEX: No such file or directory", MEASURED). Those the
    // guest's environment lacks are carried over from this process's.
    {
        let mut ec: c_int = 0;
        let mut extra: c_int = 0;
        while !(*use_env.offset(ec as isize)).is_null() {
            ec += 1;
        }
        if ENV_CARRY < 0 {
            let c = getenv(cs(c"LXRT_ENV_CARRY"));
            ENV_CARRY = !(!c.is_null() && *c == b'0' as c_char) as c_int;
        }
        let mut e = environ;
        while ENV_CARRY != 0 && !(*e).is_null() {
            if strncmp(*e, cs(c"LXRT_"), 5) == 0 || strncmp(*e, cs(c"FEX_"), 4) == 0 {
                extra += 1;
            }
            e = e.add(1);
        }
        if extra != 0 {
            let carried = calloc_table(ec.wrapping_add(extra).wrapping_add(1) as usize);
            if !carried.is_null() {
                let mut k: c_int = 0;
                let mut j: c_int = 0;
                while j < ec {
                    *carried.offset(k as isize) = *use_env.offset(j as isize);
                    k += 1;
                    j += 1;
                }
                let mut e = environ;
                while !(*e).is_null() {
                    let var = *e;
                    e = e.add(1);
                    if strncmp(var, cs(c"LXRT_"), 5) != 0 && strncmp(var, cs(c"FEX_"), 4) != 0 {
                        continue;
                    }
                    // The guest's own LD_PRELOAD, renamed for FEX's linker
                    // (main.c): the guest's environment decides that one.
                    // And FEX's per-exec descriptors (the program, the
                    // seccomp state): numbers this runtime was started with,
                    // meaningless -- or someone else's -- in the next image.
                    if strncmp(var, cs(c"LXRT_GUEST_LD_PRELOAD="), 22) == 0
                        || strncmp(var, cs(c"FEX_EXECVEFD="), 13) == 0
                        || strncmp(var, cs(c"FEX_SECCOMPFD="), 14) == 0
                    {
                        continue;
                    }
                    let eq = strchr(var, b'=' as c_int);
                    let nl = if !eq.is_null() {
                        (eq.offset_from(var) as usize).wrapping_add(1)
                    } else {
                        strlen(var)
                    };
                    let mut have = false;
                    let mut j: c_int = 0;
                    while j < ec && !have {
                        have = strncmp(*use_env.offset(j as isize), var, nl) == 0;
                        j += 1;
                    }
                    if !have {
                        *carried.offset(k as isize) = var;
                        k += 1;
                    }
                }
                *carried.offset(k as isize) = ptr::null_mut();
                use_env = carried;
            }
        }
    }
    // Android ids (runtime/android_ids.h) follow execve, recomputed by
    // Linux's rules for the new image; whatever the guest's environment
    // said about them is replaced.
    let aids = lxrt_aids_exec_env();
    if !aids.is_null() {
        // ... and so do the binds its zygote made (mounts.c: a mount
        // namespace outlives exec).
        let mnt = lxrt_mounts_exec_env();
        let mut ec: c_int = 0;
        while !(*use_env.offset(ec as isize)).is_null() {
            ec += 1;
        }
        let withids = calloc_table((ec as usize).wrapping_add(3));
        if !withids.is_null() {
            let mut k: c_int = 0;
            let mut j: c_int = 0;
            while j < ec {
                let var = *use_env.offset(j as isize);
                if strncmp(var, cs(c"LXRT_ANDROID_IDS="), 17) != 0
                    && strncmp(var, cs(c"LXRT_MOUNTS="), 12) != 0
                {
                    *withids.offset(k as isize) = var;
                    k += 1;
                }
                j += 1;
            }
            *withids.offset(k as isize) = aids.cast_mut();
            k += 1;
            if !mnt.is_null() {
                *withids.offset(k as isize) = mnt.cast_mut();
                k += 1;
            }
            *withids.offset(k as isize) = ptr::null_mut();
            use_env = withids;
        }
    }
    if lxrt_ids_on() {
        let mut ec: c_int = 0;
        let mut have = false;
        while !(*use_env.offset(ec as isize)).is_null() {
            if strncmp(*use_env.offset(ec as isize), cs(c"LXRT_SMALL_IDS="), 15) == 0 {
                have = true;
            }
            ec += 1;
        }
        if !have {
            let withids = calloc_table((ec as usize).wrapping_add(2));
            if withids.is_null() {
                return lerr(D_ENOMEM);
            }
            let mut k: c_int = 0;
            while k < ec {
                *withids.offset(k as isize) = *use_env.offset(k as isize);
                k += 1;
            }
            // The terminator is calloc's zero, as in the C.
            *withids.offset(ec as isize) = cs(c"LXRT_SMALL_IDS=1").cast_mut();
            use_env = withids;
        }
    }
    // A process traced because its command line matched LXRT_TRACE_MATCH
    // (main.c) does not pass tracing on: its children are traced only if
    // their own command lines match, as the variable says.
    let tenv = getenv(cs(c"LXRT_TRACE"));
    let tmatch = getenv(cs(c"LXRT_TRACE_MATCH"));
    let by_match =
        !tmatch.is_null() && *tmatch != 0 && !(!tenv.is_null() && *tenv == b'1' as c_char);
    if lxrt_trace_on() && !by_match {
        let mut ec: c_int = 0;
        while !(*use_env.offset(ec as isize)).is_null() {
            ec += 1;
        }
        let traced = calloc_table((ec as usize).wrapping_add(3));
        if !traced.is_null() {
            let mut have = false;
            let mut have_fd = false;
            let mut k: c_int = 0;
            while k < ec {
                let var = *use_env.offset(k as isize);
                *traced.offset(k as isize) = var;
                if strncmp(var, cs(c"LXRT_TRACE="), 11) == 0 {
                    have = true;
                }
                if strncmp(var, cs(c"LXRT_TRACE_FD="), 14) == 0 {
                    have_fd = true;
                }
                k += 1;
            }
            if !have {
                *traced.offset(ec as isize) = cs(c"LXRT_TRACE=1").cast_mut();
                ec += 1;
            }
            if !have_fd && lxrt_trace_fd() >= 0 {
                let fdenv = ptr::addr_of_mut!(TRACE_FDENV).cast::<c_char>();
                snprintf(fdenv, 32, cs(c"LXRT_TRACE_FD=%d"), lxrt_trace_fd());
                *traced.offset(ec as isize) = fdenv;
                ec += 1;
            }
            *traced.offset(ec as isize) = ptr::null_mut();
            use_env = traced;
        }
    }
    // exec detaches every SysV segment: the IPC_RMIDs deferred until this
    // process's last detach (sysv_ipc.c) are due now.
    lxrt_sysv_exit();
    execve(this, newargv, use_env);
    // Only reached on failure; Linux's execve does not return on success.
    let e = *__error();
    free(newargv.cast());
    lerr(e)
}

// <sys/wait.h>, on the int status.
#[inline(always)]
fn wstatus(x: c_int) -> c_int {
    x & 0o177
}
const WSTOPPED: c_int = 0o177;
#[inline(always)]
fn wstopsig(x: c_int) -> c_int {
    x >> 8
}
#[inline(always)]
fn wifstopped(x: c_int) -> bool {
    wstatus(x) == WSTOPPED && wstopsig(x) != 0x13
}
#[inline(always)]
fn wifexited(x: c_int) -> bool {
    wstatus(x) == 0
}
#[inline(always)]
fn wifsignaled(x: c_int) -> bool {
    wstatus(x) != WSTOPPED && wstatus(x) != 0
}
#[inline(always)]
fn wtermsig(x: c_int) -> c_int {
    wstatus(x)
}

// Linux wait status and Darwin's share the layout -- the low 7 bits are the
// terminating signal (bit 7 the core flag), 0x7f means stopped with the signal
// in bits 8-15, the exit code sits in bits 8-15 -- but not the signal NUMBERS
// (SIGBUS is 10 on Darwin, SIGUSR1 on Linux), so those are mapped. The rusage
// is converted too: Darwin's timeval has a 32-bit tv_usec and padding, and a
// struct left unwritten showed bash's `time` printing garbage.
#[no_mangle]
pub unsafe extern "C" fn lxrt_wait4(
    pid: c_int,
    status: *mut c_int,
    loptions: c_int,
    rusage: *mut c_void,
) -> c_long {
    let host_pid = lxrt_ids_target_pid(pid);
    if lxrt_ids_on() && pid > 0 && host_pid < 0 {
        return lerr(D_ECHILD);
    }
    let mut doptions: c_int = 0;
    if loptions & 1 != 0 {
        doptions |= D_WNOHANG;
    }
    if loptions & 2 != 0 {
        doptions |= D_WUNTRACED;
    }
    if loptions & 8 != 0 {
        doptions |= D_WCONTINUED;
    }

    let mut st: c_int = 0;
    let mut ru = std::mem::MaybeUninit::<DarwinRusage>::uninit();
    let r = wait4(
        host_pid,
        &mut st,
        doptions,
        if !rusage.is_null() {
            ru.as_mut_ptr()
        } else {
            ptr::null_mut()
        },
    );
    if r < 0 {
        return lerr(*__error());
    }
    if r > 0 {
        if wifsignaled(st) {
            let l = lxrt_signo_to_linux(wtermsig(st));
            st = (st & !0x7f) | ((if l > 0 { l } else { wtermsig(st) }) & 0x7f);
        } else if wifstopped(st) {
            let l = lxrt_signo_to_linux(wstopsig(st));
            st = (st & !0xff00) | (((if l > 0 { l } else { wstopsig(st) }) & 0xff) << 8);
        }
        if !status.is_null() {
            *status = st;
        }
        if !rusage.is_null() {
            lxrt_rusage_to_linux(ru.as_ptr(), rusage);
        }
    } else if !rusage.is_null() {
        ptr::write_bytes(rusage.cast::<u8>(), 0, 144); // WNOHANG, nothing reaped: Linux zeroes it
    }
    if r > 0 {
        lxrt_ids_reaped_child(r, wifexited(st) || wifsignaled(st)) as c_long
    } else {
        r as c_long
    }
}
