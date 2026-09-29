/* x18 stores and loads at sp offsets the planner cannot absorb in the
 * immediate (Steam's stp x18, x17, [sp, #0x1f8]; str at 0x7ff8; stur past 239;
 * a register offset): the trampoline addresses through a copy of the original
 * sp. Plain loads check what the rewritten stores wrote. Exit code = check. */
.text
.global _start
.type _start, %function
_start:
    sub sp, sp, #0x400
    movz x18, #1234
    movz x17, #5678
    stp x18, x17, [sp, #0x1f8]
    ldr x0, [sp, #0x1f8]
    cmp x0, #1234
    b.ne fail1
    ldr x0, [sp, #0x200]
    movz x1, #5678
    cmp x0, x1
    b.ne fail2
    str xzr, [sp, #0x1f8]
    str xzr, [sp, #0x200]
    movz x0, #1234
    str x0, [sp, #0x1f8]
    str x17, [sp, #0x200]
    ldp x18, x17, [sp, #0x1f8]
    mov x0, x18
    cmp x0, #1234
    b.ne fail3
    movz x1, #5678
    cmp x17, x1
    b.ne fail4
    sub sp, sp, #0x8, lsl #12
    movz x18, #4321
    str x18, [sp, #0x7ff8]
    ldr x0, [sp, #0x7ff8]
    movz x1, #4321
    cmp x0, x1
    b.ne fail5
    stur x18, [sp, #248]
    mov x18, #0
    ldur x18, [sp, #248]
    mov x0, x18
    cmp x0, x1
    b.ne fail6
    add x18, sp, #1, lsl #12
    add x0, sp, #0x1000
    mov x1, x18
    cmp x0, x1
    b.ne fail7
    movz x0, #1234
    str x0, [sp, #0x1f8]
    mov x3, #0x1f8
    ldr x18, [sp, x3]
    mov x0, x18
    cmp x0, #1234
    b.ne fail8
    add sp, sp, #0x8, lsl #12
    add sp, sp, #0x400
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
exit:
    mov x8, #93
    svc #0
.size _start, . - _start
