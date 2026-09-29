/* A 4 KiB-aligned image (linked with max-page-size 4096) whose last code
 * page and first data page share one 16 KiB host page. The writer loop sits
 * 20 KiB away on another host page and calls tail_fn on the shared one, so the
 * page flips RW/RX on every iteration. With an argument, tail_self_write
 * stores from the shared page itself: the known W/X livelock. */
.text
.global _start
.type _start, %function
_start:
    ldr x19, [sp]
    adrp x20, counter
    add x20, x20, :lo12:counter
    cmp x19, #2
    b.ge selfwrite
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
    mov x0, #0
    b exit
fail1: mov x0, #1
    b exit
fail2: mov x0, #2
    b exit
fail3: mov x0, #3
exit:
    mov x8, #93
    svc #0
    .skip 20480

// This store runs from the host page it writes and exposes the known livelock.
tail_self_write:
    mov x2, #10
1:
    ldr x0, [x20]
    add x0, x0, #1
    str x0, [x20]
    subs x2, x2, #1
    b.ne 1b
    ret
tail_fn:
    mov x0, #3
    ret
.size _start, . - _start

.section .rodata
ro_word: .word 0xdeadbeef
.data
.p2align 3
counter: .quad 0
