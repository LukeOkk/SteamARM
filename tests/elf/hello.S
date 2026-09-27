// A freestanding static Linux aarch64 ELF: write(1, msg, len) then exit(0).
// No libc, no dynamic loader, no relocations. The smallest thing that proves
// the runtime can map a Linux image, rewrite its syscall sites and run it.

    .text
    .globl _start
_start:
    // write(1, msg, len)
    mov     x0, #1
    adrp    x1, msg
    add     x1, x1, :lo12:msg
    mov     x2, #(msg_end - msg)
    mov     x8, #64                 // __NR_write
    svc     #0

    // exit_group(0)
    mov     x0, #0
    mov     x8, #94                 // __NR_exit_group
    svc     #0

    // Unreachable. If the runtime ever returns here, fault loudly rather than
    // running into whatever follows.
    brk     #0

    .section .rodata
msg:
    .ascii  "hello from a Linux ELF, no VM\n"
msg_end:
