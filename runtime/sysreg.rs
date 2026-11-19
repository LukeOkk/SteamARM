// Linux system-register values for registers Darwin traps at EL0.

use std::ffi::{c_char, c_int, c_void, CString};
use std::mem::size_of;
use std::sync::atomic::{AtomicU32, Ordering};

static CTR: AtomicU32 = AtomicU32::new(0);

#[link(name = "System")]
unsafe extern "C" {
    fn sysctlbyname(
        name: *const c_char,
        old_value: *mut c_void,
        old_len: *mut usize,
        new_value: *mut c_void,
        new_len: usize,
    ) -> c_int;
}

fn field(value: u64, shift: u32) -> u64 {
    value << shift
}

fn feature(name: &str) -> bool {
    let key = CString::new(format!("hw.optional.arm.{name}")).expect("feature key has no NUL");
    let mut value: c_int = 0;
    let mut size = size_of::<c_int>();
    unsafe {
        sysctlbyname(
            key.as_ptr(),
            (&mut value as *mut c_int).cast(),
            &mut size,
            std::ptr::null_mut(),
            0,
        ) == 0
            && value != 0
    }
}

#[no_mangle]
pub extern "C" fn lxrt_synthetic_ctr_el0() -> u32 {
    let cached = CTR.load(Ordering::Relaxed);
    if cached != 0 {
        return cached;
    }

    // Apple Silicon's cache-maintenance line is 64 bytes. A smaller safe
    // stride prevents icache flushes from skipping stale instruction lines.
    let words = 64u32 / 4;
    let mut log2_words = 0;
    while (1u32 << (log2_words + 1)) <= words {
        log2_words += 1;
    }
    let value = (1u32 << 31)
        | (log2_words << 24)
        | (4u32 << 20)
        | (log2_words << 16)
        | (3u32 << 14)
        | log2_words;
    let _ = CTR.compare_exchange(0, value, Ordering::Relaxed, Ordering::Relaxed);
    CTR.load(Ordering::Relaxed)
}

fn id_aa64isar0() -> u64 {
    field(
        if feature("FEAT_PMULL") {
            2
        } else if feature("FEAT_AES") {
            1
        } else {
            0
        },
        4,
    ) | field(feature("FEAT_SHA1") as u64, 8)
        | field(
            if feature("FEAT_SHA512") {
                2
            } else if feature("FEAT_SHA256") {
                1
            } else {
                0
            },
            12,
        )
        | field(feature("FEAT_CRC32") as u64, 16)
        | field(if feature("FEAT_LSE") { 2 } else { 0 }, 20)
        | field(feature("FEAT_RDM") as u64, 28)
        | field(feature("FEAT_SHA3") as u64, 32)
        | field(feature("FEAT_DotProd") as u64, 44)
        | field(feature("FEAT_FHM") as u64, 48)
        | field(
            if feature("FEAT_FlagM2") {
                2
            } else if feature("FEAT_FlagM") {
                1
            } else {
                0
            },
            52,
        )
}

fn id_aa64isar1() -> u64 {
    field(
        if feature("FEAT_DPB2") {
            2
        } else if feature("FEAT_DPB") {
            1
        } else {
            0
        },
        0,
    ) | field(feature("FEAT_JSCVT") as u64, 12)
        | field(feature("FEAT_FCMA") as u64, 16)
        | field(
            if feature("FEAT_LRCPC2") {
                2
            } else if feature("FEAT_LRCPC") {
                1
            } else {
                0
            },
            20,
        )
        | field(feature("FEAT_FRINTTS") as u64, 32)
        | field(feature("FEAT_SB") as u64, 36)
        | field(feature("FEAT_BF16") as u64, 44)
        | field(feature("FEAT_I8MM") as u64, 52)
    // Pointer authentication remains absent because guests cannot enable the
    // kernel-owned process state that Apple requires before authenticating.
}

fn id_aa64isar2() -> u64 {
    field(if feature("FEAT_WFxT") { 2 } else { 0 }, 0) | field(feature("FEAT_RPRES") as u64, 4)
}

fn id_aa64pfr0() -> u64 {
    let fp16 = feature("FEAT_FP16") as u64;
    field(1, 0)
        | field(1, 4)
        | field(fp16, 16)
        | field(fp16, 20)
        | field(feature("FEAT_DIT") as u64, 48)
        | field(feature("FEAT_CSV2") as u64, 56)
        | field(feature("FEAT_CSV3") as u64, 60)
}

fn id_aa64pfr1() -> u64 {
    // SME stays absent: guests cannot enable its streaming state through EL1.
    field(feature("FEAT_BTI") as u64, 0)
}

fn id_aa64mmfr0() -> u64 {
    // The guest uses the host's 16 KiB translation granule.
    field(2, 0) | field(2, 4) | field(1, 20)
}

fn id_aa64mmfr2() -> u64 {
    field(feature("FEAT_LSE2") as u64, 32)
}

#[no_mangle]
pub extern "C" fn lxrt_synthetic_sysreg(instruction: u32) -> u64 {
    let crm = (instruction >> 8) & 0xf;
    let op2 = (instruction >> 5) & 0x7;

    if crm == 0 && op2 == 0 {
        // MIDR_EL1: Apple implementer, architecture uses the ID registers.
        return field(0x61, 24) | field(0xf, 16) | field(0x030, 4) | 1;
    }
    if crm == 4 {
        return match op2 {
            0 => id_aa64pfr0(),
            1 => id_aa64pfr1(),
            _ => 0,
        };
    }
    if crm == 6 {
        return match op2 {
            0 => id_aa64isar0(),
            1 => id_aa64isar1(),
            2 => id_aa64isar2(),
            _ => 0,
        };
    }
    if crm == 7 {
        return match op2 {
            0 => id_aa64mmfr0(),
            2 => id_aa64mmfr2(),
            _ => 0,
        };
    }
    0
}
