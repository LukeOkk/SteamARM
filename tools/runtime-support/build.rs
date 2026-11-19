use std::collections::BTreeSet;
use std::env;
use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::process::{Command, Stdio};

const ERRNO_MAP: &[(&str, i32)] = &[
    ("EPERM", 1),
    ("ENOENT", 2),
    ("ESRCH", 3),
    ("EINTR", 4),
    ("EIO", 5),
    ("ENXIO", 6),
    ("E2BIG", 7),
    ("ENOEXEC", 8),
    ("EBADF", 9),
    ("ECHILD", 10),
    ("ENOMEM", 12),
    ("EACCES", 13),
    ("EFAULT", 14),
    ("ENOTBLK", 15),
    ("EBUSY", 16),
    ("EEXIST", 17),
    ("EXDEV", 18),
    ("ENODEV", 19),
    ("ENOTDIR", 20),
    ("EISDIR", 21),
    ("EINVAL", 22),
    ("ENFILE", 23),
    ("EMFILE", 24),
    ("ENOTTY", 25),
    ("ETXTBSY", 26),
    ("EFBIG", 27),
    ("ENOSPC", 28),
    ("ESPIPE", 29),
    ("EROFS", 30),
    ("EMLINK", 31),
    ("EPIPE", 32),
    ("EDOM", 33),
    ("ERANGE", 34),
    ("EAGAIN", 11),
    ("EDEADLK", 35),
    ("ENAMETOOLONG", 36),
    ("ENOLCK", 37),
    ("ENOSYS", 38),
    ("ENOTEMPTY", 39),
    ("ELOOP", 40),
    ("ENOMSG", 42),
    ("EIDRM", 43),
    ("ENOSTR", 60),
    ("ENODATA", 61),
    ("ETIME", 62),
    ("ENOSR", 63),
    ("EREMOTE", 66),
    ("ENOLINK", 67),
    ("EPROTO", 71),
    ("EMULTIHOP", 72),
    ("EBADMSG", 74),
    ("EOVERFLOW", 75),
    ("EILSEQ", 84),
    ("EUSERS", 87),
    ("ENOTSOCK", 88),
    ("EDESTADDRREQ", 89),
    ("EMSGSIZE", 90),
    ("EPROTOTYPE", 91),
    ("ENOPROTOOPT", 92),
    ("EPROTONOSUPPORT", 93),
    ("ESOCKTNOSUPPORT", 94),
    ("ENOTSUP", 95),
    ("EPFNOSUPPORT", 96),
    ("EAFNOSUPPORT", 97),
    ("EADDRINUSE", 98),
    ("EADDRNOTAVAIL", 99),
    ("ENETDOWN", 100),
    ("ENETUNREACH", 101),
    ("ENETRESET", 102),
    ("ECONNABORTED", 103),
    ("ECONNRESET", 104),
    ("ENOBUFS", 105),
    ("EISCONN", 106),
    ("ENOTCONN", 107),
    ("ESHUTDOWN", 108),
    ("ETOOMANYREFS", 109),
    ("ETIMEDOUT", 110),
    ("ECONNREFUSED", 111),
    ("EHOSTDOWN", 112),
    ("EHOSTUNREACH", 113),
    ("EALREADY", 114),
    ("EINPROGRESS", 115),
    ("ESTALE", 116),
    ("EDQUOT", 122),
    ("ECANCELED", 125),
    ("EOWNERDEAD", 130),
    ("ENOTRECOVERABLE", 131),
    ("EOPNOTSUPP", 95),
];

const DARWIN_CONSTANTS: &[&str] = &[
    "O_RDONLY",
    "O_CREAT",
    "O_EXCL",
    "O_NOCTTY",
    "O_TRUNC",
    "O_APPEND",
    "O_NONBLOCK",
    "O_DIRECTORY",
    "O_NOFOLLOW",
    "O_CLOEXEC",
    "O_SYNC",
    "O_DSYNC",
    "O_SYMLINK",
    "AT_SYMLINK_NOFOLLOW",
    "AT_SYMLINK_FOLLOW",
    "AT_FDCWD",
    "F_GETLK",
    "F_SETLK",
    "F_SETLKW",
    "F_RDLCK",
    "F_WRLCK",
    "F_UNLCK",
];

const ID_CONSTANTS: &[&str] = &[
    "O_NOFOLLOW",
    "MAXCOMLEN",
    "PROC_PIDTBSDINFO",
    "PROT_READ",
    "PROT_WRITE",
    "MAP_SHARED",
];

const DIRENT_CONSTANTS: &[&str] = &[
    "DT_FIFO", "DT_CHR", "DT_DIR", "DT_BLK", "DT_REG", "DT_LNK", "DT_SOCK",
];

const TIMER_CONSTANTS: &[&str] = &["SIG_BLOCK", "SIG_SETMASK"];

const PATHFD_CONSTANTS: &[&str] = &[
    "AT_FDCWD",
    "EFAULT",
    "EINVAL",
    "EOPNOTSUPP",
    "F_GETPATH",
    "MAXPATHLEN",
    "O_CLOEXEC",
    "O_RDONLY",
    "PATH_MAX",
    "EMFILE",
    "ENOMEM",
];

const GUESTPROF_CONSTANTS: &[&str] =
    &["ARM_THREAD_STATE64", "MAXPATHLEN", "PROC_PIDREGIONPATHINFO"];

const PRIVMAP_CONSTANTS: &[&str] = &[
    "MAP_ANON",
    "MAP_FIXED",
    "MAP_PRIVATE",
    "MAP_SHARED",
    "PROT_EXEC",
    "PROT_READ",
    "PROT_WRITE",
];

const STACK_CONSTANTS: &[&str] = &["MAP_ANON", "MAP_PRIVATE", "PROT_READ", "PROT_WRITE"];

const FUTEX_WAITV_CONSTANTS: &[&str] = &[
    "EAGAIN",
    "EINTR",
    "EINVAL",
    "ENOMEM",
    "ETIMEDOUT",
    "MAP_SHARED",
    "O_CLOEXEC",
    "O_CREAT",
    "O_RDWR",
    "PROT_READ",
    "PROT_WRITE",
];

const TLS_CONSTANTS: &[&str] = &["SIGBUS", "SIGSEGV", "SIGILL", "SIGTRAP"];

const ARENA_CONSTANTS: &[&str] = &["MAP_ANON", "MAP_FIXED", "MAP_PRIVATE"];

fn c_integer(value: &str) -> Option<i32> {
    let value = value.trim_end_matches(['u', 'U', 'l', 'L']);
    if let Some(hex) = value
        .strip_prefix("0x")
        .or_else(|| value.strip_prefix("0X"))
    {
        i32::from_str_radix(hex, 16).ok()
    } else if let Some(hex) = value
        .strip_prefix("-0x")
        .or_else(|| value.strip_prefix("-0X"))
    {
        i32::from_str_radix(hex, 16).ok().map(|number| -number)
    } else {
        value.parse().ok()
    }
}

fn main() {
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        panic!("SteamARM runtime support must be built for macOS");
    }
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=PATH");
    println!("cargo:rerun-if-env-changed=SDKROOT");
    println!("cargo:rerun-if-env-changed=DEVELOPER_DIR");

    let names: Vec<&str> = ERRNO_MAP
        .iter()
        .map(|(name, _)| *name)
        .chain(DARWIN_CONSTANTS.iter().copied())
        .chain(ID_CONSTANTS.iter().copied())
        .chain(DIRENT_CONSTANTS.iter().copied())
        .chain(TIMER_CONSTANTS.iter().copied())
        .chain(PATHFD_CONSTANTS.iter().copied())
        .chain(GUESTPROF_CONSTANTS.iter().copied())
        .chain(PRIVMAP_CONSTANTS.iter().copied())
        .chain(STACK_CONSTANTS.iter().copied())
        .chain(FUTEX_WAITV_CONSTANTS.iter().copied())
        .chain(TLS_CONSTANTS.iter().copied())
        .chain(ARENA_CONSTANTS.iter().copied())
        .collect();
    let mut source = String::from(
        "#include <errno.h>\n#include <fcntl.h>\n#include <dirent.h>\n#include <signal.h>\n#include <limits.h>\n#include <time.h>\n#include <sys/param.h>\n#include <sys/proc.h>\n#include <sys/proc_info.h>\n#include <sys/mman.h>\n#include <mach/arm/thread_status.h>\n",
    );
    for (index, name) in names.iter().enumerate() {
        source.push_str(&format!(
            "#ifdef {name}\nSTEAMARM_CONST_{index} {name}\n#else\nSTEAMARM_CONST_{index} 0\n#endif\n"
        ));
    }
    let mut child = Command::new("clang")
        .args(["-E", "-P", "-x", "c", "-"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("run clang to read macOS SDK constants");
    child
        .stdin
        .take()
        .expect("clang stdin")
        .write_all(source.as_bytes())
        .expect("write SDK macro query");
    let output = child.wait_with_output().expect("wait for clang");
    if !output.status.success() {
        panic!(
            "clang SDK macro query failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
    }
    let mut values = vec![None; names.len()];
    for line in String::from_utf8(output.stdout)
        .expect("clang output is UTF-8")
        .lines()
    {
        let Some((marker, value)) = line.split_once(char::is_whitespace) else {
            continue;
        };
        let Some(index) = marker
            .strip_prefix("STEAMARM_CONST_")
            .and_then(|index| index.parse::<usize>().ok())
        else {
            continue;
        };
        if let Some(slot) = values.get_mut(index) {
            *slot = c_integer(value.trim());
        }
    }
    let values: Vec<i32> = values
        .into_iter()
        .enumerate()
        .map(|(index, value)| value.unwrap_or_else(|| panic!("clang omitted {}", names[index])))
        .collect();

    let errno_values = &values[..ERRNO_MAP.len()];
    let mut seen = BTreeSet::new();
    let mut arms = String::new();
    for ((name, linux), host) in ERRNO_MAP.iter().zip(errno_values) {
        if *host > 0 && seen.insert(*host) {
            arms.push_str(&format!("        {host} => {linux}, // {name}\n"));
        }
    }
    let errno_source = format!(
        "pub fn map_errno(error: i32) -> i32 {{\n    match error {{\n{arms}        _ => 5,\n    }}\n}}\n"
    );
    let mut constants_source = String::new();
    for (name, value) in DARWIN_CONSTANTS.iter().zip(&values[ERRNO_MAP.len()..]) {
        if *value == 0 && *name != "O_RDONLY" {
            panic!("macOS SDK did not define required {name}");
        }
        constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    let mut ids_constants_source = String::new();
    for name in ["ESRCH"] {
        let index = ERRNO_MAP
            .iter()
            .position(|(error, _)| *error == name)
            .unwrap_or_else(|| panic!("{name} is in the errno map"));
        ids_constants_source.push_str(&format!(
            "pub const D_{name}: i32 = {};\n",
            errno_values[index]
        ));
    }
    let id_start = ERRNO_MAP.len() + DARWIN_CONSTANTS.len();
    for (name, value) in ID_CONSTANTS.iter().zip(&values[id_start..]) {
        if *value == 0 && *name != "O_RDONLY" {
            panic!("macOS SDK did not define required {name}");
        }
        ids_constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    let mut dirent_constants_source = String::new();
    for name in ["EFAULT", "ENOTDIR", "EINVAL"] {
        let index = ERRNO_MAP
            .iter()
            .position(|(error, _)| *error == name)
            .unwrap_or_else(|| panic!("{name} is in the errno map"));
        dirent_constants_source.push_str(&format!(
            "pub const D_{name}: i32 = {};\n",
            errno_values[index]
        ));
    }
    let dirent_start = id_start + ID_CONSTANTS.len();
    for (name, value) in DIRENT_CONSTANTS.iter().zip(&values[dirent_start..]) {
        if *value == 0 && *name != "DT_FIFO" {
            panic!("macOS SDK did not define required {name}");
        }
        dirent_constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    let output_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR"));
    fs::write(output_dir.join("errno_map_generated.rs"), errno_source)
        .expect("write generated errno table");
    fs::write(output_dir.join("fcntl_constants.rs"), constants_source)
        .expect("write generated Darwin constants");
    fs::write(output_dir.join("ids_constants.rs"), ids_constants_source)
        .expect("write generated process ID constants");
    fs::write(
        output_dir.join("dirent_constants.rs"),
        dirent_constants_source,
    )
    .expect("write generated dirent constants");
    let timer_start = dirent_start + DIRENT_CONSTANTS.len();
    let mut timer_constants_source = String::new();
    for (name, value) in TIMER_CONSTANTS.iter().zip(&values[timer_start..]) {
        if *value == 0 {
            panic!("macOS SDK did not define required {name}");
        }
        timer_constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    fs::write(
        output_dir.join("timer_constants.rs"),
        timer_constants_source,
    )
    .expect("write generated timer constants");
    let pathfd_start = timer_start + TIMER_CONSTANTS.len();
    let mut pathfd_constants_source = String::new();
    for (name, value) in PATHFD_CONSTANTS.iter().zip(&values[pathfd_start..]) {
        if *value == 0 && *name != "O_RDONLY" {
            panic!("macOS SDK did not define required {name}");
        }
        pathfd_constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    fs::write(
        output_dir.join("pathfd_constants.rs"),
        pathfd_constants_source,
    )
    .expect("write generated pathfd constants");
    let guestprof_start = pathfd_start + PATHFD_CONSTANTS.len();
    let mut guestprof_constants_source = String::new();
    for (name, value) in GUESTPROF_CONSTANTS.iter().zip(&values[guestprof_start..]) {
        if *value == 0 {
            panic!("macOS SDK did not define required {name}");
        }
        guestprof_constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    fs::write(
        output_dir.join("guestprof_constants.rs"),
        guestprof_constants_source,
    )
    .expect("write generated guest profiler constants");
    let privmap_start = guestprof_start + GUESTPROF_CONSTANTS.len();
    let mut privmap_constants_source = String::new();
    for (name, value) in PRIVMAP_CONSTANTS.iter().zip(&values[privmap_start..]) {
        if *value == 0 {
            panic!("macOS SDK did not define required {name}");
        }
        privmap_constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    fs::write(
        output_dir.join("privmap_constants.rs"),
        privmap_constants_source,
    )
    .expect("write generated private mapping constants");
    let stack_start = privmap_start + PRIVMAP_CONSTANTS.len();
    let mut stack_constants_source = String::new();
    for (name, value) in STACK_CONSTANTS.iter().zip(&values[stack_start..]) {
        if *value == 0 {
            panic!("macOS SDK did not define required {name}");
        }
        stack_constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    fs::write(
        output_dir.join("stack_constants.rs"),
        stack_constants_source,
    )
    .expect("write generated guest stack constants");
    let waitv_start = stack_start + STACK_CONSTANTS.len();
    let mut waitv_constants_source = String::new();
    for (name, value) in FUTEX_WAITV_CONSTANTS.iter().zip(&values[waitv_start..]) {
        if *value == 0 {
            panic!("macOS SDK did not define required {name}");
        }
        waitv_constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    fs::write(
        output_dir.join("futex_waitv_constants.rs"),
        waitv_constants_source,
    )
    .expect("write futex_waitv constants");
    let tls_start = waitv_start + FUTEX_WAITV_CONSTANTS.len();
    let mut tls_constants_source = String::new();
    for (name, value) in TLS_CONSTANTS.iter().zip(&values[tls_start..]) {
        if *value == 0 {
            panic!("macOS SDK did not define required {name}");
        }
        tls_constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    fs::write(output_dir.join("tls_constants.rs"), tls_constants_source)
        .expect("write TLS constants");
    let arena_start = tls_start + TLS_CONSTANTS.len();
    let mut arena_constants_source = String::new();
    for (name, value) in ARENA_CONSTANTS.iter().zip(&values[arena_start..]) {
        if *value == 0 {
            panic!("macOS SDK did not define required {name}");
        }
        arena_constants_source.push_str(&format!("pub const D_{name}: i32 = {value};\n"));
    }
    fs::write(
        output_dir.join("arena_constants.rs"),
        arena_constants_source,
    )
    .expect("write arena constants");
}
