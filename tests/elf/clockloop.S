// Calls clock_gettime(CLOCK_MONOTONIC) a fixed number of times, then exits.
// clock_gettime is 70.6% of every syscall the real guest makes
// (benchmarks/stage1-syscall-mix.txt), so this is the hot path, not a
// microbenchmark of something incidental. Wall-clock time over the run divided
// by ITERS is the per-call cost through the rewritten trampoline.

    .text
    .globl _start
_start:
    sub     sp, sp, #32                 // room for a struct timespec
    mov     x19, sp
    mov     x20, #1000
    lsl     x20, x20, #10               // 1024000 iterations

1:  mov     x0, #1                      // CLOCK_MONOTONIC
    mov     x1, x19
    mov     x8, #113                    // __NR_clock_gettime
    svc     #0
    subs    x20, x20, #1
    b.ne    1b

    mov     x0, #0
    mov     x8, #94                     // __NR_exit_group
    svc     #0
    brk     #0
