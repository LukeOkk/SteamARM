//! Short host aliases for guest Unix socket paths that exceed Darwin's limit.
//!
//! Pressure-vessel can map a short guest path into a much longer host path.
//! Darwin caps `sockaddr_un.sun_path` at 104 bytes, so this module gives those
//! sockets a private, deterministic pathname under the host temporary dir.
//! A sidecar preserves the original guest name for getsockname/getpeername.

use std::ffi::{c_char, c_int, CStr, CString, OsStr};
use std::fs::{self, DirBuilder, OpenOptions};
use std::io::Write;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::ptr;

const DARWIN_SUN_PATH: usize = 104;
const GUEST_SUN_PATH: usize = 108;
const HASH_BYTES: usize = 32;
const MAX_COLLISIONS: u8 = 32;
const DARWIN_EFAULT: c_int = 14;
const DARWIN_EINVAL: c_int = 22;
const DARWIN_ENOENT: c_int = 2;
const DARWIN_ENAMETOOLONG: c_int = 63;

#[link(name = "System")]
unsafe extern "C" {
    fn getuid() -> u32;
    fn lxrt_host_tmpdir() -> *const c_char;
}

unsafe extern "C" {
    fn lxrt_translate_guest_path(
        path: *const c_char,
        output: *mut c_char,
        capacity: usize,
    ) -> *const c_char;
}

fn host_tmpdir() -> Option<PathBuf> {
    let value = unsafe { lxrt_host_tmpdir() };
    if value.is_null() {
        return None;
    }
    let bytes = unsafe { CStr::from_ptr(value) }.to_bytes();
    (!bytes.is_empty()).then(|| PathBuf::from(OsStr::from_bytes(bytes)))
}

fn socket_directory() -> Option<PathBuf> {
    let uid = unsafe { getuid() };
    let suffix = format!("lxrt-uds-{uid}");
    let mut candidates = Vec::with_capacity(2);
    if let Some(tmp) = host_tmpdir() {
        candidates.push(tmp.join(&suffix));
    }
    let fallback = PathBuf::from("/tmp").join(suffix);
    if !candidates.iter().any(|candidate| candidate == &fallback) {
        candidates.push(fallback);
    }

    candidates.into_iter().find(|directory| {
        directory.as_os_str().as_bytes().len() + 1 + HASH_BYTES + 1 < DARWIN_SUN_PATH
            && ensure_private_directory(directory, uid)
    })
}

fn ensure_private_directory(directory: &Path, uid: u32) -> bool {
    let _ = DirBuilder::new().mode(0o700).create(directory);
    let Ok(metadata) = fs::symlink_metadata(directory) else {
        return false;
    };
    if !metadata.is_dir() || metadata.file_type().is_symlink() || metadata.uid() != uid {
        return false;
    }
    if metadata.mode() & 0o077 != 0
        && fs::set_permissions(directory, fs::Permissions::from_mode(0o700)).is_err()
    {
        return false;
    }
    true
}

fn hash_path(path: &[u8], salt: u8) -> [u64; 2] {
    let mut first = 0xcbf29ce484222325u64 ^ u64::from(salt);
    let mut second = 0x84222325cbf29ce4u64 ^ (u64::from(salt) << 32);
    for byte in path {
        first = (first ^ u64::from(*byte)).wrapping_mul(0x100000001b3);
        second = (second ^ u64::from(*byte).wrapping_add(0x9d)).wrapping_mul(0x100000001b3);
    }
    [first, second]
}

fn socket_name(path: &[u8], salt: u8) -> String {
    let hash = hash_path(path, salt);
    format!("s{:016x}{:016x}", hash[0], hash[1])
}

fn mapping_path(socket_path: &Path) -> PathBuf {
    let mut bytes = socket_path.as_os_str().as_bytes().to_vec();
    bytes.extend_from_slice(b".name");
    PathBuf::from(OsStr::from_bytes(&bytes))
}

fn mapping_record(host_path: &[u8], guest_path: &[u8]) -> Option<Vec<u8>> {
    if host_path.contains(&0) || guest_path.contains(&0) || guest_path.len() >= GUEST_SUN_PATH {
        return None;
    }
    let mut record = Vec::with_capacity(host_path.len() + guest_path.len() + 1);
    record.extend_from_slice(host_path);
    record.push(0);
    record.extend_from_slice(guest_path);
    Some(record)
}

fn write_mapping(path: &Path, record: &[u8]) -> bool {
    let Ok(mut file) = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(path)
    else {
        return false;
    };
    file.set_permissions(fs::Permissions::from_mode(0o600))
        .is_ok()
        && file.write_all(record).is_ok()
}

fn read_mapping(path: &Path, uid: u32) -> Option<(Vec<u8>, Vec<u8>)> {
    let metadata = fs::symlink_metadata(path).ok()?;
    if !metadata.is_file()
        || metadata.file_type().is_symlink()
        || metadata.uid() != uid
        || metadata.mode() & 0o077 != 0
    {
        return None;
    }
    let record = fs::read(path).ok()?;
    let split = record.iter().position(|byte| *byte == 0)?;
    let host_path = record[..split].to_vec();
    let guest_path = record[split + 1..].to_vec();
    if guest_path.len() >= GUEST_SUN_PATH || guest_path.contains(&0) {
        return None;
    }
    Some((host_path, guest_path))
}

fn copy_c_path(bytes: &[u8], output: *mut c_char, capacity: usize) -> bool {
    if output.is_null() || bytes.contains(&0) || bytes.len().saturating_add(1) > capacity {
        return false;
    }
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), output.cast::<u8>(), bytes.len());
        *output.add(bytes.len()) = 0;
    }
    true
}

/// Translates a guest filesystem socket address and shortens it if necessary.
#[no_mangle]
pub unsafe extern "C" fn lxrt_socket_path_for_darwin(
    guest_path: *const c_char,
    output: *mut c_char,
    capacity: usize,
) -> c_int {
    if guest_path.is_null() || output.is_null() {
        return DARWIN_EFAULT;
    }
    let guest = unsafe { CStr::from_ptr(guest_path) };
    let mut translated = [0 as c_char; 1024];
    let translated_path =
        unsafe { lxrt_translate_guest_path(guest_path, translated.as_mut_ptr(), translated.len()) };
    if translated_path.is_null() {
        return DARWIN_ENOENT;
    }
    let host_path = unsafe { CStr::from_ptr(translated_path) };
    if host_path.to_bytes().is_empty() {
        return DARWIN_ENOENT;
    }
    if host_path.to_bytes().len() >= DARWIN_SUN_PATH {
        let Ok(host_path) = CString::new(host_path.to_bytes()) else {
            return DARWIN_EINVAL;
        };
        let mut alias = [0 as c_char; DARWIN_SUN_PATH];
        if !unsafe {
            lxrt_socket_path_alias(
                host_path.as_ptr(),
                guest.as_ptr(),
                alias.as_mut_ptr(),
                alias.len(),
            )
        } {
            return DARWIN_ENAMETOOLONG;
        }
        let alias_path = unsafe { CStr::from_ptr(alias.as_ptr()) }.to_bytes();
        return if copy_c_path(alias_path, output, capacity) {
            0
        } else {
            DARWIN_ENAMETOOLONG
        };
    }
    if copy_c_path(host_path.to_bytes(), output, capacity) {
        0
    } else {
        DARWIN_ENAMETOOLONG
    }
}

/// Maps a too-long translated host pathname to a private, short socket path.
#[no_mangle]
pub unsafe extern "C" fn lxrt_socket_path_alias(
    translated_host_path: *const c_char,
    guest_path: *const c_char,
    output: *mut c_char,
    capacity: usize,
) -> bool {
    if translated_host_path.is_null() || guest_path.is_null() || output.is_null() {
        return false;
    }
    let host_path = unsafe { CStr::from_ptr(translated_host_path) }.to_bytes();
    let guest_path = unsafe { CStr::from_ptr(guest_path) }.to_bytes();
    if host_path.is_empty() || host_path[0] != b'/' || guest_path.len() >= GUEST_SUN_PATH {
        return false;
    }
    let Some(directory) = socket_directory() else {
        return false;
    };
    let Some(record) = mapping_record(host_path, guest_path) else {
        return false;
    };
    let uid = unsafe { getuid() };

    for salt in 0..MAX_COLLISIONS {
        let socket_path = directory.join(socket_name(host_path, salt));
        let socket_bytes = socket_path.as_os_str().as_bytes();
        if socket_bytes.len() >= DARWIN_SUN_PATH {
            return false;
        }
        let metadata_path = mapping_path(&socket_path);
        if metadata_path.exists() {
            match read_mapping(&metadata_path, uid) {
                Some((existing_host, _)) if existing_host == host_path => {
                    if !write_mapping(&metadata_path, &record) {
                        return false;
                    }
                    return copy_c_path(socket_bytes, output, capacity);
                }
                _ => continue,
            }
        }
        if socket_path.exists() || !write_mapping(&metadata_path, &record) {
            continue;
        }
        return copy_c_path(socket_bytes, output, capacity);
    }
    false
}

/// Resolves file operations on an already-aliased guest Unix socket path.
#[no_mangle]
pub unsafe extern "C" fn lxrt_socket_path_existing_alias(
    translated_host_path: *const c_char,
    guest_path: *const c_char,
    output: *mut c_char,
    capacity: usize,
) -> bool {
    if translated_host_path.is_null() || guest_path.is_null() || output.is_null() {
        return false;
    }
    let host_path = unsafe { CStr::from_ptr(translated_host_path) }
        .to_bytes()
        .to_vec();
    let guest_path = unsafe { CStr::from_ptr(guest_path) }.to_bytes().to_vec();
    if host_path.len() < DARWIN_SUN_PATH
        || host_path.first() != Some(&b'/')
        || guest_path.len() >= GUEST_SUN_PATH
    {
        return false;
    }
    let Some(directory) = socket_directory() else {
        return false;
    };
    let uid = unsafe { getuid() };
    for salt in 0..MAX_COLLISIONS {
        let socket_path = directory.join(socket_name(&host_path, salt));
        let metadata_path = mapping_path(&socket_path);
        let Some((existing_host, existing_guest)) = read_mapping(&metadata_path, uid) else {
            continue;
        };
        if existing_host == host_path && existing_guest == guest_path {
            return copy_c_path(socket_path.as_os_str().as_bytes(), output, capacity);
        }
    }
    false
}

/// Restores the original guest pathname returned by getsockname/getpeername.
#[no_mangle]
pub unsafe extern "C" fn lxrt_socket_path_unalias(
    host_socket_path: *const c_char,
    output: *mut c_char,
    capacity: usize,
) -> bool {
    if host_socket_path.is_null() || output.is_null() {
        return false;
    }
    let host_socket_path = unsafe { CStr::from_ptr(host_socket_path) }.to_bytes();
    let uid = unsafe { getuid() };
    let Some(directory) = socket_directory() else {
        return false;
    };
    let directory_bytes = directory.as_os_str().as_bytes();
    let Some(name) = host_socket_path
        .strip_prefix(directory_bytes)
        .and_then(|suffix| suffix.strip_prefix(b"/"))
    else {
        return false;
    };
    if name.len() != HASH_BYTES + 1 || name.first() != Some(&b's') {
        return false;
    }
    let socket_path = PathBuf::from(OsStr::from_bytes(host_socket_path));
    let metadata_path = mapping_path(&socket_path);
    let Some((translated_path, guest_path)) = read_mapping(&metadata_path, uid) else {
        return false;
    };
    if !(0..MAX_COLLISIONS).any(|salt| socket_name(&translated_path, salt).as_bytes() == name) {
        return false;
    }
    copy_c_path(&guest_path, output, capacity)
}
