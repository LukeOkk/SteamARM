/* A site the planner refuses (ldaxr: exclusive) is poisoned with brk #1 and
 * must trap. x16 holds Darwin's getpid number: the old poison, svc #1, ran it
 * and returned, so this program exited 0 instead of dying with SIGTRAP. */
.text
.global _start
.type _start, %function
_start:
    mov x16, #20
    adr x1, slot
    ldaxr x18, [x1]
    mov x0, #0
    mov x8, #93
    svc #0
.size _start, . - _start
.data
.p2align 3
slot: .quad 1
