// Per-site syscall trampoline, and the entry thunk that hands control to the
// guest image.
//
// Stage 1 measured that `svc` cannot be trapped on Darwin -- XNU ignores the
// SVC immediate and dispatches on x16 -- so every syscall site in the guest
// image is rewritten to branch here instead. See benchmarks/README.md.
//
// The Linux aarch64 syscall ABI clobbers only x0 (the return value) and
// preserves everything else, so the trampoline has to save and restore x1..x18
// across a C call that is free to clobber them. x30 is saved too, because the
// site was a plain `svc` and the guest does not expect its link register to
// change.
//
// The template is copied once per site and two words are patched: the literal
// holding the dispatcher's address, and the final branch back to the
// instruction after the original svc.

    .text
    .balign 8

    .globl _lxrt_tramp_template
_lxrt_tramp_template:
    // The whole general-purpose file is saved, not just the caller-saved half.
    // clone's child inherits every register the parent held at the syscall, and
    // glibc's clone wrapper relies on it: it stashes the thread function and
    // its argument in registers before `svc` and calls them in the child. A
    // snapshot that stopped at x18 left the child branching through zero.
    sub     sp, sp, #784
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    // And the vector registers, FPSR and the condition flags (NZCV), above
    // the general-purpose block
    // (the dispatcher's argument, struct lxrt_regs, stays at sp). The Linux
    // kernel preserves the whole FP/SIMD file across a syscall, so a compiler
    // may keep values in v0-v31 across an inline `svc` (glibc's
    // INTERNAL_SYSCALL, any freestanding code); the dispatcher is Darwin C
    // code and libsystem's memcpy/memset clobber them. MEASURED: a raw
    // Wayland client kept a msghdr template in q2 across its syscalls and
    // sent iovlen 0 (EMSGSIZE) after a few calls
    // (benchmarks/stage27-android-display.txt). The same holds for NZCV: a
    // counted loop compiles to `subs; svc; b.ne`, and the dispatcher's own
    // comparisons changed the flags under it (tests/elf/simd_syscall.c).
    // FPCR is left alone: nothing in the runtime changes the rounding mode
    // or the trap enables. Cost (MEASURED, median of 10 runs of 200,000
    // getpid): 18.8 -> 23.5 ns per syscall; the vector block alone and the
    // flags alone each cost about the same 5 ns, so the price is mostly the
    // larger frame, not the stores.
    add     x16, sp, #256
    st1     {v0.2d, v1.2d, v2.2d, v3.2d}, [x16], #64
    st1     {v4.2d, v5.2d, v6.2d, v7.2d}, [x16], #64
    st1     {v8.2d, v9.2d, v10.2d, v11.2d}, [x16], #64
    st1     {v12.2d, v13.2d, v14.2d, v15.2d}, [x16], #64
    st1     {v16.2d, v17.2d, v18.2d, v19.2d}, [x16], #64
    st1     {v20.2d, v21.2d, v22.2d, v23.2d}, [x16], #64
    st1     {v24.2d, v25.2d, v26.2d, v27.2d}, [x16], #64
    st1     {v28.2d, v29.2d, v30.2d, v31.2d}, [x16]
    mrs     x16, fpsr
    mrs     x17, nzcv
    str     x16, [sp, #768]
    str     x17, [sp, #776]

    // The guest's resume address. `svc` leaves no trace of where it was, and
    // clone's child has to start executing at the instruction after it, so the
    // rewriter patches it in per site and the trampoline hands it over.
    ldr     x16, Llxrt_ret
    str     x16, [sp, #248]

    mov     x0, sp                  // the saved block is the dispatcher's arg
    ldr     x16, Llxrt_lit
    blr     x16

    // FPSR is written back only when the dispatcher changed it (the usual
    // case is that it did not); NZCV always.
    ldr     x16, [sp, #768]
    mrs     x17, fpsr
    cmp     x16, x17
    b.eq    1f
    msr     fpsr, x16
1:  ldr     x17, [sp, #776]
    msr     nzcv, x17
    add     x16, sp, #256
    ld1     {v0.2d, v1.2d, v2.2d, v3.2d}, [x16], #64
    ld1     {v4.2d, v5.2d, v6.2d, v7.2d}, [x16], #64
    ld1     {v8.2d, v9.2d, v10.2d, v11.2d}, [x16], #64
    ld1     {v12.2d, v13.2d, v14.2d, v15.2d}, [x16], #64
    ld1     {v16.2d, v17.2d, v18.2d, v19.2d}, [x16], #64
    ld1     {v20.2d, v21.2d, v22.2d, v23.2d}, [x16], #64
    ld1     {v24.2d, v25.2d, v26.2d, v27.2d}, [x16], #64
    ld1     {v28.2d, v29.2d, v30.2d, v31.2d}, [x16]

    // Restore in reverse, x0/x1 last: the dispatcher wrote the syscall result
    // into the x0 slot and it must survive.
    ldr     x30,      [sp, #240]
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8,  x9,  [sp, #64]
    ldp     x6,  x7,  [sp, #48]
    ldp     x4,  x5,  [sp, #32]
    ldp     x2,  x3,  [sp, #16]
    ldp     x0,  x1,  [sp, #0]
    add     sp, sp, #784

Llxrt_branch:
    b       Llxrt_branch            // patched: back to the instruction after svc

    .balign 8
Llxrt_lit:
    .quad   0                       // patched: address of lxrt_dispatch
Llxrt_ret:
    .quad   0                       // patched: guest address after the svc
    .globl _lxrt_tramp_template_end
_lxrt_tramp_template_end:

// Offsets into the template, so the C side never hand-encodes an instruction.
    .section __DATA,__const
    .balign 8
    .globl _lxrt_tramp_size
_lxrt_tramp_size:
    .quad _lxrt_tramp_template_end - _lxrt_tramp_template
    .globl _lxrt_tramp_lit_off
_lxrt_tramp_lit_off:
    .quad Llxrt_lit - _lxrt_tramp_template
    .globl _lxrt_tramp_br_off
_lxrt_tramp_br_off:
    .quad Llxrt_branch - _lxrt_tramp_template
    .globl _lxrt_tramp_ret_off
_lxrt_tramp_ret_off:
    .quad Llxrt_ret - _lxrt_tramp_template

// void lxrt_enter(uint64_t entry, void *sp) -- hand control to the guest.
//
// Linux hands a fresh process a stack with argc at [sp] and x0 zeroed (glibc
// treats a non-zero x0 as an atexit handler from the dynamic loader). Every
// other register is cleared so the guest cannot observe host state.
    .text
    .globl _lxrt_enter
    .balign 4
_lxrt_enter:
    mov     x16, x0                 // entry
    mov     sp,  x1
    mov     x0,  xzr
    mov     x1,  xzr
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x29, xzr
    mov     x30, xzr
    br      x16

// ---------------------------------------------------------------- TLS
//
// Darwin does NOT preserve TPIDR_EL0 across a context switch: a syscall keeps
// it, but any preemption or block restores a kernel-managed value (measured,
// benchmarks/stage3-tls.txt). Linux aarch64 keeps its thread pointer there, so
// guest TLS cannot live in that register -- every access has to be rewritten,
// exactly like `svc`.
//
// The replacement is the Darwin TSD slot array, which pthread_getspecific
// itself indexes off TPIDRRO_EL0: preserved per thread, and 2.34 ns to read.
//
// Both templates are assembled with x0 as the operand register; the rewriter
// ORs the real register number into Rt/Rn and patches the TSD byte offset.

    .text
    .balign 4

// mrs xT, tpidr_el0  ->  xT = guest TLS. Clobbers nothing but xT: no stack, no
// call, four instructions.
    .globl _lxrt_tlsrd_template
_lxrt_tlsrd_template:
    mrs     x0, tpidrro_el0
    and     x0, x0, #0xfffffffffffffff8   // low 3 bits carry the CPU number
Lrd_ldr:
    ldr     x0, [x0, #0]                  // patched: TSD slot byte offset
Lrd_br:
    b       Lrd_br                        // patched: back to the next instruction
    .globl _lxrt_tlsrd_template_end
_lxrt_tlsrd_template_end:

// msr tpidr_el0, xT  ->  guest TLS = xT. Needs one scratch register, so x16
// (IP0) is borrowed and restored; there are only a handful of these per image.
    .globl _lxrt_tlswr_template
_lxrt_tlswr_template:
    str     x16, [sp, #-16]!
    mrs     x16, tpidrro_el0
    and     x16, x16, #0xfffffffffffffff8
Lwr_str:
    str     x0, [x16, #0]                 // patched: register and TSD offset
    ldr     x16, [sp], #16
Lwr_br:
    b       Lwr_br                        // patched: back to the next instruction
    .globl _lxrt_tlswr_template_end
_lxrt_tlswr_template_end:

    .section __DATA,__const
    .balign 8
    .globl _lxrt_tlsrd_size
_lxrt_tlsrd_size:  .quad _lxrt_tlsrd_template_end - _lxrt_tlsrd_template
    .globl _lxrt_tlsrd_ldr_off
_lxrt_tlsrd_ldr_off: .quad Lrd_ldr - _lxrt_tlsrd_template
    .globl _lxrt_tlsrd_br_off
_lxrt_tlsrd_br_off:  .quad Lrd_br  - _lxrt_tlsrd_template
    .globl _lxrt_tlswr_size
_lxrt_tlswr_size:  .quad _lxrt_tlswr_template_end - _lxrt_tlswr_template
    .globl _lxrt_tlswr_str_off
_lxrt_tlswr_str_off: .quad Lwr_str - _lxrt_tlswr_template
    .globl _lxrt_tlswr_br_off
_lxrt_tlswr_br_off:  .quad Lwr_br  - _lxrt_tlswr_template

// void lxrt_thread_enter(const struct lxrt_regs *saved, uint64_t sp)
//
// Start a cloned guest thread. Linux's child resumes at the instruction after
// the svc with the parent's entire register file intact except x0, which is 0,
// and sp pointing at the stack the parent supplied.
    .text
    .globl _lxrt_thread_enter
    .balign 4
_lxrt_thread_enter:
    mov     sp,  x1
    mov     x1,  x0                 // keep the block pointer out of the way
    ldr     x16, [x1, #248]         // pc_after
    ldp     x2,  x3,  [x1, #16]
    ldp     x4,  x5,  [x1, #32]
    ldp     x6,  x7,  [x1, #48]
    ldp     x8,  x9,  [x1, #64]
    ldp     x10, x11, [x1, #80]
    ldp     x12, x13, [x1, #96]
    ldp     x14, x15, [x1, #112]
    ldp     x18, x19, [x1, #144]
    ldp     x20, x21, [x1, #160]
    ldp     x22, x23, [x1, #176]
    ldp     x24, x25, [x1, #192]
    ldp     x26, x27, [x1, #208]
    ldp     x28, x29, [x1, #224]
    ldr     x30,      [x1, #240]
    ldr     x17,      [x1, #136]    // x17 last of the scratch pair
    ldr     x1,       [x1, #8]      // and finally the guest's own x1
    mov     x0,  xzr                // Linux: the child sees clone() return 0
    br      x16

// void lxrt_run_on_stack(uint64_t sp, void (*fn)(void *), void *arg)
// Continue on another stack; fn must not return. A guest thread's exit
// (thread.c) leaves the guest's stack this way before waking the joiner,
// which may unmap that stack the moment it wakes.
    .text
    .globl _lxrt_run_on_stack
    .balign 4
_lxrt_run_on_stack:
    mov     sp,  x0
    mov     x16, x1
    mov     x0,  x2
    mov     x29, xzr
    mov     x30, xzr
    blr     x16
    brk     #0x1

// ---------------------------------------------------------------- signals


// void lxrt_call_guest_handler(handler, sig, info, uc, new_sp)
//
// Run a guest signal handler on the frame the runtime built for it, nested
// inside the host handler, and come back here when it returns. sp is moved
// to the frame for the call (the handler's `ret` comes back through lr) and
// restored afterwards. x19 holds the host sp across the call: callee-saved,
// so the guest handler preserves it.
    .text
    .globl _lxrt_call_guest_handler
    .balign 4
_lxrt_call_guest_handler:
    stp     x29, x30, [sp, #-32]!
    stp     x19, x20, [sp, #16]
    mov     x29, sp
    mov     x19, sp
    mov     x9,  x0                 // handler
    mov     x0,  x1                 // sig
    mov     x1,  x2                 // info
    mov     x2,  x3                 // uc
    mov     sp,  x4                 // the guest frame
    blr     x9
    mov     sp,  x19
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

// Where a guest signal handler returns to. Linux points lr at a restorer that
// issues rt_sigreturn; `svc` cannot be used for that here, so lr points into
// the runtime directly. The guest's sp still addresses the frame, which is what
// rt_sigreturn needs.
    .text
    .globl _lxrt_sigreturn_entry
    .balign 4
_lxrt_sigreturn_entry:
    mov     x0, sp
    b       _lxrt_do_sigreturn

// void lxrt_restore_regs(const uint64_t ctx[36 + 64])
//
// Resume an interrupted guest context. ctx[0..30] are x0..x30, ctx[31] is sp,
// ctx[32] is pc, ctx[33] pstate, ctx[34] fpsr, ctx[35] fpcr, and from byte 288
// the 32 vector registers. The block must NOT live on the stack: sp moves before the last
// loads, and reading below the new sp is exactly where a second signal would
// build its frame.
//
// ONE DEVIATION, and it is unavoidable in userspace: x16 is left holding the
// resume address instead of the guest's value. Branching needs a register, and
// every other one has to be restored first. AAPCS64 reserves x16 (IP0) as
// intra-procedure scratch that no call preserves, so the exposure is a signal
// arriving inside a PLT veneer. The kernel does not have this problem because
// it finishes with `eret`.
    .globl _lxrt_restore_regs
    .balign 4
_lxrt_restore_regs:
    // Status words and vector registers first: they need scratch registers
    // that the GPR restore below reclaims last.
    ldr     x17, [x0, #264]         // pstate: NZCV (and nothing else EL0 may set)
    msr     nzcv, x17
    ldr     x17, [x0, #272]         // fpsr
    msr     fpsr, x17
    ldr     x17, [x0, #280]         // fpcr
    msr     fpcr, x17
    add     x17, x0, #288           // 32 vector registers, 16 bytes each
    ldp     q0,  q1,  [x17], #32
    ldp     q2,  q3,  [x17], #32
    ldp     q4,  q5,  [x17], #32
    ldp     q6,  q7,  [x17], #32
    ldp     q8,  q9,  [x17], #32
    ldp     q10, q11, [x17], #32
    ldp     q12, q13, [x17], #32
    ldp     q14, q15, [x17], #32
    ldp     q16, q17, [x17], #32
    ldp     q18, q19, [x17], #32
    ldp     q20, q21, [x17], #32
    ldp     q22, q23, [x17], #32
    ldp     q24, q25, [x17], #32
    ldp     q26, q27, [x17], #32
    ldp     q28, q29, [x17], #32
    ldp     q30, q31, [x17], #32
    ldr     x16, [x0, #256]         // pc, held until the final branch
    ldr     x17, [x0, #248]         // guest sp
    mov     sp,  x17
    ldr     x17, [x0, #136]         // guest x17
    ldp     x1,  x2,  [x0, #8]
    ldp     x3,  x4,  [x0, #24]
    ldp     x5,  x6,  [x0, #40]
    ldp     x7,  x8,  [x0, #56]
    ldp     x9,  x10, [x0, #72]
    ldp     x11, x12, [x0, #88]
    ldp     x13, x14, [x0, #104]
    ldr     x15,      [x0, #120]
    ldp     x18, x19, [x0, #144]
    ldp     x20, x21, [x0, #160]
    ldp     x22, x23, [x0, #176]
    ldp     x24, x25, [x0, #192]
    ldp     x26, x27, [x0, #208]
    ldp     x28, x29, [x0, #224]
    ldr     x30,      [x0, #240]
    ldr     x0,       [x0, #0]      // x0 last: it is the block pointer
    br      x16

// ---------------------------------------------------------------- CTR_EL0
//
// mrs Xt, ctr_el0 -> xT = a synthesised cache-type register. Darwin traps the
// real read at EL0 even for its own code, and every icache flush performs one.
// Two instructions materialise the constant; the rewriter patches the register
// number and both immediate halves.
    .text
    .balign 4
    .globl _lxrt_ctr_template
_lxrt_ctr_template:
    movz    x0, #0
    movk    x0, #0, lsl #16
Lctr_br:
    b       Lctr_br                 // patched: back to the next instruction
    .globl _lxrt_ctr_template_end
_lxrt_ctr_template_end:

    .section __DATA,__const
    .balign 8
    .globl _lxrt_ctr_size
_lxrt_ctr_size:   .quad _lxrt_ctr_template_end - _lxrt_ctr_template
    .globl _lxrt_ctr_br_off
_lxrt_ctr_br_off: .quad Lctr_br - _lxrt_ctr_template

// ------------------------------------------------------- ID system registers
//
// mrs Xt, <ID_AA64*|MIDR_EL1> -> xT = a synthesised value. Darwin traps every
// one of these at EL0, for its own code too, and FEX reads eleven of them to
// decide which instructions it may emit. Four instructions materialise a full
// 64-bit constant; the rewriter patches the register number and all four
// immediate quarters.
    .text
    .balign 4
    .globl _lxrt_sysreg_template
_lxrt_sysreg_template:
    movz    x0, #0
    movk    x0, #0, lsl #16
    movk    x0, #0, lsl #32
    movk    x0, #0, lsl #48
Lsysreg_br:
    b       Lsysreg_br              // patched: back to the next instruction
    .globl _lxrt_sysreg_template_end
_lxrt_sysreg_template_end:

    .section __DATA,__const
    .balign 8
    .globl _lxrt_sysreg_size
_lxrt_sysreg_size:   .quad _lxrt_sysreg_template_end - _lxrt_sysreg_template
    .globl _lxrt_sysreg_br_off
_lxrt_sysreg_br_off: .quad Lsysreg_br - _lxrt_sysreg_template

// ------------------------------------------------- JIT execute-mode stub
//
// A thread that fetches from a MAP_JIT page while in write mode cannot be
// switched to execute mode from the fault handler: measured on macOS 27, a
// handler's flip to execute does not survive sigreturn when the thread was
// interrupted in write mode (the flip to write does). runtime/jit.c saves
// the faulting context and sends the thread here instead; the flip happens
// in ordinary context, then brk hands control back to the handler, which
// restores the saved context -- now in execute mode.
    .text
    .balign 4
    .globl _lxrt_jit_exec_stub
_lxrt_jit_exec_stub:
    mov     x0, #1
    bl      _pthread_jit_write_protect_np
    .globl _lxrt_jit_exec_stub_trap
_lxrt_jit_exec_stub_trap:
    brk     #0x4c58
