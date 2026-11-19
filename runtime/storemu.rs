// Perform one AArch64 store instruction on behalf of the guest.
//
// Why this exists. A 16 KiB host page that holds both a guest code page and a
// writable guest data page (4 KiB-aligned images and libraries, subpage.c) is
// read-write or read-execute, never both: Darwin refuses write+execute
// outside MAP_JIT. A store executed from that page INTO that page cannot
// complete natively -- the store needs write, the fetch of the store itself
// needs execute -- and the fault handler flipped the page between the two at
// the same pc forever (MEASURED: tests/elf/subpage4k.S "selfwrite", an endless
// pair of write/exec flips at one pc). So the fault handler performs that one
// store itself: this file decodes it and carries it out while the thread is
// parked in the handler, with the page opened for writing by the caller and
// closed again before the thread resumes at pc + 4. The host page is never
// writable and executable at the same time.
//
// Covered: every A64 base and SIMD&FP store -- STR/STRB/STRH/STUR/STTR
// (unsigned offset, unscaled, pre/post-index, register offset), STP/STNP
// (X, W, S, D, Q), ST1-ST4 (multiple and single structure, post-index by
// immediate or register) -- the ordered stores (STLR, STLLR, STLUR), the LSE
// atomics (LDADD/LDCLR/LDEOR/LDSET/LD[SU]MAX/LD[SU]MIN, SWP, CAS, CASP), the
// exclusive stores (STXR/STLXR/STXP/STLXP, below) and DC ZVA. Anything else is
// refused, and the caller keeps its previous behaviour.
//
// Atomicity: atomics and CAS are performed with host atomics on the same
// memory, so they stay atomic against other threads that access it natively.
// A plain store keeps per-element single-copy atomicity (aligned 1/2/4/8/16
// bytes). Every emulated access is bracketed by full barriers, which is at
// least as strong as any ordering the guest instruction asked for.
//
// Exclusive stores. The local exclusive monitor does not survive the fault
// (MEASURED on an M4, benchmarks/stage23: 64 of 64 STXR fail when a page
// fault falls between them and their LDXR, 0 of 64 without), so a native
// retry of the STXR would fail and the guest's retry loop would fault again.
// The STXR is performed instead as a compare-and-swap against the
// value its load-exclusive returned: the nearest preceding LDXR/LDAXR (LDXP/
// LDAXP) with the same size and base register, provided nothing between the
// two can have overwritten that value or the base (checked conservatively,
// word by word). That is how QEMU's user mode emulates LL/SC; like it, this
// accepts ABA (a value changed and changed back in between still succeeds).
// No such load found: refused.
//
// This code runs inside the fault handler: no allocation, no locks, and no
// panicking paths (register and buffer accesses are raw, arithmetic wraps
// exactly as the AArch64 code the C original compiled to).

use std::arch::asm;
use std::ffi::{c_char, c_int, c_uint, c_void, CStr};
use std::mem::{offset_of, size_of};
use std::ptr;
use std::sync::atomic::{fence, AtomicU16, AtomicU32, AtomicU64, AtomicU8, Ordering};

// enum lxrt_storemu_kind (storemu.h).
const LXRT_SM_STORE: c_int = 0; // plain store: data[] in memory order
const LXRT_SM_RELEASE: c_int = 1; // STLR / STLLR / STLUR: data[] with release order
const LXRT_SM_ZVA: c_int = 2; // DC ZVA: len zero bytes
const LXRT_SM_ATOMIC: c_int = 3; // LSE LD<op> / SWP: old value to rt
const LXRT_SM_CAS: c_int = 4; // CAS: old value to rs
const LXRT_SM_CASP: c_int = 5; // CASP: old pair to rs, rs+1
const LXRT_SM_EXCL: c_int = 6; // STXR/STLXR/STXP/STLXP: compare-and-swap, status to rs

// struct lxrt_storemu (storemu.h).
#[repr(C)]
pub struct LxrtStoremu {
    kind: c_int,
    addr: u64,          // first byte written
    len: u32,           // bytes written
    elem: u32,          // single-copy-atomic unit of a plain store (bytes)
    op: c_int,          // LXRT_SM_ATOMIC: the operation
    rs: c_uint,         // result registers (see kind)
    rt: c_uint,
    pair32: bool,       // CASP / STXP with 32-bit registers
    val: [u64; 2],      // operand / new value (pair: [0] low half)
    cmp: [u64; 2],      // expected value (pair: [0] low half)
    wb: c_int,          // base register to write back, -1 none (31 = SP)
    wb_val: u64,
    data: [u8; 64],     // LXRT_SM_STORE / LXRT_SM_RELEASE
    // LXRT_SM_EXCL with block: the loop's retry is performed.
    block: bool,
    ldxr_pc: u64,
    stxr_pc: u64,
    lt: c_uint,         // the load-exclusive's destinations
    lt2: c_uint,
    st_rt: c_uint,      // the store-exclusive's sources
    st_rt2: c_uint,
    rn: c_uint,         // their base
    nbetween: c_int,
    between: [u32; 16], // the words between the two
}

const _: [(); 264] = [(); size_of::<LxrtStoremu>()];
const _: [(); 8] = [(); offset_of!(LxrtStoremu, addr)];
const _: [(); 36] = [(); offset_of!(LxrtStoremu, pair32)];
const _: [(); 40] = [(); offset_of!(LxrtStoremu, val)];
const _: [(); 72] = [(); offset_of!(LxrtStoremu, wb)];
const _: [(); 88] = [(); offset_of!(LxrtStoremu, data)];
const _: [(); 152] = [(); offset_of!(LxrtStoremu, block)];
const _: [(); 160] = [(); offset_of!(LxrtStoremu, ldxr_pc)];
const _: [(); 176] = [(); offset_of!(LxrtStoremu, lt)];
const _: [(); 196] = [(); offset_of!(LxrtStoremu, nbetween)];
const _: [(); 200] = [(); offset_of!(LxrtStoremu, between)];

// _STRUCT_ARM_THREAD_STATE64 (arm64, no pointer authentication: __fp, __lr,
// __sp and __pc are plain fields).
#[repr(C)]
struct Tstate {
    x: [u64; 29],
    fp: u64,
    lr: u64,
    sp: u64,
    pc: u64,
    cpsr: u32,
    pad: u32,
}

const _: [(); 272] = [(); size_of::<Tstate>()];

// ucontext_t and _STRUCT_MCONTEXT64 as <sys/ucontext.h> lays them out:
// uc_mcontext is the seventh member of the ucontext; the machine context is
// __es (16 bytes), __ss (272 bytes), __ns (__v[32] of 128 bits, 16-aligned).
const UC_MCONTEXT_OFFSET: usize = 48;
const MC_SS_OFFSET: usize = 16;
const MC_NS_V_OFFSET: usize = 288;

type MachPort = c_uint;
type KernReturn = c_int;
const KERN_SUCCESS: KernReturn = 0;

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: MachPort;
    fn mach_vm_read_overwrite(
        target_task: MachPort,
        address: u64,
        size: u64,
        data: u64,
        outsize: *mut u64,
    ) -> KernReturn;
}

#[inline(always)]
unsafe fn mcontext(uap: *mut c_void) -> *mut u8 {
    uap.cast::<u8>()
        .add(UC_MCONTEXT_OFFSET)
        .cast::<*mut u8>()
        .read()
}

// X registers as the ucontext holds them. r == 31 is XZR here; the _sp
// variants treat it as SP (a base register).
#[inline]
unsafe fn xr(ss: *const Tstate, r: c_uint) -> u64 {
    if r == 31 {
        return 0;
    }
    if r == 29 {
        return (*ss).fp;
    }
    if r == 30 {
        return (*ss).lr;
    }
    ptr::addr_of!((*ss).x).cast::<u64>().add(r as usize).read()
}

#[inline]
unsafe fn xr_sp(ss: *const Tstate, r: c_uint) -> u64 {
    if r == 31 {
        (*ss).sp
    } else {
        xr(ss, r)
    }
}

#[inline]
unsafe fn set_xr(ss: *mut Tstate, r: c_uint, v: u64) {
    if r == 31 {
        return;
    }
    if r == 29 {
        (*ss).fp = v;
    } else if r == 30 {
        (*ss).lr = v;
    } else {
        ptr::addr_of_mut!((*ss).x)
            .cast::<u64>()
            .add(r as usize)
            .write(v);
    }
}

#[inline]
unsafe fn set_xr_sp(ss: *mut Tstate, r: c_uint, v: u64) {
    if r == 31 {
        (*ss).sp = v;
    } else {
        set_xr(ss, r, v);
    }
}

#[inline(always)]
fn fld(w: u32, hi: u32, lo: u32) -> u32 {
    (w >> lo) & (1u32.wrapping_shl(hi - lo + 1).wrapping_sub(1))
}

#[inline(always)]
fn sext(v: u64, nbits: u32) -> i64 {
    let s = 64u32.wrapping_sub(nbits);
    (v.wrapping_shl(s) as i64).wrapping_shr(s)
}

#[inline(always)]
fn mask_n(n: u32) -> u64 {
    if n >= 8 {
        !0u64
    } else {
        (1u64 << (8 * n)) - 1
    }
}

unsafe fn put_le(dst: *mut u8, v: u64, n: u32) {
    let mut i = 0u32;
    while i < n {
        dst.add(i as usize).write(v.wrapping_shr(8 * i) as u8);
        i += 1;
    }
}

// Element e of esize bytes of a vector register (little-endian lanes).
unsafe fn put_lane(dst: *mut u8, v: u128, e: u32, esize: u32) {
    let b = v.to_le_bytes();
    ptr::copy_nonoverlapping(
        b.as_ptr().add((e * esize) as usize),
        dst,
        esize as usize,
    );
}

#[inline]
unsafe fn refuse(why: *mut *const c_char, what: &'static CStr) -> c_int {
    *why = what.as_ptr();
    -1
}

unsafe fn read_word(a: u64, w: *mut u32) -> bool {
    let mut got: u64 = 0;
    mach_vm_read_overwrite(mach_task_self_, a, 4, w as usize as u64, &mut got) == KERN_SUCCESS
        && got == 4
}

// Can instruction w, found between a load-exclusive and its store-exclusive,
// change register r or break the straight line? Conservative: every field that
// can name a destination counts, and every control transfer but a conditional
// branch not taken (execution reached the store) gives up.
fn clobbers(w: u32, r: c_uint) -> bool {
    if (w & 0x1c00_0000) == 0x1400_0000 {
        // branches, system
        if (w & 0xff00_0010) == 0x5400_0000 {
            return false; // B.cond
        }
        if (w & 0x7e00_0000) == 0x3400_0000 {
            return false; // CBZ/CBNZ
        }
        if (w & 0x7e00_0000) == 0x3600_0000 {
            return false; // TBZ/TBNZ
        }
        if w == 0xd503_201f || w == 0xd503_203f {
            return false; // NOP, YIELD
        }
        let b = w & 0xffff_f0ff;
        if b == 0xd503_309f || b == 0xd503_30bf || b == 0xd503_30df {
            return false; // DSB, DMB, ISB
        }
        return true; // CLREX, BL, MRS, ...
    }
    if (w & 31) == r {
        return true;
    }
    if (w & 0x0a00_0000) == 0x0800_0000 // loads and stores
        && (fld(w, 9, 5) == r || fld(w, 14, 10) == r || fld(w, 20, 16) == r)
    {
        return true;
    }
    false
}

// ------------------------------------------------ the LL/SC loop body
//
// Enough of A64 to run the few instructions a compiler puts between a
// load-exclusive and its store-exclusive (clang and GCC's atomic expansions:
// add/sub/logical/move/compare/select/bitfield/shift/multiply and the
// conditional branches out of the loop). No memory access: anything else is
// refused. Registers are a private copy; nothing reaches the thread until the
// caller commits it.
#[repr(C)]
#[derive(Clone, Copy)]
struct Ireg {
    x: [u64; 31],
    sp: u64,
    nzcv: u32, // N 8, Z 4, C 2, V 1
}

#[inline]
unsafe fn ird(r: *const Ireg, n: c_uint, sp: bool) -> u64 {
    if n == 31 {
        if sp {
            (*r).sp
        } else {
            0
        }
    } else {
        ptr::addr_of!((*r).x).cast::<u64>().add(n as usize).read()
    }
}

#[inline]
unsafe fn iwr(r: *mut Ireg, n: c_uint, mut v: u64, sf: bool, sp: bool) {
    if !sf {
        v &= 0xffff_ffff;
    }
    if n == 31 {
        if sp {
            (*r).sp = v;
        }
        return;
    }
    ptr::addr_of_mut!((*r).x)
        .cast::<u64>()
        .add(n as usize)
        .write(v);
}

fn cond_holds(cond: u32, f: u32) -> bool {
    let n = f & 8 != 0;
    let z = f & 4 != 0;
    let c = f & 2 != 0;
    let v = f & 1 != 0;
    let res = match cond >> 1 {
        0 => z,
        1 => c,
        2 => n,
        3 => v,
        4 => c && !z,
        5 => n == v,
        6 => n == v && !z,
        _ => true,
    };
    if (cond & 1) != 0 && cond != 15 {
        !res
    } else {
        res
    }
}

fn add_carry(x: u64, y: u64, c: u32, sf: bool, f: &mut u32) -> u64 {
    if sf {
        let us: u128 = (x as u128).wrapping_add(y as u128).wrapping_add(c as u128);
        let ss: i128 = (x as i64 as i128)
            .wrapping_add(y as i64 as i128)
            .wrapping_add(c as i128);
        let res = us as u64;
        *f = (((res >> 63) << 3)
            | (((res == 0) as u64) << 2)
            | (((res as u128 != us) as u64) << 1)
            | ((res as i64 as i128 != ss) as u64)) as u32;
        return res;
    }
    let us: u64 = (x as u32 as u64)
        .wrapping_add(y as u32 as u64)
        .wrapping_add(c as u64);
    let ss: i64 = (x as u32 as i32 as i64)
        .wrapping_add(y as u32 as i32 as i64)
        .wrapping_add(c as i64);
    let res = us as u32;
    *f = ((res >> 31) << 3)
        | (((res == 0) as u32) << 2)
        | (((res as u64 != us) as u32) << 1)
        | ((res as i32 as i64 != ss) as u32);
    res as u64
}

fn nz_of(res: u64, sf: bool) -> u32 {
    ((((if sf { res >> 63 } else { res >> 31 }) & 1) << 3) | (((res == 0) as u64) << 2)) as u32
}

fn shifted(mut v: u64, kind: u32, amt: u32, sf: bool) -> u64 {
    let size: u32 = if sf { 64 } else { 32 };
    if !sf {
        v &= 0xffff_ffff;
    }
    if amt != 0 {
        match kind {
            0 => v = v.wrapping_shl(amt),
            1 => v = v.wrapping_shr(amt),
            2 => {
                v = if sf {
                    (v as i64).wrapping_shr(amt) as u64
                } else {
                    ((v as u32 as i32).wrapping_shr(amt) as u32) as u64
                }
            }
            _ => v = v.wrapping_shr(amt) | v.wrapping_shl(size.wrapping_sub(amt)),
        }
    }
    if sf {
        v
    } else {
        v & 0xffff_ffff
    }
}

fn extended(mut v: u64, option: u32, amt: u32, sf: bool) -> u64 {
    let len: u32 = 8u32 << (option & 3);
    if len < 64 {
        v &= (1u64 << len) - 1;
        if option & 4 != 0 {
            v = sext(v, len) as u64;
        }
    }
    v = v.wrapping_shl(amt);
    if sf {
        v
    } else {
        v & 0xffff_ffff
    }
}

// DecodeBitMasks from the ARM ARM. Returns (wmask, tmask).
fn bit_masks(n: u32, imms: u32, immr: u32, immediate: bool, datasize: u32) -> Option<(u64, u64)> {
    let combined: u32 = (n << 6) | (!imms & 0x3f);
    if combined == 0 {
        return None;
    }
    let len: i32 = 31 - combined.leading_zeros() as i32;
    if len < 1 {
        return None;
    }
    let levels: u32 = (1u32 << len) - 1;
    if immediate && (imms & levels) == levels {
        return None;
    }
    let s = imms & levels;
    let r = immr & levels;
    let esize: u32 = 1u32 << len;
    let d = (s.wrapping_sub(r) & 0x3f) & levels;
    let emask: u64 = if esize == 64 {
        !0u64
    } else {
        (1u64 << esize) - 1
    };
    let mut welem: u64 = if s + 1 == 64 {
        !0u64
    } else {
        (1u64 << (s + 1)) - 1
    };
    let telem: u64 = if d + 1 == 64 {
        !0u64
    } else {
        (1u64 << (d + 1)) - 1
    };
    if r != 0 {
        welem = (welem.wrapping_shr(r) | welem.wrapping_shl(esize - r)) & emask;
    }
    let mut w: u64 = 0;
    let mut t: u64 = 0;
    let mut i: u32 = 0;
    while i < 64 {
        w |= welem.wrapping_shl(i);
        t |= telem.wrapping_shl(i);
        i += esize;
    }
    if datasize == 32 {
        w &= 0xffff_ffff;
        t &= 0xffff_ffff;
    }
    Some((w, t))
}

// One instruction. *next becomes the following pc. False: not interpreted.
unsafe fn interp(w: u32, pc: u64, r: *mut Ireg, next: &mut u64) -> bool {
    *next = pc.wrapping_add(4);
    let sf = (w >> 31) != 0;
    let rd = w & 31;
    let rn = fld(w, 9, 5);
    let rm = fld(w, 20, 16);
    let mut f: u32 = 0;

    if w == 0xd503_201f || w == 0xd503_203f {
        return true; // NOP, YIELD
    }
    let bar = w & 0xffff_f0ff;
    if bar == 0xd503_309f || bar == 0xd503_30bf || bar == 0xd503_30df {
        return true; // DSB, DMB, ISB
    }
    if (w & 0xff00_0010) == 0x5400_0000 {
        // B.cond
        if cond_holds(w & 15, (*r).nzcv) {
            *next = pc.wrapping_add(sext(fld(w, 23, 5) as u64, 19).wrapping_mul(4) as u64);
        }
        return true;
    }
    if (w & 0x7e00_0000) == 0x3400_0000 {
        // CBZ, CBNZ
        let mut v = ird(r, rd, false);
        if !sf {
            v &= 0xffff_ffff;
        }
        if (v != 0) == (fld(w, 24, 24) != 0) {
            *next = pc.wrapping_add(sext(fld(w, 23, 5) as u64, 19).wrapping_mul(4) as u64);
        }
        return true;
    }
    if (w & 0x7e00_0000) == 0x3600_0000 {
        // TBZ, TBNZ
        let bit = (fld(w, 31, 31) << 5) | fld(w, 23, 19);
        if ((ird(r, rd, false).wrapping_shr(bit) & 1) != 0) == (fld(w, 24, 24) != 0) {
            *next = pc.wrapping_add(sext(fld(w, 18, 5) as u64, 14).wrapping_mul(4) as u64);
        }
        return true;
    }
    if (w & 0x1f80_0000) == 0x1100_0000 {
        // ADD/SUB(S) immediate
        let op = fld(w, 30, 30) != 0;
        let s = fld(w, 29, 29) != 0;
        let imm: u64 = (fld(w, 21, 10) as u64) << (if fld(w, 22, 22) != 0 { 12 } else { 0 });
        let res = add_carry(
            ird(r, rn, true),
            if op { !imm } else { imm },
            op as u32,
            sf,
            &mut f,
        );
        if s {
            (*r).nzcv = f;
        }
        iwr(r, rd, res, sf, !s);
        return true;
    }
    if (w & 0x1f20_0000) == 0x0b00_0000 {
        // ADD/SUB(S) shifted register
        let kind = fld(w, 23, 22);
        let amt = fld(w, 15, 10);
        if kind == 3 || (!sf && amt >= 32) {
            return false;
        }
        let op = fld(w, 30, 30) != 0;
        let s = fld(w, 29, 29) != 0;
        let y = shifted(ird(r, rm, false), kind, amt, sf);
        let res = add_carry(
            ird(r, rn, false),
            if op { !y } else { y },
            op as u32,
            sf,
            &mut f,
        );
        if s {
            (*r).nzcv = f;
        }
        iwr(r, rd, res, sf, false);
        return true;
    }
    if (w & 0x1fe0_0000) == 0x0b20_0000 {
        // ADD/SUB(S) extended register
        let amt = fld(w, 12, 10);
        if amt > 4 {
            return false;
        }
        let op = fld(w, 30, 30) != 0;
        let s = fld(w, 29, 29) != 0;
        let y = extended(ird(r, rm, false), fld(w, 15, 13), amt, sf);
        let res = add_carry(
            ird(r, rn, true),
            if op { !y } else { y },
            op as u32,
            sf,
            &mut f,
        );
        if s {
            (*r).nzcv = f;
        }
        iwr(r, rd, res, sf, !s);
        return true;
    }
    if (w & 0x1fe0_fc00) == 0x1a00_0000 {
        // ADC/SBC(S)
        let op = fld(w, 30, 30) != 0;
        let s = fld(w, 29, 29) != 0;
        let y = ird(r, rm, false);
        let res = add_carry(
            ird(r, rn, false),
            if op { !y } else { y },
            ((*r).nzcv >> 1) & 1,
            sf,
            &mut f,
        );
        if s {
            (*r).nzcv = f;
        }
        iwr(r, rd, res, sf, false);
        return true;
    }
    if (w & 0x1f80_0000) == 0x1200_0000 {
        // logical immediate
        let opc = fld(w, 30, 29);
        let x = ird(r, rn, false);
        if !sf && fld(w, 22, 22) != 0 {
            return false;
        }
        let imm = match bit_masks(
            fld(w, 22, 22),
            fld(w, 15, 10),
            fld(w, 21, 16),
            true,
            if sf { 64 } else { 32 },
        ) {
            Some((wmask, _)) => wmask,
            None => return false,
        };
        let mut res = if opc == 1 {
            x | imm
        } else if opc == 2 {
            x ^ imm
        } else {
            x & imm
        };
        if !sf {
            res &= 0xffff_ffff;
        }
        if opc == 3 {
            (*r).nzcv = nz_of(res, sf);
        }
        iwr(r, rd, res, sf, opc != 3);
        return true;
    }
    if (w & 0x1f00_0000) == 0x0a00_0000 {
        // logical shifted register
        let opc = fld(w, 30, 29);
        let amt = fld(w, 15, 10);
        if !sf && amt >= 32 {
            return false;
        }
        let mut y = shifted(ird(r, rm, false), fld(w, 23, 22), amt, sf);
        let x = ird(r, rn, false);
        if fld(w, 21, 21) != 0 {
            y = !y;
        }
        let mut res = if opc == 1 {
            x | y
        } else if opc == 2 {
            x ^ y
        } else {
            x & y
        };
        if !sf {
            res &= 0xffff_ffff;
        }
        if opc == 3 {
            (*r).nzcv = nz_of(res, sf);
        }
        iwr(r, rd, res, sf, false);
        return true;
    }
    if (w & 0x1f80_0000) == 0x1280_0000 {
        // MOVN, MOVZ, MOVK
        let opc = fld(w, 30, 29);
        let hw = fld(w, 22, 21);
        if opc == 1 || (!sf && hw > 1) {
            return false;
        }
        let imm: u64 = (fld(w, 20, 5) as u64) << (16 * hw);
        let res = if opc == 0 {
            !imm
        } else if opc == 2 {
            imm
        } else {
            (ird(r, rd, false) & !(0xffffu64 << (16 * hw))) | imm
        };
        iwr(r, rd, res, sf, false);
        return true;
    }
    if (w & 0x1f80_0000) == 0x1300_0000 {
        // SBFM, BFM, UBFM
        let opc = fld(w, 30, 29);
        let rr = fld(w, 21, 16);
        let s = fld(w, 15, 10);
        let datasize: u32 = if sf { 64 } else { 32 };
        if opc == 3 || fld(w, 22, 22) != sf as u32 || (!sf && (rr >= 32 || s >= 32)) {
            return false;
        }
        let (wmask, tmask) = match bit_masks(fld(w, 22, 22), s, rr, false, datasize) {
            Some(masks) => masks,
            None => return false,
        };
        let mut src = ird(r, rn, false);
        let mut dst = ird(r, rd, false);
        if !sf {
            src &= 0xffff_ffff;
            dst &= 0xffff_ffff;
        }
        let mut rot = if rr != 0 {
            src.wrapping_shr(rr) | src.wrapping_shl(datasize - rr)
        } else {
            src
        };
        if !sf {
            rot &= 0xffff_ffff;
        }
        let res = if opc == 1 {
            let bot = (dst & !wmask) | (rot & wmask);
            (dst & !tmask) | (bot & tmask)
        } else if opc == 2 {
            rot & wmask & tmask
        } else {
            let top: u64 = if (src.wrapping_shr(s) & 1) != 0 { !0u64 } else { 0 };
            (top & !tmask) | (rot & wmask & tmask)
        };
        iwr(r, rd, res, sf, false);
        return true;
    }
    if (w & 0x3fe0_0800) == 0x1a80_0000 {
        // CSEL, CSINC, CSINV, CSNEG
        let res;
        if cond_holds(fld(w, 15, 12), (*r).nzcv) {
            res = ird(r, rn, false);
        } else {
            let mut alt = ird(r, rm, false);
            if fld(w, 30, 30) != 0 {
                alt = !alt;
            }
            if fld(w, 10, 10) != 0 {
                alt = alt.wrapping_add(1);
            }
            res = alt;
        }
        iwr(r, rd, res, sf, false);
        return true;
    }
    if (w & 0x3fe0_0410) == 0x3a40_0000 {
        // CCMN, CCMP
        if cond_holds(fld(w, 15, 12), (*r).nzcv) {
            let op = fld(w, 30, 30) != 0;
            let y: u64 = if fld(w, 11, 11) != 0 {
                fld(w, 20, 16) as u64
            } else {
                ird(r, rm, false)
            };
            add_carry(
                ird(r, rn, false),
                if op { !y } else { y },
                op as u32,
                sf,
                &mut f,
            );
            (*r).nzcv = f;
        } else {
            (*r).nzcv = w & 15;
        }
        return true;
    }
    if (w & 0x7fe0_f000) == 0x1ac0_2000 {
        // LSLV, LSRV, ASRV, RORV
        let amt = (ird(r, rm, false) & (if sf { 63 } else { 31 })) as u32;
        iwr(
            r,
            rd,
            shifted(ird(r, rn, false), fld(w, 11, 10), amt, sf),
            sf,
            false,
        );
        return true;
    }
    if (w & 0x7fe0_0000) == 0x1b00_0000 {
        // MADD, MSUB
        let prod = ird(r, rn, false).wrapping_mul(ird(r, rm, false));
        let a = ird(r, fld(w, 14, 10), false);
        iwr(
            r,
            rd,
            if fld(w, 15, 15) != 0 {
                a.wrapping_sub(prod)
            } else {
                a.wrapping_add(prod)
            },
            sf,
            false,
        );
        return true;
    }
    false
}

// The load-exclusive a store-exclusive at pc pairs with: the nearest preceding
// LDXR/LDAXR (LDXP/LDAXP) of the same size and base, with no other exclusive,
// ordered or CAS instruction in between. The words between, in order.
unsafe fn find_ldxr(
    insn: u32,
    pc: u64,
    lpc: &mut u64,
    lw: &mut u32,
    between: *mut u32,
    nb: *mut c_int,
    why: *mut *const c_char,
) -> bool {
    let size = insn >> 30;
    let pair = fld(insn, 21, 21);
    let rn = fld(insn, 9, 5);
    let mut back = [0u32; 16];
    let back = back.as_mut_ptr();
    let mut k: usize = 1;
    while k <= 16 {
        let mut w: u32 = 0;
        if !read_word(pc.wrapping_sub(4u64.wrapping_mul(k as u64)), &mut w) {
            refuse(why, c"store-exclusive: code before it unreadable");
            return false;
        }
        if (w & 0x3f00_0000) != 0x0800_0000 {
            back.add(k - 1).write(w);
            k += 1;
            continue;
        }
        let is_casp = fld(w, 21, 21) != 0 && (w >> 30) < 2 && fld(w, 23, 23) == 0;
        if fld(w, 23, 23) != 0
            || fld(w, 22, 22) == 0
            || is_casp
            || fld(w, 21, 21) != pair
            || (w >> 30) != size
            || fld(w, 9, 5) != rn
        {
            refuse(why, c"store-exclusive: no matching load-exclusive");
            return false;
        }
        let lt = w & 31;
        let lt2 = fld(w, 14, 10);
        if lt == 31 || (pair != 0 && lt2 == 31) {
            refuse(why, c"store-exclusive: load-exclusive discards its value");
            return false;
        }
        *lpc = pc.wrapping_sub(4u64.wrapping_mul(k as u64));
        *lw = w;
        *nb = (k - 1) as c_int;
        let mut i: usize = 0;
        while i < k - 1 {
            between.add(i).write(back.add(k - 2 - i).read());
            i += 1;
        }
        return true;
    }
    refuse(
        why,
        c"store-exclusive: no load-exclusive within 16 instructions",
    );
    false
}

// The expected value of a store-exclusive at pc. Two ways to know it:
//
//  1. The load-exclusive's destination still holds what it loaded (nothing in
//     between can have written it, nor the base): compare-and-swap against it.
//  2. The loop retries from the load-exclusive (`cbnz Ws, <ldxr>` right after
//     the store): then running it again from there with the registers as they
//     are is exactly the guest's own retry after a failed store-exclusive, so
//     the handler performs that retry -- the load as an atomic load, the body
//     interpreted, the store as a compare-and-swap, again until it succeeds
//     or the body branches out. Compilers emit this form and reuse the loaded
//     register (clang: ldaxr w8; add w8, w8, w0; stlxr w10, w8), which the
//     first way cannot use.
unsafe fn exclusive_plan(
    insn: u32,
    pc: u64,
    ss: *const Tstate,
    m: *mut LxrtStoremu,
    why: *mut *const c_char,
) -> bool {
    let size = insn >> 30;
    let pair = fld(insn, 21, 21) != 0;
    let rn = fld(insn, 9, 5);
    let mut lpc: u64 = 0;
    let mut lw: u32 = 0;
    let between = ptr::addr_of_mut!((*m).between).cast::<u32>();
    if !find_ldxr(
        insn,
        pc,
        &mut lpc,
        &mut lw,
        between,
        ptr::addr_of_mut!((*m).nbetween),
        why,
    ) {
        return false;
    }
    let lt = lw & 31;
    let lt2 = fld(lw, 14, 10);
    let nbetween = (*m).nbetween;
    let mut kept = true;
    let mut i: c_int = 0;
    while i < nbetween {
        let w = between.add(i as usize).read();
        if clobbers(w, lt) || (pair && clobbers(w, lt2)) || clobbers(w, rn) {
            kept = false;
        }
        i += 1;
    }
    if kept {
        if !pair {
            (*m).cmp[0] = xr(ss, lt) & mask_n(1u32 << size);
        } else if size == 3 {
            (*m).cmp[0] = xr(ss, lt);
            (*m).cmp[1] = xr(ss, lt2);
        } else {
            (*m).cmp[0] = (xr(ss, lt) & 0xffff_ffff) | (xr(ss, lt2) << 32);
        }
        return true;
    }
    let mut next: u32 = 0;
    if !read_word(pc.wrapping_add(4), &mut next)
        || (next & 0xff00_001f) != (0x3500_0000 | fld(insn, 20, 16))
        || pc
            .wrapping_add(4)
            .wrapping_add(sext(fld(next, 23, 5) as u64, 19).wrapping_mul(4) as u64)
            != lpc
    {
        refuse(
            why,
            c"store-exclusive: loaded value overwritten, and no cbnz retry to the load",
        );
        return false;
    }
    let mut scratch: Ireg = Ireg {
        x: [0; 31],
        sp: 0,
        nzcv: 0,
    };
    let mut i: c_int = 0;
    while i < nbetween {
        let w = between.add(i as usize).read();
        let mut nx: u64 = 0;
        let branch = (w & 0x1c00_0000) == 0x1400_0000;
        if (!branch && clobbers(w, rn))
            || !interp(
                w,
                lpc.wrapping_add(4).wrapping_add(4u64.wrapping_mul(i as u64)),
                &mut scratch,
                &mut nx,
            )
        {
            refuse(
                why,
                c"store-exclusive: loop body writes the base or is not interpretable",
            );
            return false;
        }
        i += 1;
    }
    (*m).block = true;
    (*m).ldxr_pc = lpc;
    (*m).stxr_pc = pc;
    (*m).lt = lt;
    (*m).lt2 = lt2;
    true
}

/// Decode the instruction word at pc against the thread state in uap (a
/// ucontext_t *). Returns 0 and fills *m when it is a store this module can
/// perform; otherwise -1 with *why saying what it is. Nothing is written.
#[no_mangle]
pub unsafe extern "C" fn lxrt_storemu_decode(
    insn: u32,
    pc: u64,
    uap: *mut c_void,
    m: *mut LxrtStoremu,
    why: *mut *const c_char,
) -> c_int {
    let mc = mcontext(uap);
    let ss = mc.add(MC_SS_OFFSET).cast::<Tstate>().cast_const();
    let v = mc.add(MC_NS_V_OFFSET).cast::<u128>().cast_const();
    ptr::write_bytes(m, 0, 1);
    (*m).wb = -1;
    *why = ptr::null();
    let rt = insn & 31;
    let rn = fld(insn, 9, 5);
    let data = ptr::addr_of_mut!((*m).data).cast::<u8>();

    // DC ZVA, Xt: zero the naturally aligned block that holds Xt.
    if (insn & 0xffff_ffe0) == 0xd50b_7420 {
        let dczid: u64;
        asm!("mrs {}, dczid_el0", out(reg) dczid, options(nomem, nostack, preserves_flags));
        if dczid & 16 != 0 {
            return refuse(why, c"DC ZVA prohibited");
        }
        let block: u32 = 4u32.wrapping_shl((dczid & 15) as u32);
        (*m).kind = LXRT_SM_ZVA;
        (*m).len = block;
        (*m).addr = xr(ss, rt) & !(block.wrapping_sub(1) as u64);
        return 0;
    }

    // Exclusive, ordered and compare-and-swap: size 001000 o2 L o1 Rs o0 Rt2 Rn Rt.
    if (insn & 0x3f00_0000) == 0x0800_0000 {
        let size = insn >> 30;
        let o2 = fld(insn, 23, 23) != 0;
        let l = fld(insn, 22, 22) != 0;
        let o1 = fld(insn, 21, 21) != 0;
        let rs = fld(insn, 20, 16);
        let rt2 = fld(insn, 14, 10);
        let base = xr_sp(ss, rn);
        if o2 && !o1 {
            // STLR, STLLR (L=1: loads)
            if l {
                return refuse(why, c"load-acquire");
            }
            let n: u32 = 1u32 << size;
            (*m).kind = LXRT_SM_RELEASE;
            (*m).addr = base;
            (*m).elem = n;
            (*m).len = n;
            put_le(data, xr(ss, rt), n);
        } else if o2 && o1 {
            // CAS{A}{L}{B,H}
            if rt2 != 31 {
                return refuse(why, c"unallocated CAS form");
            }
            let n: u32 = 1u32 << size;
            (*m).kind = LXRT_SM_CAS;
            (*m).addr = base;
            (*m).len = n;
            (*m).rs = rs;
            (*m).cmp[0] = xr(ss, rs) & mask_n(n);
            (*m).val[0] = xr(ss, rt) & mask_n(n);
        } else if o1 && size < 2 {
            // CASP{A}{L}
            if rt2 != 31 || (rs & 1) != 0 || (rt & 1) != 0 {
                return refuse(why, c"unallocated CASP form");
            }
            let x = size == 1;
            (*m).kind = LXRT_SM_CASP;
            (*m).addr = base;
            (*m).len = if x { 16 } else { 8 };
            (*m).rs = rs;
            if x {
                (*m).cmp[0] = xr(ss, rs);
                (*m).cmp[1] = xr(ss, rs + 1);
                (*m).val[0] = xr(ss, rt);
                (*m).val[1] = xr(ss, rt + 1);
            } else {
                (*m).pair32 = true;
                (*m).cmp[0] = (xr(ss, rs) & 0xffff_ffff) | (xr(ss, rs + 1) << 32);
                (*m).val[0] = (xr(ss, rt) & 0xffff_ffff) | (xr(ss, rt + 1) << 32);
            }
        } else if !l {
            // STXR, STLXR, STXP, STLXP
            (*m).kind = LXRT_SM_EXCL;
            (*m).addr = base;
            (*m).rs = rs;
            if !o1 {
                (*m).len = 1u32 << size;
                (*m).val[0] = xr(ss, rt) & mask_n((*m).len);
            } else if size == 3 {
                (*m).len = 16;
                (*m).val[0] = xr(ss, rt);
                (*m).val[1] = xr(ss, rt2);
            } else {
                (*m).len = 8;
                (*m).pair32 = true;
                (*m).val[0] = (xr(ss, rt) & 0xffff_ffff) | (xr(ss, rt2) << 32);
            }
            (*m).st_rt = rt;
            (*m).st_rt2 = rt2;
            (*m).rn = rn;
            if !exclusive_plan(insn, pc, ss, m, why) {
                return -1;
            }
        } else {
            return refuse(why, c"load-exclusive");
        }
        // These fault on a misaligned address natively (alignment is checked
        // before permission); never perform one the hardware would refuse.
        if (*m).addr & ((*m).len.wrapping_sub(1) as u64) != 0 {
            return refuse(why, c"misaligned atomic or ordered access");
        }
        return 0;
    }

    // STLUR{B,H} (FEAT_LRCPC2): size 011001 opc 0 imm9 00 Rn Rt, opc 00.
    if (insn & 0x3f20_0c00) == 0x1900_0000 {
        if fld(insn, 23, 22) != 0 {
            return refuse(why, c"LDAPUR");
        }
        let n: u32 = 1u32 << (insn >> 30);
        (*m).kind = LXRT_SM_RELEASE;
        (*m).addr = xr_sp(ss, rn).wrapping_add(sext(fld(insn, 20, 12) as u64, 9) as u64);
        (*m).elem = n;
        (*m).len = n;
        put_le(data, xr(ss, rt), n);
        if (*m).addr & ((n - 1) as u64) != 0 {
            return refuse(why, c"misaligned STLUR");
        }
        return 0;
    }

    // Pairs: opc 101 V type L imm7 Rt2 Rn Rt (type 00 STNP, 01 post, 10 offset, 11 pre).
    if (insn & 0x3a00_0000) == 0x2800_0000 {
        let opc = insn >> 30;
        let vreg = fld(insn, 26, 26) != 0;
        let kind = fld(insn, 24, 23);
        let rt2 = fld(insn, 14, 10);
        if fld(insn, 22, 22) != 0 {
            return refuse(why, c"load pair");
        }
        let n: u32;
        if vreg {
            if opc == 3 {
                return refuse(why, c"unallocated pair");
            }
            n = 4u32 << opc;
        } else {
            if opc & 1 != 0 {
                return refuse(
                    why,
                    if opc == 1 {
                        c"STGP (MTE)"
                    } else {
                        c"unallocated pair"
                    },
                );
            }
            n = if opc == 2 { 8 } else { 4 };
        }
        let off: i64 = sext(fld(insn, 21, 15) as u64, 7).wrapping_mul(n as i64);
        let base = xr_sp(ss, rn);
        (*m).addr = if kind == 1 {
            base
        } else {
            base.wrapping_add(off as u64)
        };
        if kind == 1 || kind == 3 {
            (*m).wb = rn as c_int;
            (*m).wb_val = base.wrapping_add(off as u64);
        }
        if vreg {
            put_lane(data, v.add(rt as usize).read(), 0, n);
            put_lane(data.add(n as usize), v.add(rt2 as usize).read(), 0, n);
        } else {
            put_le(data, xr(ss, rt), n);
            put_le(data.add(n as usize), xr(ss, rt2), n);
        }
        (*m).kind = LXRT_SM_STORE;
        (*m).len = 2 * n;
        (*m).elem = n;
        return 0;
    }

    // LSE atomics: size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt.
    if (insn & 0x3f20_0c00) == 0x3820_0000 {
        let o3 = fld(insn, 15, 15) != 0;
        let opc = fld(insn, 14, 12);
        let n: u32 = 1u32 << (insn >> 30);
        if o3 && opc != 0 {
            return refuse(
                why,
                if opc == 4 {
                    c"LDAPR"
                } else {
                    c"ST64B/LD64B family"
                },
            );
        }
        (*m).kind = LXRT_SM_ATOMIC;
        (*m).op = if o3 { 8 } else { opc as c_int }; // 0..7 LD<op>, 8 SWP
        (*m).addr = xr_sp(ss, rn);
        (*m).len = n;
        (*m).rt = rt;
        (*m).val[0] = xr(ss, fld(insn, 20, 16)) & mask_n(n);
        if (*m).addr & ((n - 1) as u64) != 0 {
            return refuse(why, c"misaligned atomic");
        }
        return 0;
    }

    // Single register: size 111 V 0x opc ...
    if (insn & 0x3a00_0000) == 0x3800_0000 {
        let size = insn >> 30;
        let vreg = fld(insn, 26, 26) != 0;
        let opc = fld(insn, 23, 22);
        let n: u32;
        if !vreg {
            if opc != 0 {
                return refuse(why, c"load or prefetch");
            }
            n = 1u32 << size;
        } else if opc == 0 {
            n = 1u32 << size;
        } else if opc == 2 && size == 0 {
            n = 16;
        } else {
            return refuse(why, c"SIMD&FP load");
        }
        let base = xr_sp(ss, rn);
        if fld(insn, 24, 24) != 0 {
            // unsigned offset
            (*m).addr = base.wrapping_add((fld(insn, 21, 10) as u64).wrapping_mul(n as u64));
        } else if fld(insn, 21, 21) == 0 {
            let imm: i64 = sext(fld(insn, 20, 12) as u64, 9);
            match fld(insn, 11, 10) {
                0 => {
                    // STUR
                    (*m).addr = base.wrapping_add(imm as u64);
                }
                1 => {
                    // post-index
                    (*m).addr = base;
                    (*m).wb = rn as c_int;
                    (*m).wb_val = base.wrapping_add(imm as u64);
                }
                2 => {
                    // STTR
                    if vreg {
                        return refuse(why, c"unallocated");
                    }
                    (*m).addr = base.wrapping_add(imm as u64);
                }
                _ => {
                    // pre-index
                    (*m).addr = base.wrapping_add(imm as u64);
                    (*m).wb = rn as c_int;
                    (*m).wb_val = (*m).addr;
                }
            }
        } else if fld(insn, 11, 10) == 2 {
            // register offset
            let option = fld(insn, 15, 13);
            let s = fld(insn, 12, 12) != 0;
            if option & 2 == 0 {
                return refuse(why, c"unallocated extend");
            }
            let mut off = xr(ss, fld(insn, 20, 16));
            if option == 2 {
                off = off as u32 as u64; // UXTW
            } else if option == 6 {
                off = off as u32 as i32 as i64 as u64; // SXTW
            }
            let mut shift: u32 = 0;
            if s {
                let mut t = n;
                while t > 1 {
                    shift += 1;
                    t >>= 1;
                }
            }
            (*m).addr = base.wrapping_add(off.wrapping_shl(shift));
        } else {
            return refuse(why, c"unallocated or pointer-authenticated load");
        }
        if vreg {
            put_lane(data, v.add(rt as usize).read(), 0, n);
        } else {
            put_le(data, xr(ss, rt), n);
        }
        (*m).kind = LXRT_SM_STORE;
        (*m).elem = n;
        (*m).len = n;
        return 0;
    }

    // ST1-ST4 (multiple structures): 0 Q 0011000 0 0 00000 opcode size Rn Rt,
    // post-index 0 Q 0011001 0 0 Rm opcode size Rn Rt.
    if (insn & 0xbfff_0000) == 0x0c00_0000 || (insn & 0xbfe0_0000) == 0x0c80_0000 {
        let post = fld(insn, 23, 23) != 0;
        let q = fld(insn, 30, 30) != 0;
        let opcode = fld(insn, 15, 12);
        let size = fld(insn, 11, 10);
        let rm = fld(insn, 20, 16);
        let (rpt, selem): (u32, u32) = match opcode {
            0x0 => (1, 4),
            0x2 => (4, 1),
            0x4 => (1, 3),
            0x6 => (3, 1),
            0x7 => (1, 1),
            0x8 => (1, 2),
            0xa => (2, 1),
            _ => return refuse(why, c"unallocated ST multiple"),
        };
        if size == 3 && !q && selem != 1 {
            return refuse(why, c"unallocated ST multiple");
        }
        let ebytes: u32 = 1u32 << size;
        let elements: u32 = (if q { 16u32 } else { 8u32 }) >> size;
        let mut offs: u32 = 0;
        let mut r: u32 = 0;
        while r < rpt {
            let mut e: u32 = 0;
            while e < elements {
                let mut tt = (rt + r) % 32;
                let mut s: u32 = 0;
                while s < selem {
                    put_lane(data.add(offs as usize), v.add(tt as usize).read(), e, ebytes);
                    offs += ebytes;
                    tt = (tt + 1) % 32;
                    s += 1;
                }
                e += 1;
            }
            r += 1;
        }
        let base = xr_sp(ss, rn);
        (*m).kind = LXRT_SM_STORE;
        (*m).addr = base;
        (*m).len = offs;
        (*m).elem = ebytes;
        if post {
            (*m).wb = rn as c_int;
            (*m).wb_val = base.wrapping_add(if rm == 31 { offs as u64 } else { xr(ss, rm) });
        }
        return 0;
    }

    // ST1-ST4 (single structure): 0 Q 0011010 0 R 00000 opcode S size Rn Rt,
    // post-index 0 Q 0011011 0 R Rm opcode S size Rn Rt.
    if (insn & 0xbfdf_0000) == 0x0d00_0000 || (insn & 0xbfc0_0000) == 0x0d80_0000 {
        let post = fld(insn, 23, 23) != 0;
        let q = fld(insn, 30, 30);
        let rbit = fld(insn, 21, 21);
        let rm = fld(insn, 20, 16);
        let opcode = fld(insn, 15, 13);
        let s = fld(insn, 12, 12);
        let size = fld(insn, 11, 10);
        let mut scale = opcode >> 1;
        let selem = (((opcode & 1) << 1) | rbit) + 1;
        let index: u32;
        match scale {
            0 => {
                index = (q << 3) | (s << 2) | size;
            }
            1 => {
                if size & 1 != 0 {
                    return refuse(why, c"unallocated ST single");
                }
                index = (q << 2) | (s << 1) | (size >> 1);
            }
            2 => {
                if size & 2 != 0 {
                    return refuse(why, c"unallocated ST single");
                }
                if size & 1 == 0 {
                    index = (q << 1) | s;
                } else {
                    if s != 0 {
                        return refuse(why, c"unallocated ST single");
                    }
                    index = q;
                    scale = 3;
                }
            }
            _ => {
                return refuse(why, c"unallocated ST single (replicate is load-only)");
            }
        }
        let ebytes: u32 = 1u32 << scale;
        let mut offs: u32 = 0;
        let mut k: u32 = 0;
        while k < selem {
            put_lane(
                data.add(offs as usize),
                v.add(((rt + k) % 32) as usize).read(),
                index,
                ebytes,
            );
            offs += ebytes;
            k += 1;
        }
        let base = xr_sp(ss, rn);
        (*m).kind = LXRT_SM_STORE;
        (*m).addr = base;
        (*m).len = offs;
        (*m).elem = ebytes;
        if post {
            (*m).wb = rn as c_int;
            (*m).wb_val = base.wrapping_add(if rm == 31 { offs as u64 } else { xr(ss, rm) });
        }
        return 0;
    }

    refuse(why, c"not a store")
}

// ---------------------------------------------------------------- memory

unsafe fn store_unit(a: u64, d: *const u8, n: u32, order: Ordering) {
    let mut x: u64 = 0;
    if n <= 8 && (n & n.wrapping_sub(1)) == 0 && (a & (n.wrapping_sub(1) as u64)) == 0 {
        ptr::copy_nonoverlapping(d, ptr::addr_of_mut!(x).cast::<u8>(), n as usize);
        match n {
            1 => AtomicU8::from_ptr(a as usize as *mut u8).store(x as u8, order),
            2 => AtomicU16::from_ptr(a as usize as *mut u16).store(x as u16, order),
            4 => AtomicU32::from_ptr(a as usize as *mut u32).store(x as u32, order),
            _ => AtomicU64::from_ptr(a as usize as *mut u64).store(x, order),
        }
        return;
    }
    if n == 16 && (a & 15) == 0 {
        // An aligned 16-byte STP is single-copy atomic with FEAT_LSE2.
        let lo = d.cast::<u64>().read_unaligned();
        let hi = d.add(8).cast::<u64>().read_unaligned();
        asm!(
            "stp {lo}, {hi}, [{a}]",
            lo = in(reg) lo,
            hi = in(reg) hi,
            a = in(reg) a,
            options(nostack, preserves_flags),
        );
        return;
    }
    let p = a as usize as *mut u8;
    let mut i: u32 = 0;
    while i < n {
        p.add(i as usize).write_volatile(d.add(i as usize).read());
        i += 1;
    }
}

unsafe fn load_n(a: u64, n: u32) -> u64 {
    match n {
        1 => AtomicU8::from_ptr(a as usize as *mut u8).load(Ordering::SeqCst) as u64,
        2 => AtomicU16::from_ptr(a as usize as *mut u16).load(Ordering::SeqCst) as u64,
        4 => AtomicU32::from_ptr(a as usize as *mut u32).load(Ordering::SeqCst) as u64,
        _ => AtomicU64::from_ptr(a as usize as *mut u64).load(Ordering::SeqCst),
    }
}

// Compare-and-swap of n (1/2/4/8) bytes; *expected becomes the old value.
unsafe fn cas_n(a: u64, n: u32, expected: &mut u64, nv: u64) -> bool {
    match n {
        1 => {
            match AtomicU8::from_ptr(a as usize as *mut u8).compare_exchange(
                *expected as u8,
                nv as u8,
                Ordering::SeqCst,
                Ordering::SeqCst,
            ) {
                Ok(old) => {
                    *expected = old as u64;
                    true
                }
                Err(old) => {
                    *expected = old as u64;
                    false
                }
            }
        }
        2 => {
            match AtomicU16::from_ptr(a as usize as *mut u16).compare_exchange(
                *expected as u16,
                nv as u16,
                Ordering::SeqCst,
                Ordering::SeqCst,
            ) {
                Ok(old) => {
                    *expected = old as u64;
                    true
                }
                Err(old) => {
                    *expected = old as u64;
                    false
                }
            }
        }
        4 => {
            match AtomicU32::from_ptr(a as usize as *mut u32).compare_exchange(
                *expected as u32,
                nv as u32,
                Ordering::SeqCst,
                Ordering::SeqCst,
            ) {
                Ok(old) => {
                    *expected = old as u64;
                    true
                }
                Err(old) => {
                    *expected = old as u64;
                    false
                }
            }
        }
        _ => {
            match AtomicU64::from_ptr(a as usize as *mut u64).compare_exchange(
                *expected,
                nv,
                Ordering::SeqCst,
                Ordering::SeqCst,
            ) {
                Ok(old) => {
                    *expected = old;
                    true
                }
                Err(old) => {
                    *expected = old;
                    false
                }
            }
        }
    }
}

// 16-byte compare-and-swap (CASPAL, as the guest's own CASP would be).
unsafe fn cas_16(a: u64, lo: &mut u64, hi: &mut u64, nlo: u64, nhi: u64) -> bool {
    let mut x0: u64 = *lo;
    let mut x1: u64 = *hi;
    let elo = *lo;
    let ehi = *hi;
    asm!(
        ".arch_extension lse",
        "caspal x0, x1, x2, x3, [{a}]",
        a = in(reg) a,
        inout("x0") x0,
        inout("x1") x1,
        in("x2") nlo,
        in("x3") nhi,
        options(nostack, preserves_flags),
    );
    *lo = x0;
    *hi = x1;
    x0 == elo && x1 == ehi
}

fn rmw_value(op: c_int, old: u64, arg: u64, n: u32) -> u64 {
    let so = sext(old, 8u32.wrapping_mul(n));
    let sa = sext(arg, 8u32.wrapping_mul(n));
    match op {
        0 => old.wrapping_add(arg), // LDADD
        1 => old & !arg,            // LDCLR
        2 => old ^ arg,             // LDEOR
        3 => old | arg,             // LDSET
        4 => {
            // LDSMAX
            if so > sa {
                old
            } else {
                arg
            }
        }
        5 => {
            // LDSMIN
            if so < sa {
                old
            } else {
                arg
            }
        }
        6 => {
            // LDUMAX
            if old > arg {
                old
            } else {
                arg
            }
        }
        7 => {
            // LDUMIN
            if old < arg {
                old
            } else {
                arg
            }
        }
        _ => arg, // SWP
    }
}

// The retry of an LL/SC loop (exclusive_plan, way 2), from the load-exclusive
// with the thread's registers as they are: atomic load, body interpreted,
// compare-and-swap, until the swap succeeds or the body branches out of the
// loop. The result is committed as if the guest had run it: all registers,
// the flags, and pc after the store-exclusive (status 0) or at the branch
// target. After 1000 lost races the store-exclusive is reported failed (it
// may always fail) and the guest's own loop takes over.
unsafe fn perform_retry(m: *const LxrtStoremu, ss: *mut Tstate) {
    let mut base: Ireg = Ireg {
        x: [0; 31],
        sp: 0,
        nzcv: 0,
    };
    let mut i: u32 = 0;
    while i < 31 {
        iwr(&mut base, i, xr(ss, i), true, false);
        i += 1;
    }
    base.sp = (*ss).sp;
    base.nzcv = (*ss).cpsr >> 28;
    let n = (*m).len;
    let addr = (*m).addr;
    let ldxr_pc = (*m).ldxr_pc;
    let stxr_pc = (*m).stxr_pc;
    let pair32 = (*m).pair32;
    let between = ptr::addr_of!((*m).between).cast::<u32>();
    let mut attempt: c_int = 0;
    loop {
        let mut r: Ireg = base;
        let lo: u64;
        let mut hi: u64 = 0;
        if n == 16 {
            // An aligned 16-byte LDP is single-copy atomic with FEAT_LSE2.
            asm!(
                "ldp {lo}, {hi}, [{a}]",
                lo = out(reg) lo,
                hi = out(reg) hi,
                a = in(reg) addr,
                options(nostack, preserves_flags, readonly),
            );
        } else {
            lo = load_n(addr, n);
        }
        if pair32 {
            iwr(&mut r, (*m).lt, lo & 0xffff_ffff, true, false);
            iwr(&mut r, (*m).lt2, lo >> 32, true, false);
        } else {
            iwr(&mut r, (*m).lt, lo, true, false);
            if n == 16 {
                iwr(&mut r, (*m).lt2, hi, true, false);
            }
        }
        let mut pc = ldxr_pc.wrapping_add(4);
        let mut next: u64 = 0;
        let mut steps: c_int = 0;
        while pc != stxr_pc {
            if pc <= ldxr_pc || pc > stxr_pc {
                break; // branched out of the loop
            }
            steps += 1;
            if steps > 64 {
                break;
            }
            let idx = (pc.wrapping_sub(ldxr_pc) / 4).wrapping_sub(1) as usize;
            interp(between.add(idx).read(), pc, &mut r, &mut next);
            pc = next;
        }
        let at_store = pc == stxr_pc;
        if at_store {
            let a = ird(&r, (*m).st_rt, false);
            let b = ird(&r, (*m).st_rt2, false);
            let ok;
            if n == 16 {
                let mut elo = lo;
                let mut ehi = hi;
                ok = cas_16(addr, &mut elo, &mut ehi, a, b);
            } else {
                let mut e = lo;
                let nv = if pair32 {
                    (a & 0xffff_ffff) | (b << 32)
                } else {
                    a & mask_n(n)
                };
                ok = cas_n(addr, n, &mut e, nv);
            }
            if !ok && attempt < 1000 {
                attempt += 1;
                continue; // lost a race: the guest would retry too
            }
            iwr(&mut r, (*m).rs, if ok { 0 } else { 1 }, false, false);
            pc = stxr_pc.wrapping_add(4);
        }
        let mut i: u32 = 0;
        while i < 31 {
            set_xr(ss, i, ird(&r, i, false));
            i += 1;
        }
        (*ss).sp = r.sp;
        (*ss).cpsr = ((*ss).cpsr & 0x0fff_ffff) | r.nzcv.wrapping_shl(28);
        (*ss).pc = pc;
        return;
    }
}

/// Perform a decoded store: the memory access (the caller has made the bytes
/// writable), the result and writeback registers, and pc += 4.
#[no_mangle]
pub unsafe extern "C" fn lxrt_storemu_perform(m: *const LxrtStoremu, uap: *mut c_void) {
    let ss = mcontext(uap).add(MC_SS_OFFSET).cast::<Tstate>();
    let a = (*m).addr;
    let len = (*m).len;
    let data = ptr::addr_of!((*m).data).cast::<u8>();
    fence(Ordering::SeqCst);
    match (*m).kind {
        LXRT_SM_STORE | LXRT_SM_RELEASE => {
            let order = if (*m).kind == LXRT_SM_RELEASE {
                Ordering::Release
            } else {
                Ordering::Relaxed
            };
            let mut unit = if (*m).elem != 0 { (*m).elem } else { len };
            if len == 16 && (a & 15) == 0 {
                unit = 16; // an aligned STP of X registers is one 128-bit access (LSE2)
            }
            let mut off: u32 = 0;
            while off < len {
                store_unit(a.wrapping_add(off as u64), data.add(off as usize), unit, order);
                off = off.wrapping_add(unit);
            }
        }
        LXRT_SM_ZVA => {
            static ZERO: [u8; 8] = [0; 8];
            let step = if len >= 8 { 8 } else { len };
            let mut off: u32 = 0;
            while off < len {
                store_unit(a.wrapping_add(off as u64), ZERO.as_ptr(), step, Ordering::Relaxed);
                off = off.wrapping_add(step);
            }
        }
        LXRT_SM_ATOMIC => {
            let n = len;
            let mut old = load_n(a, n);
            loop {
                let mut e = old;
                if cas_n(
                    a,
                    n,
                    &mut e,
                    rmw_value((*m).op, old, (*m).val[0], n) & mask_n(n),
                ) {
                    break;
                }
                old = e;
            }
            set_xr(ss, (*m).rt, old & mask_n(n));
        }
        LXRT_SM_CAS => {
            let mut e = (*m).cmp[0];
            cas_n(a, len, &mut e, (*m).val[0]);
            set_xr(ss, (*m).rs, e & mask_n(len));
        }
        LXRT_SM_CASP => {
            if (*m).pair32 {
                let mut e = (*m).cmp[0];
                cas_n(a, 8, &mut e, (*m).val[0]);
                set_xr(ss, (*m).rs, e & 0xffff_ffff);
                set_xr(ss, (*m).rs.wrapping_add(1), e >> 32);
            } else {
                let mut lo = (*m).cmp[0];
                let mut hi = (*m).cmp[1];
                cas_16(a, &mut lo, &mut hi, (*m).val[0], (*m).val[1]);
                set_xr(ss, (*m).rs, lo);
                set_xr(ss, (*m).rs.wrapping_add(1), hi);
            }
        }
        LXRT_SM_EXCL => {
            if (*m).block {
                perform_retry(m, ss);
                fence(Ordering::SeqCst);
                return; // registers and pc set by the retry
            }
            let ok;
            if len == 16 {
                let mut lo = (*m).cmp[0];
                let mut hi = (*m).cmp[1];
                ok = cas_16(a, &mut lo, &mut hi, (*m).val[0], (*m).val[1]);
            } else {
                let mut e = (*m).cmp[0];
                ok = cas_n(a, len, &mut e, (*m).val[0]);
            }
            set_xr(ss, (*m).rs, if ok { 0 } else { 1 });
        }
        _ => {}
    }
    fence(Ordering::SeqCst);
    if (*m).wb >= 0 {
        set_xr_sp(ss, (*m).wb as c_uint, (*m).wb_val);
    }
    (*ss).pc = (*ss).pc.wrapping_add(4);
}
