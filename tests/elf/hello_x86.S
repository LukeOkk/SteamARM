// A freestanding static x86-64 Linux ELF. The whole point of Stage 5: this
// cannot run on an ARM CPU without translation, so reaching its output means
// FEX's JIT ran under the runtime.
    .text
    .globl _start
_start:
    mov     $1, %rax            // __NR_write
    mov     $1, %rdi
    lea     msg(%rip), %rsi
    mov     $(msg_end - msg), %rdx
    syscall

    mov     $231, %rax          // __NR_exit_group
    xor     %rdi, %rdi
    syscall
    ud2

    .section .rodata
msg:
    .ascii  "hello from x86-64, translated by FEX, on macOS, with no VM\n"
msg_end:
