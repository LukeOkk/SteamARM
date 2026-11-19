// Per-process mount table: what a fake bwrap leaves behind. See mounts.h.
// Evidence for what has to be interpreted:
// benchmarks/stage6-bwrap-plan.txt (the real plan pressure-vessel generates,
// 47 options, no --unshare-*), and pressure-vessel's own strings: it hands the
// plan over as `bwrap --args FD` and "replaces self with bwrap" (execve).
//
// This is a line-for-line port of mounts.c. Path translation is reached from
// guest syscall paths and from signal handlers, so it keeps the C's shape:
// fixed-size buffers, libc string routines, no Rust allocation, no panics,
// and a pthread mutex that is only ever taken with trylock.

use std::ffi::{c_char, c_int, c_long, c_uint, c_void};
use std::mem::MaybeUninit;
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};

// Darwin arm64 values (<sys/syslimits.h>, <sys/errno.h>, <sys/fcntl.h>,
// <sys/stat.h>, <time.h>).
const PATH_MAX: usize = 1024;
const D_ENOENT: c_int = 2;
const D_EINTR: c_int = 4;
const D_ENOMEM: c_int = 12;
const D_EEXIST: c_int = 17;
const D_EINVAL: c_int = 22;
const D_ENOSYS: c_int = 78;
const D_O_WRONLY: c_int = 0x0001;
const D_O_CREAT: c_int = 0x0200;
const D_O_TRUNC: c_int = 0x0400;
const D_S_IFMT: u16 = 0o170000;
const D_S_IFDIR: u16 = 0o040000;
const D_S_IFREG: u16 = 0o100000;
const D_CLOCK_MONOTONIC: c_int = 6;
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

type ModeT = u16;

// struct stat, Darwin arm64 (always the 64-bit inode layout): 144 bytes.
#[repr(C)]
struct Stat {
    st_dev: i32,
    st_mode: ModeT,
    st_nlink: u16,
    rest: [u64; 17],
}

#[repr(C)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: c_long,
}

// pthread_mutex_t, Darwin LP64: signature + 56 opaque bytes.
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}

type ExecGuest =
    unsafe extern "C" fn(*const c_char, *const *mut c_char, *const *mut c_char) -> c_long;

#[link(name = "System")]
unsafe extern "C" {
    fn __error() -> *mut c_int;
    fn _exit(status: c_int) -> !;
    fn atoi(text: *const c_char) -> c_int;
    fn chdir(path: *const c_char) -> c_int;
    fn chmod(path: *const c_char, mode: ModeT) -> c_int;
    fn clock_gettime(clock: c_int, time: *mut Timespec) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn free(pointer: *mut c_void);
    fn getenv(name: *const c_char) -> *mut c_char;
    fn malloc(size: usize) -> *mut c_void;
    fn memcpy(destination: *mut c_void, source: *const c_void, length: usize) -> *mut c_void;
    fn memmove(destination: *mut c_void, source: *const c_void, length: usize) -> *mut c_void;
    fn mkdir(path: *const c_char, mode: ModeT) -> c_int;
    fn mkdtemp(template: *mut c_char) -> *mut c_char;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn pthread_mutex_trylock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn qsort(
        base: *mut c_void,
        count: usize,
        width: usize,
        compare: unsafe extern "C" fn(*const c_void, *const c_void) -> c_int,
    );
    fn read(fd: c_int, buffer: *mut c_void, length: usize) -> isize;
    fn readlink(path: *const c_char, buffer: *mut c_char, length: usize) -> isize;
    fn realloc(pointer: *mut c_void, size: usize) -> *mut c_void;
    #[link_name = "realpath$DARWIN_EXTSN"]
    fn realpath(path: *const c_char, resolved: *mut c_char) -> *mut c_char;
    fn snprintf(buffer: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn sprintf(buffer: *mut c_char, format: *const c_char, ...) -> c_int;
    fn stat(path: *const c_char, out: *mut Stat) -> c_int;
    fn strchr(text: *const c_char, character: c_int) -> *mut c_char;
    fn strcmp(left: *const c_char, right: *const c_char) -> c_int;
    fn strdup(text: *const c_char) -> *mut c_char;
    fn strerror(error: c_int) -> *const c_char;
    fn strlen(text: *const c_char) -> usize;
    fn strncmp(left: *const c_char, right: *const c_char, length: usize) -> c_int;
    fn strrchr(text: *const c_char, character: c_int) -> *mut c_char;
    fn strstr(haystack: *const c_char, needle: *const c_char) -> *mut c_char;
    fn strtok(text: *mut c_char, separators: *const c_char) -> *mut c_char;
    fn strtol(text: *const c_char, end: *mut *mut c_char, base: c_int) -> c_long;
    fn symlink(target: *const c_char, path: *const c_char) -> c_int;
    fn unlink(path: *const c_char) -> c_int;
    fn write(fd: c_int, buffer: *const c_void, length: usize) -> isize;

    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_host_tmpdir() -> *const c_char;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
}

// A NUL-terminated string literal as a C pointer.
macro_rules! cs {
    ($text:expr) => {
        concat!($text, "\0").as_ptr().cast::<c_char>()
    };
}

#[inline(always)]
unsafe fn errno() -> c_int {
    *__error()
}

// LERR(e): -(long)lxrt_errno_to_linux(e)
#[inline(always)]
unsafe fn lerr(error: c_int) -> c_long {
    -(lxrt_errno_to_linux(error) as c_long)
}

#[inline(always)]
unsafe fn streq(left: *const c_char, right: *const c_char) -> bool {
    strcmp(left, right) == 0
}

#[inline(always)]
fn s_isdir(mode: ModeT) -> bool {
    mode & D_S_IFMT == D_S_IFDIR
}

#[inline(always)]
fn s_isreg(mode: ModeT) -> bool {
    mode & D_S_IFMT == D_S_IFREG
}

// A PATH_MAX (or other fixed-size) stack buffer, uninitialised as in C.
#[inline(always)]
fn stack_buffer<const N: usize>() -> MaybeUninit<[c_char; N]> {
    MaybeUninit::uninit()
}

const MAX_MOUNTS: usize = 256;

#[repr(C)]
struct Mount {
    dst: [c_char; PATH_MAX],
    src: [c_char; PATH_MAX],
    ro: bool,
}

static mut G_M: MaybeUninit<[Mount; MAX_MOUNTS]> = MaybeUninit::zeroed();
static mut G_N: c_int = 0;

#[inline(always)]
unsafe fn mount_at(index: c_int) -> *mut Mount {
    ptr::addr_of_mut!(G_M)
        .cast::<Mount>()
        .offset(index as isize)
}

#[inline(always)]
unsafe fn mount_dst(index: c_int) -> *mut c_char {
    ptr::addr_of_mut!((*mount_at(index)).dst).cast::<c_char>()
}

#[inline(always)]
unsafe fn mount_src(index: c_int) -> *mut c_char {
    ptr::addr_of_mut!((*mount_at(index)).src).cast::<c_char>()
}

#[inline(always)]
unsafe fn g_n() -> c_int {
    ptr::addr_of!(G_N).read()
}

// --------------------------------------------------------------- the table

unsafe extern "C" fn by_dst_len_desc(a: *const c_void, b: *const c_void) -> c_int {
    let la = strlen(ptr::addr_of!((*a.cast::<Mount>()).dst).cast());
    let lb = strlen(ptr::addr_of!((*b.cast::<Mount>()).dst).cast());
    if la < lb {
        1
    } else if la > lb {
        -1
    } else {
        0
    }
}

unsafe fn add_mount(dst: *const c_char, src: *const c_char, ro: bool) {
    let n = g_n();
    if n >= MAX_MOUNTS as c_int {
        fprintf(
            lxrt_trace_stream(),
            cs!("[lxrt] bwrap: more than %d mounts, dropping %s\n"),
            MAX_MOUNTS as c_int,
            dst,
        );
        return;
    }
    snprintf(mount_dst(n), PATH_MAX, cs!("%s"), dst);
    // Canonical on the host side, so that getcwd() -- which the kernel
    // answers canonically (/private/tmp, not /tmp) -- maps back.
    let mut real = stack_buffer::<PATH_MAX>();
    let real = real.as_mut_ptr().cast::<c_char>();
    let resolved: *const c_char = if !realpath(src, real).is_null() {
        real
    } else {
        src
    };
    snprintf(mount_src(n), PATH_MAX, cs!("%s"), resolved);
    (*mount_at(n)).ro = ro;
    ptr::addr_of_mut!(G_N).write(n + 1);
    qsort(
        ptr::addr_of_mut!(G_M).cast(),
        (n + 1) as usize,
        size_of::<Mount>(),
        by_dst_len_desc,
    );
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_mounts_active() -> bool {
    g_n() > 0
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_mounts_bind(dst: *const c_char, src: *const c_char, ro: bool) {
    lxrt_mounts_unbind(dst);
    add_mount(dst, src, ro);
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_mounts_unbind(dst: *const c_char) -> bool {
    let n = g_n();
    let mut i: c_int = 0;
    while i < n {
        if strcmp(mount_dst(i), dst) == 0 {
            memmove(
                mount_at(i).cast(),
                mount_at(i + 1).cast(),
                size_of::<Mount>() * (n - i - 1) as usize,
            );
            ptr::addr_of_mut!(G_N).write(n - 1);
            return true;
        }
        i += 1;
    }
    false
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_mounts_exec_env() -> *const c_char {
    static mut LAST: *mut c_char = ptr::null_mut();
    if g_n() == 0 {
        return ptr::null();
    }
    let s = serialize();
    if s.is_null() {
        return ptr::null();
    }
    let last = ptr::addr_of_mut!(LAST);
    free((*last).cast());
    *last = malloc(strlen(s) + 13).cast();
    if !(*last).is_null() {
        sprintf(*last, cs!("LXRT_MOUNTS=%s"), s);
    }
    free(s.cast());
    *last
}

// Is this guest path inside a read-only bind? bwrap makes the mount itself
// read-only; here the dispatcher refuses the write with EROFS instead.
#[no_mangle]
pub unsafe extern "C" fn lxrt_mounts_readonly(path: *const c_char) -> bool {
    let n = g_n();
    if n == 0 || path.is_null() || *path != b'/' as c_char {
        return false;
    }
    let mut i: c_int = 0;
    while i < n {
        let d = mount_dst(i);
        let l = strlen(d);
        let next = *path.add(l);
        if strncmp(path, d, l) == 0
            && (next == 0 || next == b'/' as c_char || (l == 1 && *d == b'/' as c_char))
        {
            return (*mount_at(i)).ro; // longest prefix wins: the table is sorted
        }
        i += 1;
    }
    false
}

// Host path -> guest path, for getcwd(). Binds first (longest source wins is
// not needed: the first match is fine, sources rarely nest), then the root.
#[no_mangle]
pub unsafe extern "C" fn lxrt_mounts_untranslate(
    host: *const c_char,
    buf: *mut c_char,
    n: usize,
) -> *const c_char {
    if host.is_null() || *host != b'/' as c_char {
        return ptr::null();
    }
    let count = g_n();
    let mut i: c_int = 0;
    while i < count {
        let src = mount_src(i);
        let l = strlen(src);
        if l > 1
            && strncmp(host, src, l) == 0
            && (*host.add(l) == 0 || *host.add(l) == b'/' as c_char)
        {
            snprintf(buf, n, cs!("%s%s"), mount_dst(i), host.add(l));
            return buf;
        }
        i += 1;
    }
    let root = getenv(cs!("LXRT_ROOT"));
    if !root.is_null() && *root != 0 {
        let mut real = stack_buffer::<PATH_MAX>();
        let real = real.as_mut_ptr().cast::<c_char>();
        let r: *const c_char = if !realpath(root, real).is_null() {
            real
        } else {
            root
        };
        let l = strlen(r);
        if strncmp(host, r, l) == 0 && (*host.add(l) == 0 || *host.add(l) == b'/' as c_char) {
            let rest: *const c_char = if *host.add(l) != 0 {
                host.add(l)
            } else {
                cs!("/")
            };
            snprintf(buf, n, cs!("%s"), rest);
            return buf;
        }
    }
    ptr::null()
}

// Prefix cache for through_root_symlinks(): every absolute path of every
// path syscall walked its /a, /a/b, ... prefixes with one readlink() each,
// again and again for the same directories -- a quarter of a game's loading
// thread was in readlink while Counter-Strike 2 opened its assets (sampled,
// benchmarks/stage61-cs2-stutter.txt). The symlinks in the sandbox root are
// made when the tree is set up and the guest's own writes go to the binds,
// so the answer for a prefix (plain, or a link and its target) is kept for a
// short while and dropped when this process makes, moves or removes anything
// (lxrt_mounts_symlinks_changed()). Other guest processes writing links into
// the root itself are what the time limit is for.
const SYMCACHE_SLOTS: usize = 512;
const SYMCACHE_TTL_NS: u64 = 2 * 1000 * 1000 * 1000;

#[repr(C)]
struct SymcacheEntry {
    prefix: *mut c_char, // in_root path, malloc'd
    target: *mut c_char, // readlink() result, or NULL for a plain entry
    when: u64,           // CLOCK_MONOTONIC ns of the lookup
    gen: u64,
}

static mut G_SYMCACHE: MaybeUninit<[SymcacheEntry; SYMCACHE_SLOTS]> = MaybeUninit::zeroed();
static mut G_SYMCACHE_LOCK: PthreadMutex = PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
};
static G_SYMCACHE_GEN: AtomicU64 = AtomicU64::new(1);

#[no_mangle]
pub extern "C" fn lxrt_mounts_symlinks_changed() {
    G_SYMCACHE_GEN.fetch_add(1, Ordering::SeqCst);
}

unsafe fn symcache_now() -> u64 {
    let mut ts = MaybeUninit::<Timespec>::uninit();
    clock_gettime(D_CLOCK_MONOTONIC, ts.as_mut_ptr());
    let ts = ts.as_ptr();
    ((*ts).tv_sec as u64)
        .wrapping_mul(1_000_000_000)
        .wrapping_add((*ts).tv_nsec as u64)
}

unsafe fn symcache_hash(mut s: *const c_char) -> c_uint {
    let mut h: c_uint = 2_166_136_261;
    while *s != 0 {
        h = (h ^ (*s as u8) as c_uint).wrapping_mul(16_777_619);
        s = s.add(1);
    }
    h % SYMCACHE_SLOTS as c_uint
}

#[inline(always)]
unsafe fn symcache_slot(in_root: *const c_char) -> *mut SymcacheEntry {
    ptr::addr_of_mut!(G_SYMCACHE)
        .cast::<SymcacheEntry>()
        .add(symcache_hash(in_root) as usize)
}

// readlink(in_root) through the cache. Returns the target length, 0 for a
// plain entry (no link), -1 when the cache could not answer and the caller
// must ask the kernel. A signal handler that reaches path translation must
// not block on the lock: trylock, and the kernel is asked when it is held.
unsafe fn symcache_readlink(in_root: *const c_char, target: *mut c_char, n: usize) -> isize {
    if !getenv(cs!("LXRT_NO_SYMCACHE")).is_null() {
        return -1;
    }
    if pthread_mutex_trylock(ptr::addr_of_mut!(G_SYMCACHE_LOCK)) != 0 {
        return -1;
    }
    let mut rc: isize = -1;
    let e = symcache_slot(in_root);
    let gen = G_SYMCACHE_GEN.load(Ordering::SeqCst);
    if !(*e).prefix.is_null()
        && (*e).gen == gen
        && strcmp((*e).prefix, in_root) == 0
        && symcache_now().wrapping_sub((*e).when) < SYMCACHE_TTL_NS
    {
        if (*e).target.is_null() {
            rc = 0;
        } else {
            let tl = strlen((*e).target);
            if tl < n {
                memcpy(target.cast(), (*e).target.cast(), tl + 1);
                rc = tl as isize;
            }
        }
    }
    pthread_mutex_unlock(ptr::addr_of_mut!(G_SYMCACHE_LOCK));
    rc
}

unsafe fn symcache_store(in_root: *const c_char, target: *const c_char) {
    if pthread_mutex_trylock(ptr::addr_of_mut!(G_SYMCACHE_LOCK)) != 0 {
        return;
    }
    let e = symcache_slot(in_root);
    let p = strdup(in_root);
    let t = if !target.is_null() {
        strdup(target)
    } else {
        ptr::null_mut()
    };
    if !p.is_null() && (target.is_null() || !t.is_null()) {
        free((*e).prefix.cast());
        free((*e).target.cast());
        (*e).prefix = p;
        (*e).target = t;
        (*e).when = symcache_now();
        (*e).gen = G_SYMCACHE_GEN.load(Ordering::SeqCst);
    } else {
        free(p.cast());
        free(t.cast());
    }
    pthread_mutex_unlock(ptr::addr_of_mut!(G_SYMCACHE_LOCK));
}

// Symlinks that --symlink created in the sandbox root (/lib -> usr/lib, ...)
// point INTO binds; left to the host kernel they resolve against the empty
// directories standing in for the bind targets. Rewrite them in guest terms
// first, component by component, until the path meets a bind or a real file.
unsafe fn through_root_symlinks(
    path: *const c_char,
    out: *mut c_char,
    n: usize,
) -> *const c_char {
    let root = getenv(cs!("LXRT_ROOT"));
    if root.is_null() || *root == 0 {
        return path;
    }
    let mut cur_buffer = stack_buffer::<PATH_MAX>();
    let cur = cur_buffer.as_mut_ptr().cast::<c_char>();
    snprintf(cur, PATH_MAX, cs!("%s"), path);
    let mut hops = 0;
    while hops < 40 {
        let mut changed = false;
        // Walk prefixes /a, /a/b, ... that no bind covers.
        let mut slash = cur.add(1);
        loop {
            if *slash != b'/' as c_char && *slash != 0 {
                slash = slash.add(1);
                continue;
            }
            let save = *slash;
            if save == 0 {
                break; // the last component is left alone: readlink,
                       // lstat and unlink act on the link itself
            }
            *slash = 0;
            let mut bound = false;
            let count = g_n();
            let mut i: c_int = 0;
            while i < count && !bound {
                let dst = mount_dst(i);
                let l = strlen(dst);
                bound = strncmp(cur, dst, l) == 0
                    && (*cur.add(l) == 0 || *cur.add(l) == b'/' as c_char);
                i += 1;
            }
            if bound {
                *slash = save;
                break;
            }
            let mut in_root_buffer = stack_buffer::<PATH_MAX>();
            let in_root = in_root_buffer.as_mut_ptr().cast::<c_char>();
            let mut target_buffer = stack_buffer::<PATH_MAX>();
            let target = target_buffer.as_mut_ptr().cast::<c_char>();
            snprintf(in_root, PATH_MAX, cs!("%s%s"), root.cast_const(), cur.cast_const());
            let mut tl = symcache_readlink(in_root, target, PATH_MAX);
            if tl < 0 {
                tl = readlink(in_root, target, PATH_MAX - 1);
                if tl > 0 {
                    *target.offset(tl) = 0;
                    symcache_store(in_root, target);
                } else if tl < 0 && errno() == D_EINVAL {
                    symcache_store(in_root, ptr::null()); // exists, not a link
                }
            }
            if tl > 0 {
                *target.offset(tl) = 0;
                let mut next_buffer = stack_buffer::<PATH_MAX>();
                let next = next_buffer.as_mut_ptr().cast::<c_char>();
                let separator: *const c_char = if save != 0 { cs!("/") } else { cs!("") };
                let rest: *const c_char = if save != 0 { slash.add(1) } else { cs!("") };
                if *target == b'/' as c_char {
                    snprintf(
                        next,
                        PATH_MAX,
                        cs!("%s%s%s"),
                        target.cast_const(),
                        separator,
                        rest,
                    );
                } else {
                    let dir = strrchr(cur, b'/' as c_int);
                    *dir = 0;
                    snprintf(
                        next,
                        PATH_MAX,
                        cs!("%s/%s%s%s"),
                        cur.cast_const(),
                        target.cast_const(),
                        separator,
                        rest,
                    );
                }
                snprintf(cur, PATH_MAX, cs!("%s"), next.cast_const());
                changed = true;
                break;
            }
            *slash = save;
            if save == 0 {
                break;
            }
            slash = slash.add(1);
        }
        if !changed {
            break;
        }
        hops += 1;
    }
    snprintf(out, n, cs!("%s"), cur.cast_const());
    out
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_mounts_translate(
    path: *const c_char,
    buf: *mut c_char,
    n: usize,
) -> *const c_char {
    if g_n() == 0 || path.is_null() || *path != b'/' as c_char {
        return ptr::null();
    }
    let mut canon = stack_buffer::<PATH_MAX>();
    let path = through_root_symlinks(path, canon.as_mut_ptr().cast(), PATH_MAX);
    let count = g_n();
    let mut i: c_int = 0;
    while i < count {
        let d = mount_dst(i);
        let l = strlen(d);
        if strncmp(path, d, l) != 0 {
            i += 1;
            continue;
        }
        let is_root = l == 1 && *d == b'/' as c_char;
        if *path.add(l) != 0 && *path.add(l) != b'/' as c_char && !is_root {
            i += 1;
            continue;
        }
        let rest = path.add(if is_root { 0 } else { l });
        let k = snprintf(buf, n, cs!("%s%s"), mount_src(i).cast_const(), rest);
        if k <= 0 || k as usize >= n {
            return ptr::null();
        }
        return buf;
    }
    ptr::null()
}

// Records: dst \x1e src \x1e ro, separated by \x1f.
#[no_mangle]
pub unsafe extern "C" fn lxrt_mounts_load_env() {
    let env = getenv(cs!("LXRT_MOUNTS"));
    if env.is_null() || *env == 0 {
        return;
    }
    let copy = strdup(env);
    let mut rec = strtok(copy, cs!("\x1f"));
    while !rec.is_null() {
        'record: {
            let mut a = strchr(rec, 0x1e);
            if a.is_null() {
                break 'record;
            }
            *a = 0;
            a = a.add(1);
            let mut b = strchr(a, 0x1e);
            if b.is_null() {
                break 'record;
            }
            *b = 0;
            b = b.add(1);
            add_mount(rec, a, *b == b'1' as c_char);
        }
        rec = strtok(ptr::null_mut(), cs!("\x1f"));
    }
    free(copy.cast());
}

unsafe fn serialize() -> *mut c_char {
    let count = g_n();
    let mut need: usize = 1;
    let mut i: c_int = 0;
    while i < count {
        need += strlen(mount_dst(i)) + strlen(mount_src(i)) + 5;
        i += 1;
    }
    let out = malloc(need).cast::<c_char>();
    if out.is_null() {
        return ptr::null_mut();
    }
    let mut at: usize = 0;
    i = 0;
    while i < count {
        at = at.wrapping_add(snprintf(
            out.add(at),
            need.wrapping_sub(at),
            cs!("%s\x1e%s\x1e%c\x1f"),
            mount_dst(i).cast_const(),
            mount_src(i).cast_const(),
            (if (*mount_at(i)).ro { b'1' } else { b'0' }) as c_int,
        ) as usize);
        i += 1;
    }
    out
}

// ------------------------------------------------------- the interpreter

// Read `--args FD`: NUL-separated arguments, appended to the vector.
unsafe fn read_args_fd(
    fd: c_int,
    vec: *mut *mut *mut c_char,
    cnt: *mut c_int,
    cap: *mut c_int,
) -> c_int {
    let mut sz: usize = 0;
    let mut capb: usize = 65536;
    let mut buf = malloc(capb).cast::<c_char>();
    if buf.is_null() {
        return -D_ENOMEM;
    }
    loop {
        if sz == capb {
            capb *= 2;
            let nb = realloc(buf.cast(), capb).cast::<c_char>();
            if nb.is_null() {
                free(buf.cast());
                return -D_ENOMEM;
            }
            buf = nb;
        }
        let r = read(fd, buf.add(sz).cast(), capb - sz);
        if r < 0 {
            if errno() == D_EINTR {
                continue;
            }
            let e = errno();
            free(buf.cast());
            return -e;
        }
        if r == 0 {
            break;
        }
        sz += r as usize;
    }
    let mut at: usize = 0;
    while at < sz {
        if *cnt + 1 >= *cap {
            *cap *= 2;
            *vec = realloc((*vec).cast(), *cap as usize * size_of::<*mut c_char>()).cast();
        }
        *(*vec).offset(*cnt as isize) = strdup(buf.add(at));
        *cnt += 1;
        at += strlen(buf.add(at)) + 1;
    }
    free(buf.cast());
    0
}

unsafe fn mkdirs(path: *const c_char, mode: ModeT) -> c_int {
    let mut tmp_buffer = stack_buffer::<PATH_MAX>();
    let tmp = tmp_buffer.as_mut_ptr().cast::<c_char>();
    snprintf(tmp, PATH_MAX, cs!("%s"), path);
    let mut p = tmp.add(1);
    while *p != 0 {
        if *p == b'/' as c_char {
            *p = 0;
            mkdir(tmp, mode);
            *p = b'/' as c_char;
        }
        p = p.add(1);
    }
    if mkdir(tmp, mode) == 0 || errno() == D_EEXIST {
        0
    } else {
        -1
    }
}

unsafe fn mkparents(path: *const c_char) -> c_int {
    let mut tmp_buffer = stack_buffer::<PATH_MAX>();
    let tmp = tmp_buffer.as_mut_ptr().cast::<c_char>();
    snprintf(tmp, PATH_MAX, cs!("%s"), path);
    let s = strrchr(tmp, b'/' as c_int);
    if s.is_null() || s == tmp {
        return 0;
    }
    *s = 0;
    mkdirs(tmp, 0o755)
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_bwrap_exec(
    argv0: *const *mut c_char,
    envp: *const *mut c_char,
    exec_guest: ExecGuest,
) -> c_long {
    const POINTER: usize = size_of::<*mut c_char>();

    // Copy the vector so --args can extend it.
    let mut cap: c_int = 64;
    let mut cnt: c_int = 0;
    let mut v = malloc(cap as usize * POINTER).cast::<*mut c_char>();
    let mut index: isize = 0;
    while !(*argv0.offset(index)).is_null() {
        if cnt + 1 >= cap {
            cap *= 2;
            v = realloc(v.cast(), cap as usize * POINTER).cast();
        }
        *v.offset(cnt as isize) = *argv0.offset(index);
        cnt += 1;
        index += 1;
    }
    *v.offset(cnt as isize) = ptr::null_mut();

    let mut sandbox_buffer = stack_buffer::<PATH_MAX>();
    let sandbox = sandbox_buffer.as_mut_ptr().cast::<c_char>();
    snprintf(
        sandbox,
        PATH_MAX,
        cs!("%s/lxrt-sandbox-XXXXXX"),
        lxrt_host_tmpdir(),
    );
    if mkdtemp(sandbox).is_null() {
        return lerr(errno());
    }
    if lxrt_trace_on() {
        fprintf(
            lxrt_trace_stream(),
            cs!("[lxrt] bwrap: sandbox root %s\n"),
            sandbox.cast_const(),
        );
    }

    // Environment edits (--setenv/--unsetenv) accumulate here.
    let mut ecap: c_int = 64;
    let mut ecnt: c_int = 0;
    let mut env = malloc(ecap as usize * POINTER).cast::<*mut c_char>();
    index = 0;
    while !envp.is_null() && !(*envp.offset(index)).is_null() {
        if ecnt + 1 >= ecap {
            ecap *= 2;
            env = realloc(env.cast(), ecap as usize * POINTER).cast();
        }
        *env.offset(ecnt as isize) = *envp.offset(index);
        ecnt += 1;
        index += 1;
    }
    *env.offset(ecnt as isize) = ptr::null_mut();
    let mut chdir_buffer = MaybeUninit::<[c_char; PATH_MAX]>::zeroed();
    let chdir_to = chdir_buffer.as_mut_ptr().cast::<c_char>();
    let mut perms: ModeT = 0;

    // The emulator's own files stay visible, as they do on Linux, where FEX
    // reaches its x86 rootfs and config through what it opened before bwrap
    // ran. Here every process re-reads them by path, so without these binds
    // an x86 program in the sandbox had no loader and no libc. Resolved in
    // the CALLER's view, before the plan's own binds enter the table.
    let mut emu_dst_buffer = MaybeUninit::<[[c_char; PATH_MAX]; 3]>::uninit();
    let mut emu_src_buffer = MaybeUninit::<[[c_char; PATH_MAX]; 3]>::uninit();
    let emu_dst = emu_dst_buffer.as_mut_ptr().cast::<c_char>();
    let emu_src = emu_src_buffer.as_mut_ptr().cast::<c_char>();
    let mut n_emu: usize = 0;
    {
        let mut cfg_buffer = MaybeUninit::<[c_char; PATH_MAX]>::zeroed();
        let cfg = cfg_buffer.as_mut_ptr().cast::<c_char>();
        let home = getenv(cs!("HOME"));
        if !home.is_null() && *home == b'/' as c_char {
            snprintf(cfg, PATH_MAX, cs!("%s/.fex-emu"), home.cast_const());
        }
        // /usr/lib/lxrt-emu: FEX and the aarch64 loader and libraries it is
        // linked against (PT_INTERP and DT_RPATH point there), the part of
        // the emulator that is ELF files rather than runtime.
        let emu: [*const c_char; 3] = [
            getenv(cs!("FEX_ROOTFS")),
            if *cfg != 0 { cfg.cast_const() } else { ptr::null() },
            cs!("/usr/lib/lxrt-emu"),
        ];
        let mut k: usize = 0;
        while k < 3 {
            let p = *emu.as_ptr().add(k);
            k += 1;
            if p.is_null() || *p != b'/' as c_char || *p.add(1) == 0 {
                continue; // "/" is no overlay: nothing to keep
            }
            let mut host_buffer = stack_buffer::<PATH_MAX>();
            let host = host_buffer.as_mut_ptr().cast::<c_char>();
            let h = guest_to_host(p, host, PATH_MAX);
            let mut st = MaybeUninit::<Stat>::uninit();
            if h.is_null() || stat(h, st.as_mut_ptr()) != 0 || !s_isdir((*st.as_ptr()).st_mode) {
                continue;
            }
            snprintf(emu_dst.add(n_emu * PATH_MAX), PATH_MAX, cs!("%s"), p);
            snprintf(emu_src.add(n_emu * PATH_MAX), PATH_MAX, cs!("%s"), h);
            n_emu += 1;
        }
    }

    let mut i: c_int = 1;
    'options: while i < cnt {
        // `break 'option` is the C loop's `continue` (the i++ below still
        // runs); `break 'options` is its `break`.
        'option: {
            let o: *const c_char = *v.offset(i as isize);
            // v[i + k], read only after NEED(k) has checked the bound.
            macro_rules! arg {
                ($k:expr) => {
                    (*v.offset((i + $k) as isize)).cast_const()
                };
            }
            macro_rules! need {
                ($k:expr) => {
                    if i + $k >= cnt {
                        fprintf(
                            lxrt_trace_stream(),
                            cs!("[lxrt] bwrap: %s needs %d argument(s)\n"),
                            o,
                            $k as c_int,
                        );
                        return lerr(D_EINVAL);
                    }
                };
            }
            macro_rules! is {
                ($text:expr) => {
                    streq(o, cs!($text))
                };
            }
            if is!("--") {
                i += 1;
                break 'options;
            }
            if *o != b'-' as c_char || *o.add(1) != b'-' as c_char {
                break 'options; // the command starts
            }
            if is!("--version") {
                // pressure-vessel probes its bwrap before using it. Say which
                // bwrap this stands in for: the plan semantics are those of 0.10.
                static MSG: [u8; 37] = *b"bubblewrap 0.10.0 (lxrt interpreter)\n";
                let _ = write(1, MSG.as_ptr().cast(), MSG.len());
                _exit(0);
            }
            if is!("--args") {
                need!(1);
                let fd = atoi(arg!(1));
                // Splice the fd's arguments in place of these two.
                let mut extra = malloc(16 * POINTER).cast::<*mut c_char>();
                let mut ec: c_int = 0;
                let mut ecap2: c_int = 16;
                let r = read_args_fd(fd, &mut extra, &mut ec, &mut ecap2);
                if r < 0 {
                    return lerr(-r);
                }
                let tail = cnt - (i + 2);
                let nv = malloc((cnt - 2 + ec + 1) as usize * POINTER).cast::<*mut c_char>();
                memcpy(nv.cast(), v.cast(), i as usize * POINTER);
                memcpy(
                    nv.offset(i as isize).cast(),
                    extra.cast(),
                    ec as usize * POINTER,
                );
                memcpy(
                    nv.offset((i + ec) as isize).cast(),
                    v.offset((i + 2) as isize).cast(),
                    tail as usize * POINTER,
                );
                cnt = i + ec + tail;
                *nv.offset(cnt as isize) = ptr::null_mut();
                v = nv;
                close(fd);
                i -= 1; // re-examine at i
                break 'option;
            }
            if is!("--ro-bind")
                || is!("--bind")
                || is!("--dev-bind")
                || is!("--ro-bind-try")
                || is!("--bind-try")
                || is!("--dev-bind-try")
            {
                need!(2);
                let src = arg!(1);
                let dst = arg!(2);
                // /proc, /sys and /dev onto themselves: those trees are the
                // runtime's own (procfs.c, proc_ext.c, the /dev passthrough) in
                // every process, sandboxed
                // or not, so the bind is already in effect. check-requirements
                // binds both before running `true`.
                if (streq(src, cs!("/proc")) && streq(dst, cs!("/proc")))
                    || (streq(src, cs!("/sys")) && streq(dst, cs!("/sys")))
                    || (streq(src, cs!("/dev")) && streq(dst, cs!("/dev")))
                {
                    i += 2;
                    break 'option;
                }
                let mut host_buffer = stack_buffer::<PATH_MAX>();
                let host = host_buffer.as_mut_ptr().cast::<c_char>();
                let h = guest_to_host(src, host, PATH_MAX);
                let mut st = MaybeUninit::<Stat>::uninit();
                if h.is_null() || stat(h, st.as_mut_ptr()) != 0 {
                    if !strstr(o, cs!("-try")).is_null() {
                        i += 2;
                        break 'option;
                    }
                    fprintf(
                        lxrt_trace_stream(),
                        cs!("[lxrt] bwrap: %s %s: source missing (%s)\n"),
                        o,
                        src,
                        if !h.is_null() { h } else { src },
                    );
                    return lerr(D_ENOENT);
                }
                // The destination has to exist in the sandbox root for a guest
                // that stats the parent directory; the bind itself is a table entry.
                let mut in_root_buffer = stack_buffer::<PATH_MAX>();
                let in_root = in_root_buffer.as_mut_ptr().cast::<c_char>();
                snprintf(in_root, PATH_MAX, cs!("%s%s"), sandbox.cast_const(), dst);
                if s_isdir((*st.as_ptr()).st_mode) {
                    mkdirs(in_root, 0o755);
                } else {
                    mkparents(in_root);
                    let fd = open(in_root, D_O_CREAT | D_O_WRONLY, 0o644 as c_int);
                    if fd >= 0 {
                        close(fd);
                    }
                }
                add_mount(dst, h, strncmp(o, cs!("--ro"), 4) == 0);
                i += 2;
                break 'option;
            }
            if is!("--symlink") {
                need!(2);
                let mut in_root_buffer = stack_buffer::<PATH_MAX>();
                let in_root = in_root_buffer.as_mut_ptr().cast::<c_char>();
                snprintf(in_root, PATH_MAX, cs!("%s%s"), sandbox.cast_const(), arg!(2));
                mkparents(in_root);
                unlink(in_root);
                if symlink(arg!(1), in_root) != 0 && errno() != D_EEXIST {
                    return lerr(errno());
                }
                i += 2;
                break 'option;
            }
            if is!("--tmpfs") || is!("--dir") {
                need!(1);
                let mut in_root_buffer = stack_buffer::<PATH_MAX>();
                let in_root = in_root_buffer.as_mut_ptr().cast::<c_char>();
                snprintf(in_root, PATH_MAX, cs!("%s%s"), sandbox.cast_const(), arg!(1));
                if mkdirs(in_root, if perms != 0 { perms } else { 0o755 }) != 0 {
                    return lerr(errno());
                }
                perms = 0;
                i += 1;
                break 'option;
            }
            if is!("--proc") || is!("--dev") {
                need!(1);
                let mut in_root_buffer = stack_buffer::<PATH_MAX>();
                let in_root = in_root_buffer.as_mut_ptr().cast::<c_char>();
                snprintf(in_root, PATH_MAX, cs!("%s%s"), sandbox.cast_const(), arg!(1));
                mkdirs(in_root, 0o755);
                if is!("--dev") {
                    add_mount(arg!(1), cs!("/dev"), false);
                }
                // /proc is synthesised by procfs.c wherever it is asked for.
                i += 1;
                break 'option;
            }
            if is!("--ro-bind-data") || is!("--bind-data") || is!("--file") {
                need!(2);
                let fd = atoi(arg!(1));
                let mut in_root_buffer = stack_buffer::<PATH_MAX>();
                let in_root = in_root_buffer.as_mut_ptr().cast::<c_char>();
                snprintf(in_root, PATH_MAX, cs!("%s%s"), sandbox.cast_const(), arg!(2));
                mkparents(in_root);
                let out = open(
                    in_root,
                    D_O_CREAT | D_O_WRONLY | D_O_TRUNC,
                    (if perms != 0 { perms } else { 0o644 }) as c_int,
                );
                perms = 0;
                if out < 0 {
                    return lerr(errno());
                }
                let mut data = stack_buffer::<65536>();
                let buf = data.as_mut_ptr().cast::<c_void>();
                loop {
                    let r = read(fd, buf, 65536);
                    if r <= 0 {
                        break;
                    }
                    write(out, buf, r as usize);
                }
                close(out);
                close(fd);
                i += 2;
                break 'option;
            }
            if is!("--setenv") {
                need!(2);
                let l = strlen(arg!(1));
                let kv = malloc(l + strlen(arg!(2)) + 2).cast::<c_char>();
                sprintf(kv, cs!("%s=%s"), arg!(1), arg!(2));
                let mut replaced = false;
                let mut k: c_int = 0;
                while k < ecnt {
                    let slot = env.offset(k as isize);
                    if strncmp(*slot, arg!(1), l) == 0 && *(*slot).add(l) == b'=' as c_char {
                        *slot = kv;
                        replaced = true;
                        break;
                    }
                    k += 1;
                }
                if !replaced {
                    if ecnt + 1 >= ecap {
                        ecap *= 2;
                        env = realloc(env.cast(), ecap as usize * POINTER).cast();
                    }
                    *env.offset(ecnt as isize) = kv;
                    ecnt += 1;
                    *env.offset(ecnt as isize) = ptr::null_mut();
                }
                i += 2;
                break 'option;
            }
            if is!("--unsetenv") {
                need!(1);
                let l = strlen(arg!(1));
                let mut k: c_int = 0;
                while k < ecnt {
                    let slot = env.offset(k as isize);
                    if strncmp(*slot, arg!(1), l) == 0 && *(*slot).add(l) == b'=' as c_char {
                        ecnt -= 1;
                        *slot = *env.offset(ecnt as isize);
                        *env.offset(ecnt as isize) = ptr::null_mut();
                        break;
                    }
                    k += 1;
                }
                i += 1;
                break 'option;
            }
            if is!("--chdir") {
                need!(1);
                snprintf(chdir_to, PATH_MAX, cs!("%s"), arg!(1));
                i += 1;
                break 'option;
            }
            if is!("--perms") {
                need!(1);
                perms = strtol(arg!(1), ptr::null_mut(), 8) as ModeT;
                i += 1;
                break 'option;
            }
            if is!("--chmod") {
                // bwrap: --chmod OCTAL PATH (two arguments), on a path the plan
                // already made. Only what the plan created in the sandbox is
                // changed: a bind's placeholder is, its host source is not.
                need!(2);
                let mut in_root_buffer = stack_buffer::<PATH_MAX>();
                let in_root = in_root_buffer.as_mut_ptr().cast::<c_char>();
                snprintf(in_root, PATH_MAX, cs!("%s%s"), sandbox.cast_const(), arg!(2));
                if chmod(in_root, strtol(arg!(1), ptr::null_mut(), 8) as ModeT) != 0 {
                    fprintf(
                        lxrt_trace_stream(),
                        cs!("[lxrt] bwrap: --chmod %s %s: %s\n"),
                        arg!(1),
                        arg!(2),
                        strerror(errno()),
                    );
                }
                i += 2;
                break 'option;
            }
            if is!("--sync-fd")
                || is!("--info-fd")
                || is!("--json-status-fd")
                || is!("--block-fd")
                || is!("--userns-block-fd")
                || is!("--lock-file")
                || is!("--remount-ro")
                || is!("--exec-label")
                || is!("--file-label")
                || is!("--hostname")
                || is!("--argv0")
            {
                // fds that bwrap would hold or signal on: nothing to hold here.
                need!(1);
                i += 1;
                break 'option;
            }
            let unshare = strncmp(o, cs!("--unshare-"), 10) == 0;
            if is!("--new-session")
                || is!("--die-with-parent")
                || is!("--level-prefix")
                || is!("--not-a-security-boundary")
                || is!("--as-pid-1")
                || is!("--clearenv")
                || is!("--disable-userns")
                || is!("--assert-userns-disabled")
                || is!("--unshare-all")
                || unshare
                || is!("--cap-add")
                || is!("--cap-drop")
                || is!("--uid")
                || is!("--gid")
                || is!("--seccomp")
                || is!("--add-seccomp-fd")
            {
                let takes_argument = is!("--cap-add")
                    || is!("--cap-drop")
                    || is!("--uid")
                    || is!("--gid")
                    || is!("--seccomp")
                    || is!("--add-seccomp-fd");
                let refuse = unshare || takes_argument;
                if refuse {
                    // The evidence says pressure-vessel never asks for these. If a
                    // future one does, say so rather than pretend: a sandbox that
                    // silently is not one is worse than no sandbox.
                    fprintf(
                        lxrt_trace_stream(),
                        cs!("[lxrt] bwrap: %s is not implementable on Darwin (no namespaces/capabilities)\n"),
                        o,
                    );
                    return lerr(D_ENOSYS);
                }
                if takes_argument {
                    i += 1;
                }
                break 'option;
            }
            fprintf(
                lxrt_trace_stream(),
                cs!("[lxrt] bwrap: unknown option %s\n"),
                o,
            );
            return lerr(D_EINVAL);
        }
        i += 1;
    }
    if i >= cnt {
        fprintf(lxrt_trace_stream(), cs!("[lxrt] bwrap: no command\n"));
        return lerr(D_EINVAL);
    }

    // (emulator binds: resolved before the plan, applied after it -- below)
    let mut k: usize = 0;
    while k < n_emu {
        let dst = emu_dst.add(k * PATH_MAX).cast_const();
        let src = emu_src.add(k * PATH_MAX).cast_const();
        let mut in_root_buffer = stack_buffer::<PATH_MAX>();
        let in_root = in_root_buffer.as_mut_ptr().cast::<c_char>();
        snprintf(in_root, PATH_MAX, cs!("%s%s"), sandbox.cast_const(), dst);
        mkdirs(in_root, 0o755);
        add_mount(dst, src, true);
        if lxrt_trace_on() {
            fprintf(
                lxrt_trace_stream(),
                cs!("[lxrt] bwrap: emulator bind %s -> %s\n"),
                dst,
                src,
            );
        }
        k += 1;
    }

    // The child's view: the sandbox directory is its root, the binds its table.
    let ser = serialize();
    let ser_text: *const c_char = if !ser.is_null() { ser } else { cs!("") };
    let mut rootvar_buffer = stack_buffer::<{ PATH_MAX + 16 }>();
    let rootvar = rootvar_buffer.as_mut_ptr().cast::<c_char>();
    snprintf(
        rootvar,
        PATH_MAX + 16,
        cs!("LXRT_ROOT=%s"),
        sandbox.cast_const(),
    );
    let ml = strlen(ser_text) + 16;
    let mounts_kv = malloc(ml).cast::<c_char>();
    snprintf(mounts_kv, ml, cs!("LXRT_MOUNTS=%s"), ser_text);
    let mut set_root = false;
    let mut set_mounts = false;
    let mut e: c_int = 0;
    while e < ecnt {
        let slot = env.offset(e as isize);
        if strncmp(*slot, cs!("LXRT_ROOT="), 10) == 0 {
            *slot = rootvar;
            set_root = true;
        }
        if strncmp(*slot, cs!("LXRT_MOUNTS="), 12) == 0 {
            *slot = mounts_kv;
            set_mounts = true;
        }
        e += 1;
    }
    if ecnt + 2 >= ecap {
        ecap += 4;
        env = realloc(env.cast(), ecap as usize * POINTER).cast();
    }
    if !set_root {
        *env.offset(ecnt as isize) = rootvar;
        ecnt += 1;
    }
    if !set_mounts {
        *env.offset(ecnt as isize) = mounts_kv;
        ecnt += 1;
    }
    *env.offset(ecnt as isize) = ptr::null_mut();
    if *chdir_to != 0 {
        let mut host_buffer = stack_buffer::<PATH_MAX>();
        let host = host_buffer.as_mut_ptr().cast::<c_char>();
        // The cwd is a host notion; take it through the child's own view.
        let mut h = lxrt_mounts_translate(chdir_to, host, PATH_MAX);
        if h.is_null() {
            snprintf(
                host,
                PATH_MAX,
                cs!("%s%s"),
                sandbox.cast_const(),
                chdir_to.cast_const(),
            );
            h = host;
        }
        if chdir(h) != 0 {
            fprintf(
                lxrt_trace_stream(),
                cs!("[lxrt] bwrap: chdir %s: %s\n"),
                chdir_to.cast_const(),
                strerror(errno()),
            );
        }
    }
    // bwrap runs its command with execvp: a bare name is looked up in PATH
    // (steam-runtime-check-requirements runs plain `true`). Resolved in the
    // caller's view; the binds that matter (/usr) carry it into the child's.
    let mut cmd: *const c_char = *v.offset(i as isize);
    static mut FOUND: MaybeUninit<[c_char; PATH_MAX]> = MaybeUninit::zeroed();
    if strchr(cmd, b'/' as c_int).is_null() {
        let mut pathv: *const c_char =
            cs!("/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
        let mut e: c_int = 0;
        while e < ecnt {
            let entry = *env.offset(e as isize);
            if strncmp(entry, cs!("PATH="), 5) == 0 {
                pathv = entry.add(5);
                break;
            }
            e += 1;
        }
        let mut d = pathv;
        while *d != 0 {
            let end: *const c_char = strchr(d, b':' as c_int);
            let dl = if !end.is_null() {
                end.offset_from(d) as usize
            } else {
                strlen(d)
            };
            let mut cand_buffer = stack_buffer::<PATH_MAX>();
            let cand = cand_buffer.as_mut_ptr().cast::<c_char>();
            let mut host_buffer = stack_buffer::<PATH_MAX>();
            let host = host_buffer.as_mut_ptr().cast::<c_char>();
            if dl != 0 && dl + strlen(cmd) + 2 < PATH_MAX {
                snprintf(cand, PATH_MAX, cs!("%.*s/%s"), dl as c_int, d, cmd);
                let mut st = MaybeUninit::<Stat>::uninit();
                let mut ov_buffer = stack_buffer::<PATH_MAX>();
                let ov = ov_buffer.as_mut_ptr().cast::<c_char>();
                let rootfs = getenv(cs!("FEX_ROOTFS"));
                let mut in_rootfs = false;
                if !rootfs.is_null() && *rootfs == b'/' as c_char {
                    let mut rc_buffer = stack_buffer::<PATH_MAX>();
                    let rc = rc_buffer.as_mut_ptr().cast::<c_char>();
                    snprintf(
                        rc,
                        PATH_MAX,
                        cs!("%s%s"),
                        rootfs.cast_const(),
                        cand.cast_const(),
                    );
                    in_rootfs = stat(guest_to_host(rc, ov, PATH_MAX), st.as_mut_ptr()) == 0
                        && s_isreg((*st.as_ptr()).st_mode);
                }
                if in_rootfs
                    || (stat(guest_to_host(cand, host, PATH_MAX), st.as_mut_ptr()) == 0
                        && s_isreg((*st.as_ptr()).st_mode))
                {
                    let found = ptr::addr_of_mut!(FOUND).cast::<c_char>();
                    snprintf(found, PATH_MAX, cs!("%s"), cand.cast_const());
                    cmd = found;
                    break;
                }
            }
            if end.is_null() {
                break;
            }
            d = end.add(1);
        }
    }
    if lxrt_trace_on() {
        fprintf(
            lxrt_trace_stream(),
            cs!("[lxrt] bwrap: %d mounts, exec %s\n"),
            g_n(),
            cmd,
        );
    }
    exec_guest(cmd, v.offset(i as isize), env)
}

// A bind SOURCE is a path in the parent's view: through its own table first,
// then its root, then the host.
unsafe fn guest_to_host(path: *const c_char, buf: *mut c_char, n: usize) -> *const c_char {
    let m = lxrt_mounts_translate(path, buf, n);
    if !m.is_null() {
        return m;
    }
    let root = getenv(cs!("LXRT_ROOT"));
    if !root.is_null() && *root != 0 && *path == b'/' as c_char {
        let mut cand_buffer = stack_buffer::<PATH_MAX>();
        let cand = cand_buffer.as_mut_ptr().cast::<c_char>();
        snprintf(cand, PATH_MAX, cs!("%s%s"), root.cast_const(), path);
        let mut st = MaybeUninit::<Stat>::uninit();
        if stat(cand, st.as_mut_ptr()) == 0 {
            snprintf(buf, n, cs!("%s"), cand.cast_const());
            return buf;
        }
    }
    snprintf(buf, n, cs!("%s"), path);
    buf
}
