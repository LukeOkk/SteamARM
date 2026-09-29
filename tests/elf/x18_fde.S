/* The x18 pass touches only words inside .eh_frame FDEs. The two x18
 * instructions in _start are code; `table` sits in .text outside every FDE and
 * must read back unchanged. Under LXRT_X18_ALL_TEXT=x18_fde the same words are
 * rewritten into branches and this program must fail. */
.text
.global _start
.type _start, %function
_start:
    .cfi_startproc
    mov x18, #5
    add x0, x18, #1
    cmp x0, #6
    b.ne fail
    adr x1, table
    ldr w2, [x1]
    movz w3, #0x0252
    movk w3, #0x9100, lsl #16
    cmp w2, w3
    b.ne fail
    ldr w2, [x1, #4]
    movz w3, #0x03e0
    movk w3, #0xaa12, lsl #16
    cmp w2, w3
    b.ne fail
    mov x0, #0
    mov x8, #93
    svc #0
    .cfi_endproc
.size _start, . - _start

table:
    .word 0x91000252
    .word 0xaa1203e0

.type fail, %function
fail:
    .cfi_startproc
    mov x0, #1
    mov x8, #93
    svc #0
    .cfi_endproc
.size fail, . - fail
