/* The x18 forms the planner refused until 0.3.4, executed: ldar/stlr (ordered,
 * not exclusive), mrs/msr of NZCV/FPCR/FPSR, and sp computed from x18 --
 * growing, shrinking, equal, misaligned, a switch to another stack. Two values
 * are kept in x18 across page faults on fresh .bss pages (Darwin zeroes x18 on
 * every exception return; a rewritten svc saves it, a fault does not), so
 * LXRT_NO_X18=1 must fail this program.
 * Exit codes: 1..5 ordered loads/stores; 6..8 NZCV/FPCR/FPSR;
 * 9..15 SP grow, shrink, equal, offset, extended, switch, misaligned;
 * 99 means the case-C trampoline (a new sp within 16 bytes above the saved
 * pair) failed to trap. */
.text
.global _start
.type _start, %function
_start:
    ldr x21, [sp]
    mov x19, sp
    adrp x16, word_slot
    add x16, x16, :lo12:word_slot
    movz w2, #0x3344
    movk w2, #0x1122, lsl #16
    str w2, [x16]
    mov x18, x16
    adrp x9, fresh1
    add x9, x9, :lo12:fresh1
    bl fault_pages
    ldar w18, [x18]
    mov x0, x18
    cmp x0, x2
    b.ne fail1
    mov x18, #0
    ldar w18, [x16]
    mov x0, x18
    cmp x0, x2
    b.ne fail2
    mov w2, #0xa5
    strb w2, [x16]
    mov x18, x16
    ldarb w18, [x18]
    mov x0, x18
    cmp x0, #0xa5
    b.ne fail3

    adrp x1, quad_slot
    add x1, x1, :lo12:quad_slot
    movz x18, #0x5678
    movk x18, #0x1234, lsl #16
    movk x18, #0xface, lsl #32
    movk x18, #0xfeed, lsl #48
    stlr x18, [x1]
    ldr x0, [x1]
    mov x2, x18
    cmp x0, x2
    b.ne fail4
    mov x2, #0x765
    str x2, [sp, #-16]!
    ldar x18, [sp]
    ldr x2, [sp], #16
    mov x0, x18
    cmp x0, x2
    b.ne fail5

    mov x3, #1
    cmp x3, #2
    mrs x18, nzcv
    b.mi 1f
    b fail6
1:
    mrs x4, nzcv
    mov x0, x18
    cmp x0, x4
    b.ne fail6
    mov x18, #0x60000000
    msr nzcv, x18
    b.eq 2f
    b fail7
2:
    b.cs 3f
    b fail7
3:
    mrs x5, nzcv
    mov x0, #0x60000000
    cmp x5, x0
    b.ne fail7

    mrs x20, fpcr
    mrs x18, fpcr
    mrs x6, fpcr
    mov x0, x18
    cmp x0, x6
    b.ne fail8
    orr x18, x20, #0x00c00000
    msr fpcr, x18
    mrs x7, fpcr
    and x7, x7, #0x00c00000
    mov x0, #0x00c00000
    cmp x7, x0
    b.ne fail8
    msr fpcr, x20
    mov x18, #0
    msr fpsr, x18
    mrs x7, fpsr
    cbnz x7, fail8

    sub x18, sp, #0x100
    adrp x9, fresh2
    add x9, x9, :lo12:fresh2
    bl fault_pages
    mov x0, #0xa0
    mov x1, #0xa1
    mov x2, #0xa2
    mov x6, #0x20000000
    msr nzcv, x6
    mov sp, x18
    mrs x4, nzcv
    mov x3, sp
    sub x6, x19, #0x100
    cmp x3, x6
    b.ne fail9
    cmp x0, #0xa0
    b.ne fail9
    cmp x1, #0xa1
    b.ne fail9
    cmp x2, #0xa2
    b.ne fail9
    mov x5, x18
    cmp x5, x6
    b.ne fail9
    and x4, x4, #0xf0000000
    mov x7, #0x20000000
    cmp x4, x7
    b.ne fail9
    mov sp, x19

    sub sp, sp, #0x200
    mov x22, sp
    add x18, sp, #0x200
    mov x0, #0xa0
    mov x1, #0xa1
    mov sp, x18
    mov x3, sp
    cmp x3, x19
    b.ne fail10
    cmp x0, #0xa0
    b.ne fail10
    cmp x1, #0xa1
    b.ne fail10
    mov x18, sp
    mov sp, x18
    mov x3, sp
    cmp x3, x19
    b.ne fail11
    cmp x0, #0xa0
    b.ne fail11
    cmp x1, #0xa1
    b.ne fail11

    sub x18, x19, #0x100
    add sp, x18, #0x40
    mov x3, sp
    sub x6, x19, #0xc0
    cmp x3, x6
    b.ne fail12
    mov sp, x19
    sub sp, x18, #0x40
    mov x3, sp
    sub x6, x19, #0x140
    cmp x3, x6
    b.ne fail12
    mov sp, x19
    movn x18, #0x7f
    add sp, sp, x18
    mov x3, sp
    sub x6, x19, #0x80
    cmp x3, x6
    b.ne fail13
    mov x18, #0x80
    add sp, sp, x18
    mov x3, sp
    cmp x3, x19
    b.ne fail13

    adrp x18, stack_end
    add x18, x18, :lo12:stack_end
    mov sp, x18
    mov x0, #0xa0
    mov x1, #0xa1
    stp x0, x1, [sp, #-16]!
    mov x0, #0
    mov x1, #0
    ldp x0, x1, [sp], #16
    mov sp, x19
    cmp x0, #0xa0
    b.ne fail14
    cmp x1, #0xa1
    b.ne fail14

    sub x18, x19, #0x108
    mov x0, #0xa0
    mov x1, #0xa1
    mov sp, x18
    mov x3, sp
    mov sp, x19
    sub x6, x19, #0x108
    cmp x3, x6
    b.ne fail15
    cmp x0, #0xa0
    b.ne fail15
    cmp x1, #0xa1
    b.ne fail15
    sub sp, sp, #0x200
    add x18, sp, #0x108
    mov sp, x18
    mov x3, sp
    mov sp, x19
    sub x6, x19, #0xf8
    cmp x3, x6
    b.ne fail15
    cmp x0, #0xa0
    b.ne fail15
    cmp x1, #0xa1
    b.ne fail15

    cmp x21, #2
    b.lt success
    sub x18, x19, #8
    mov sp, x18
    mov x0, #99
    b exit
success:
    mov x0, #0
    b exit
fail1: mov x0, #1
    b exit
fail2: mov x0, #2
    b exit
fail3: mov x0, #3
    b exit
fail4: mov x0, #4
    b exit
fail5: mov x0, #5
    b exit
fail6: mov x0, #6
    b exit
fail7: mov x0, #7
    b exit
fail8: mov x0, #8
    b exit
fail9: mov x0, #9
    b exit
fail10: mov x0, #10
    b exit
fail11: mov x0, #11
    b exit
fail12: mov x0, #12
    b exit
fail13: mov x0, #13
    b exit
fail14: mov x0, #14
    b exit
fail15: mov x0, #15
exit:
    mov x8, #93
    svc #0
.size _start, . - _start


// First write to each 4 KiB of an untouched 64 KiB .bss region: page faults.
.type fault_pages, %function
fault_pages:
    mov x10, #16
1:  str xzr, [x9]
    add x9, x9, #4096
    subs x10, x10, #1
    b.ne 1b
    ret
.size fault_pages, . - fault_pages

.data
.p2align 3
word_slot: .word 0
    .word 0
quad_slot: .quad 0
.bss
.p2align 14
fresh1: .skip 65536
fresh2: .skip 65536
.p2align 4
stack_buffer: .skip 16384
stack_end:
