/* blr/br/ret through x18: the BLR trampoline exposes the guest return
 * address in x30, keeps x17 and sp, and the virtual x18 still holds the target
 * afterwards, also across a nested call. Exit code = failed check. */
.text
.global _start
.type _start, %function
_start:
    mov x17, #0x1717
    mov x19, sp
    adr x18, target
    blr x18
returned:
    cmp x0, #42
    b.ne fail1
    adr x1, inner
    mov x2, x18
    cmp x2, x1
    b.ne fail2
    adr x18, landing
    br x18
    b fail3
landing:
    mov x9, #0x1717
    cmp x17, x9
    b.ne fail3
    mov x0, sp
    cmp x0, x19
    b.ne fail4
    adr x18, after_ret
    ret x18
    b fail5
after_ret:
    mov x0, #0
    b exit

target:
    adr x1, returned
    cmp x30, x1
    b.ne fail6
    mov x9, #0x1717
    cmp x17, x9
    b.ne fail7
    mov x1, sp
    cmp x1, x19
    b.ne fail8
    adr x1, target
    mov x2, x18
    cmp x2, x1
    b.ne fail9
    mov x20, x30
    adr x18, inner
    blr x18
back_inner:
    cmp x0, #7
    b.ne fail10
    adr x1, inner
    mov x2, x18
    cmp x2, x1
    b.ne fail11
    mov x30, x20
    add x0, x0, #35
    ret
inner:
    mov x0, #7
    ret
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
exit:
    mov x8, #93
    svc #0
.size _start, . - _start
