/* Exercise Linux x18 as an indirect call and branch under the Darwin runtime.
 * The callee checks that BLR exposed the original guest return PC in x30. */
.text
.global _start
.type _start, %function
_start:
    adr x18, target
    blr x18
after_call:
    cmp x0, #42
    b.ne fail
    adr x18, after_branch
    br x18
    b fail
after_branch:
    adr x18, after_ret
    ret x18
    b fail
after_ret:
    mov x0, #0
    mov x8, #93
    svc #0
.size _start, . - _start

.type target, %function
target:
    adr x1, after_call
    cmp x30, x1
    b.ne fail
    mov x0, #42
    ret
.size target, . - target

.type fail, %function
fail:
    mov x0, #1
    mov x8, #93
    svc #0
.size fail, . - fail
