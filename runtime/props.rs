//! Android property paths exposed to lxrun guests.
//!
//! Bionic reads the property areas through `/dev/__properties__` and writes
//! through `/dev/socket/property_service`. This module maps those guest paths
//! to a private host directory and starts the existing property service on
//! demand. The service implementation remains in propsvc.c.

use std::ffi::{c_char, c_int, c_void, CStr, CString, OsStr, OsString};
use std::fs::{self, File, OpenOptions};
use std::mem::MaybeUninit;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::{DirBuilderExt, FileTypeExt, MetadataExt, OpenOptionsExt};
use std::os::unix::io::AsRawFd;
use std::path::Path;
use std::process::{Command, Stdio};
use std::ptr;
use std::sync::{Mutex, MutexGuard, Once};
use std::thread;
use std::time::Duration;

const F_GETPATH: c_int = 50;
const LOCK_SH: c_int = 1;
const LOCK_EX: c_int = 2;
const LOCK_NB: c_int = 4;
const EWOULDBLOCK: i32 = 35;
const PATH_MAX: usize = 1024;

struct State {
    offered: c_int,
    areas_tried: bool,
    dir: Option<CString>,
    areas: Option<CString>,
    root: Option<CString>,
    dev: u64,
}

impl Default for State {
    fn default() -> Self {
        Self {
            offered: 0,
            areas_tried: false,
            dir: None,
            areas: None,
            root: None,
            dev: 0,
        }
    }
}

static STATE: Mutex<State> = Mutex::new(State {
    offered: 0,
    areas_tried: false,
    dir: None,
    areas: None,
    root: None,
    dev: 0,
});
static FORK_HOOKS: Once = Once::new();
static mut FORK_GUARD: MaybeUninit<MutexGuard<'static, State>> = MaybeUninit::uninit();

#[repr(C)]
struct StatPrefix {
    dev: i32,
    mode: u16,
    nlink: u16,
    ino: u64,
    uid: u32,
    gid: u32,
}

#[link(name = "System")]
unsafe extern "C" {
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn flock(fd: c_int, operation: c_int) -> c_int;
    fn realpath(path: *const c_char, resolved: *mut c_char) -> *mut c_char;
    fn getuid() -> u32;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
}

fn lock_state() -> MutexGuard<'static, State> {
    STATE
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

extern "C" fn fork_prepare() {
    let guard = lock_state();
    unsafe {
        ptr::addr_of_mut!(FORK_GUARD)
            .cast::<MutexGuard<'static, State>>()
            .write(guard);
    }
}

extern "C" fn fork_release() {
    unsafe {
        ptr::drop_in_place(ptr::addr_of_mut!(FORK_GUARD).cast::<MutexGuard<'static, State>>());
    }
}

extern "C" fn register_fork_hooks() {
    FORK_HOOKS.call_once(|| unsafe {
        let _ = pthread_atfork(Some(fork_prepare), Some(fork_release), Some(fork_release));
    });
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FORK_HOOKS_INIT: extern "C" fn() = register_fork_hooks;

fn trace(format: &'static [u8], message: &CStr) {
    unsafe {
        let _ = fprintf(
            lxrt_trace_stream(),
            format.as_ptr().cast(),
            message.as_ptr(),
        );
    }
}

fn real_path(path: &[u8]) -> Option<CString> {
    let input = CString::new(path).ok()?;
    let mut output = vec![0 as c_char; PATH_MAX * 4];
    let result = unsafe { realpath(input.as_ptr(), output.as_mut_ptr()) };
    if result.is_null() {
        return None;
    }
    Some(unsafe { CStr::from_ptr(output.as_ptr()) }.to_owned())
}

fn joined_path(base: &[u8], suffix: &[u8]) -> Option<CString> {
    let mut bytes = Vec::new();
    bytes
        .try_reserve(base.len().saturating_add(suffix.len()).saturating_add(1))
        .ok()?;
    bytes.extend_from_slice(base);
    if !base.ends_with(b"/") && !suffix.starts_with(b"/") {
        bytes.push(b'/');
    }
    bytes.extend_from_slice(suffix);
    CString::new(bytes).ok()
}

fn os_path(bytes: &[u8]) -> &Path {
    Path::new(OsStr::from_bytes(bytes))
}

fn env_bytes(name: &str) -> Option<Vec<u8>> {
    std::env::var_os(name).map(|value| value.as_os_str().as_bytes().to_vec())
}

fn offered_locked(state: &mut State) -> bool {
    if state.offered != 0 {
        return state.offered > 0;
    }
    state.offered = -1;

    let Some(root) = env_bytes("LXRT_ROOT") else {
        return false;
    };
    if root.is_empty() || env_bytes("LXRT_PROPERTY_SERVICE").as_deref() == Some(b"0") {
        return false;
    }
    let Some(root) = real_path(&root) else {
        return false;
    };
    if root.as_bytes().len() >= PATH_MAX {
        return false;
    }
    let Some(probe) = joined_path(
        root.as_bytes(),
        b"system/etc/selinux/plat_property_contexts",
    ) else {
        return false;
    };
    if File::open(os_path(probe.as_bytes())).is_err() {
        return false;
    }

    let requested_dir = env_bytes("LXRT_PROPERTY_DIR").filter(|value| !value.is_empty());
    let dir = requested_dir.unwrap_or_else(|| {
        let hash = root
            .as_bytes()
            .iter()
            .fold(0xcbf29ce484222325u64, |hash, byte| {
                (hash ^ u64::from(*byte)).wrapping_mul(0x100000001b3)
            });
        format!("/tmp/lxrt-props-{}-{hash:016x}", unsafe { getuid() }).into_bytes()
    });
    if dir.len() >= 256 {
        return false;
    }
    let Ok(dir_c) = CString::new(dir) else {
        return false;
    };

    // Match mkdir(0700): an existing directory is fine, then lstat and the
    // ownership/mode check decide whether this process may use it.
    let _ = fs::DirBuilder::new()
        .mode(0o700)
        .create(os_path(dir_c.as_bytes()));
    let Ok(meta) = fs::symlink_metadata(os_path(dir_c.as_bytes())) else {
        return false;
    };
    if !meta.is_dir() || meta.uid() != unsafe { getuid() } || (meta.mode() & 0o077) != 0 {
        let message = CString::new(dir_c.as_bytes()).unwrap_or_default();
        trace(
            b"[lxrt] properties: %s is not a private directory of this user; no property service\n\0",
            &message,
        );
        return false;
    }

    let Some(canonical_dir) = real_path(dir_c.as_bytes()) else {
        return false;
    };
    if canonical_dir.as_bytes().len() >= 256 {
        return false;
    }
    let Some(areas) = joined_path(canonical_dir.as_bytes(), b"__properties__") else {
        return false;
    };

    state.dev = meta.dev();
    state.root = Some(root);
    state.dir = Some(canonical_dir);
    state.areas = Some(areas);
    state.offered = 1;
    true
}

// 0: no service, 1: serving, -1: a service is starting or leaving.
fn service_state(state: &State) -> c_int {
    let Some(dir) = state.dir.as_ref() else {
        return 0;
    };
    let Some(lock_path) = joined_path(dir.as_bytes(), b"service.lock") else {
        return 0;
    };
    let Ok(file) = File::open(os_path(lock_path.as_bytes())) else {
        return 0;
    };
    let locked = unsafe { flock(file.as_raw_fd(), LOCK_SH | LOCK_NB) } != 0
        && std::io::Error::last_os_error().raw_os_error() == Some(EWOULDBLOCK);
    drop(file);
    if !locked {
        return 0;
    }
    let Some(socket_path) = joined_path(dir.as_bytes(), b"property_service") else {
        return -1;
    };
    match fs::metadata(os_path(socket_path.as_bytes())) {
        Ok(meta) if meta.file_type().is_socket() => 1,
        _ => -1,
    }
}

fn socket_answers(state: &State) -> bool {
    for _ in 0..1000 {
        let status = service_state(state);
        if status >= 0 {
            return status == 1;
        }
        thread::sleep(Duration::from_millis(10));
    }
    false
}

fn areas_exist(state: &State) -> bool {
    let Some(areas) = state.areas.as_ref() else {
        return false;
    };
    let Some(info) = joined_path(areas.as_bytes(), b"property_info") else {
        return false;
    };
    File::open(os_path(info.as_bytes())).is_ok()
}

fn spawn_locked(state: &State, for_socket: bool) -> bool {
    let Some(dir) = state.dir.as_ref() else {
        return false;
    };
    let Some(lock_path) = joined_path(dir.as_bytes(), b"spawn.lock") else {
        return false;
    };
    let lock = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .mode(0o600)
        .open(os_path(lock_path.as_bytes()))
        .ok();
    if let Some(file) = &lock {
        unsafe {
            let _ = flock(file.as_raw_fd(), LOCK_EX);
        }
    }

    let mut ready = if for_socket {
        socket_answers(state)
    } else {
        areas_exist(state)
    };
    if !ready {
        let exe = std::env::current_exe();
        let spawn_result = exe.map(|exe| {
            let mut command = Command::new(exe);
            let root = state.root.as_ref().expect("offered root");
            let dir = state.dir.as_ref().expect("offered directory");
            command
                .arg("--property-service")
                .arg(OsString::from_vec(dir.as_bytes().to_vec()))
                .arg(OsString::from_vec(root.as_bytes().to_vec()))
                .arg("--daemon")
                .env_clear()
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null());
            for (key, value) in std::env::vars_os() {
                if key.as_os_str().as_bytes().starts_with(b"LXRT_PROPERTY_") {
                    command.env(key, value);
                }
            }
            command.status()
        });
        match spawn_result {
            Ok(Ok(_)) => {}
            Ok(Err(error)) | Err(error) => {
                if unsafe { lxrt_trace_on() } {
                    let message = CString::new(error.to_string()).unwrap_or_default();
                    trace(
                        b"[lxrt] properties: cannot start the service: %s\n\0",
                        &message,
                    );
                }
            }
        }
        ready = if for_socket {
            socket_answers(state)
        } else {
            areas_exist(state)
        };
        if !ready {
            let message = CString::new(dir.as_bytes()).unwrap_or_default();
            trace(
                b"[lxrt] properties: the service did not start (%s/service.log)\n\0",
                &message,
            );
        }
    }
    drop(lock);
    ready
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lxrt_props_translate(
    path: *const c_char,
    buf: *mut c_char,
    n: usize,
) -> *const c_char {
    if path.is_null() || buf.is_null() || n == 0 {
        return ptr::null();
    }
    let guest = CStr::from_ptr(path).to_bytes();
    if !guest.starts_with(b"/dev/") {
        return ptr::null();
    }
    let areas = guest.starts_with(b"/dev/__properties__")
        && (guest.len() == 19 || guest.get(19) == Some(&b'/'));
    let socket = guest == b"/dev/socket/property_service";
    if !areas && !socket {
        return ptr::null();
    }

    register_fork_hooks();
    let mut state = lock_state();
    let offered = offered_locked(&mut state);
    if offered && areas && !state.areas_tried {
        state.areas_tried = true;
        if !areas_exist(&state) {
            spawn_locked(&state, false);
        }
    }
    if !offered {
        return ptr::null();
    }

    let mut translated = Vec::new();
    if areas {
        let Some(area_path) = state.areas.as_ref() else {
            return ptr::null();
        };
        translated.extend_from_slice(area_path.as_bytes());
        translated.extend_from_slice(&guest[19..]);
    } else if let Some(dir) = state.dir.as_ref() {
        translated.extend_from_slice(dir.as_bytes());
        translated.extend_from_slice(b"/property_service");
    }
    if translated.len() >= n {
        return ptr::null();
    }
    ptr::copy_nonoverlapping(translated.as_ptr(), buf.cast::<u8>(), translated.len());
    *buf.add(translated.len()) = 0;
    buf
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lxrt_props_is_socket(guest_path: *const c_char) -> bool {
    !guest_path.is_null()
        && CStr::from_ptr(guest_path).to_bytes() == b"/dev/socket/property_service"
}

#[unsafe(no_mangle)]
pub extern "C" fn lxrt_props_start_service() -> bool {
    register_fork_hooks();
    let mut state = lock_state();
    offered_locked(&mut state) && spawn_locked(&state, true)
}

fn under(path: &[u8], dir: &[u8]) -> bool {
    path.starts_with(dir) && (path.len() == dir.len() || path.get(dir.len()) == Some(&b'/'))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lxrt_props_fix_stat(host: *const c_char, fd: c_int, st: *mut c_void) {
    if st.is_null() {
        return;
    }
    let state = lock_state();
    if state.offered <= 0 {
        return;
    }
    let stat = &mut *st.cast::<StatPrefix>();
    if stat.dev as u32 as u64 != state.dev {
        return;
    }

    let mut fd_path = vec![0 as c_char; PATH_MAX * 4];
    let path = if host.is_null() {
        if fd < 0 || fcntl(fd, F_GETPATH, fd_path.as_mut_ptr()) != 0 {
            return;
        }
        CStr::from_ptr(fd_path.as_ptr()).to_bytes()
    } else {
        CStr::from_ptr(host).to_bytes()
    };
    let Some(areas) = state.areas.as_ref() else {
        return;
    };
    if under(path, areas.as_bytes()) {
        stat.uid = 0;
        stat.gid = 0;
    }
}
