/* Syscall and TLS sites 144 MiB apart in one executable segment: beyond one
 * pool's +/-128 MiB `b` range, so each 32 MiB slice needs its own pool.
 * The build is ~144 MiB of zeros; tests/elf/run.sh deletes it. */
.text
.global _start
.type _start, %function
_start:
    mov x8, #172
    svc #0
    mov x19, x0
    adrp x9, far
    add x9, x9, :lo12:far
    blr x9
    cmp x0, x19
    b.ne fail
    mov x0, #0
    mov x8, #93
    svc #0
fail:
    mov x0, #1
    mov x8, #93
    svc #0
    .skip 144 << 20
far:
    mov x8, #172
    svc #0
    mrs x1, tpidr_el0
    ret
.size _start, . - _start
