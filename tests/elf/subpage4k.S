/* A 4 KiB-aligned image (linked with max-page-size 4096) whose last code
 * page and first data page share one 16 KiB host page. The writer loop sits
 * 20 KiB away on another host page and calls tail_fn on the shared one, so the
 * page flips RW/RX on every iteration.
 *
 * With an argument, by its first letter:
 *   s  tail_self_write stores from the shared page into it, 10 times (the
 *      W/X livelock before runtime/storemu.c): exit 0, 4 if the count is off.
 *   c  a store from the shared page into its own read-execute code page:
 *      Linux faults; so must the runtime (no exit code of this program).
 *   p  the code page is made read-write-execute (mprotect), and a store from
 *      the shared page writes `mov x0,#42; mov x8,#93; svc #0; udf #0` into
 *      it, then runs it: exit 42 only if the runtime rewrote that new svc.
 *   f  the same store, from the far page (the flip path): exit 42 likewise.
 * An svc the runtime did not rewrite runs as a Darwin syscall; x16 = 20
 * makes that getpid, and the udf after it then raises SIGILL. */
.text
.global _start
.type _start, %function
_start:
    ldr x19, [sp]
    adrp x20, counter
    add x20, x20, :lo12:counter
    cmp x19, #2
    b.lt 1f
    ldr x9, [sp, #16]                   // argv[1]
    ldrb w9, [x9]
    cmp w9, #'s'
    b.eq selfwrite
    cmp w9, #'c'
    b.eq codewrite
    cmp w9, #'p'
    b.eq patch
    cmp w9, #'f'
    b.eq patchfar
    mov x0, #9
    b exit
1:
    mov x21, #1000
writer:
    ldr x0, [x20]
    add x0, x0, #1
    str x0, [x20]
    bl tail_fn
    cmp x0, #3
    b.ne fail1
    subs x21, x21, #1
    b.ne writer
    ldr x0, [x20]
    cmp x0, #1000
    b.ne fail2
    adrp x1, ro_word
    add x1, x1, :lo12:ro_word
    ldr w0, [x1]
    movz w2, #0xbeef
    movk w2, #0xdead, lsl #16
    cmp w0, w2
    b.ne fail3
    mov x0, #0
    b exit
selfwrite:
    bl tail_self_write
    ldr x0, [x20]
    cmp x0, #10
    b.ne fail4
    mov x0, #0
    b exit
codewrite:
    bl tail_code_write
    mov x0, #0
    b exit
patch:
    bl tail_patch
    b exit                              // only reached if mprotect failed
fail1: mov x0, #1
    b exit
fail2: mov x0, #2
    b exit
fail3: mov x0, #3
    b exit
fail4: mov x0, #4
exit:
    mov x8, #93
    svc #0

// mprotect(the page of patch_area, 4096, RWX), then the four words in x4:x5.
.macro patch_setup
    adr x0, patch_area
    and x0, x0, #~0xfff
    mov x1, #4096
    mov x2, #7
    mov x8, #226
    svc #0
    cbnz x0, 8f
    adr x3, patch_area
    movz x4, #0x0540
    movk x4, #0xd280, lsl #16           // mov x0, #42
    movk x4, #0x0ba8, lsl #32
    movk x4, #0xd280, lsl #48           // mov x8, #93
    movz x5, #0x0001
    movk x5, #0xd400, lsl #16           // svc #0, then udf #0
.endm
.macro patch_run
    dc cvau, x3
    dsb ish
    ic ivau, x3
    dsb ish
    isb
    mov x16, #20
    br x3
8:  mov x0, #5
.endm

patchfar:
    patch_setup
    stp x4, x5, [x3]                    // from the far page: the flip path
    patch_run
    b exit
    .skip 20480

// These stores run from the host page they write.
tail_self_write:
    mov x2, #10
1:
    ldr x0, [x20]
    add x0, x0, #1
    str x0, [x20]
    subs x2, x2, #1
    b.ne 1b
    ret
tail_code_write:
    adr x0, tail_fn
    str wzr, [x0]
    ret
tail_patch:
    patch_setup
    stp x4, x5, [x3]                    // emulated: into an executable page
    patch_run
    ret
tail_fn:
    mov x0, #3
    ret
.p2align 4
patch_area:
    .word 0, 0, 0, 0
.size _start, . - _start

.section .rodata
ro_word: .word 0xdeadbeef
.data
.p2align 3
counter: .quad 0
