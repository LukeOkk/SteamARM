/* Store forms executed from the host page they write (runtime/storemu.c).
 *
 * A 4 KiB-aligned image: the end of the code and the start of the data share
 * one 16 KiB host page, which is read-execute or read-write, never both.
 * `kit` sits in that shared page and runs a list of store forms against a
 * 1.5 KiB buffer at x0, recording every register result at x1. It runs twice:
 * first against a buffer on the stack (plain memory, the hardware executes
 * every store), then against `target`, in the shared page (every store to it
 * faults and is performed by the runtime). The two buffers and the two result
 * areas must come out identical: exit 0. Exit 3: the buffers differ, 4: the
 * results differ. With an argument, the four areas are also written to stdout
 * (1.5 KiB, 1.5 KiB, 1 KiB, 1 KiB) for a byte-level diff. */
.arch armv8.4-a+lse+rcpc
.text
.global _start
.type _start, %function
_start:
    ldr x23, [sp]                       // argc
    sub sp, sp, #4096
    add x19, sp, #63
    and x19, x19, #~63                  // native target (64-byte aligned)
    add x20, x19, #1536                 // native results
    add x21, x20, #1024                 // emulated results
    adrp x22, target
    add x22, x22, :lo12:target

    mov x0, x19
    bl fill
    mov x0, x22
    bl fill
    mov x0, x20
    bl zero1k
    mov x0, x21
    bl zero1k

    mov x0, x19
    mov x1, x20
    bl kit
    mov x0, x22
    mov x1, x21
    bl kit

    cmp x23, #2
    b.lt 1f
    mov x0, #1
    mov x1, x19
    mov x2, #1536
    mov x8, #64                         // write
    svc #0
    mov x0, #1
    mov x1, x22
    mov x2, #1536
    mov x8, #64
    svc #0
    mov x0, #1
    mov x1, x20
    mov x2, #2048                       // both result areas
    mov x8, #64
    svc #0
1:
    mov x0, x19
    mov x1, x22
    mov x2, #1536
    bl same
    mov x9, #3
    cbz x0, exit_x9
    mov x0, x20
    mov x1, x21
    mov x2, #1024
    bl same
    mov x9, #4
    cbz x0, exit_x9
    mov x9, #0
exit_x9:
    mov x0, x9
    mov x8, #93
    svc #0

// fill(x0): byte i = i * 7 + 3
fill:
    mov x1, #0
2:  add x2, x1, x1, lsl #3
    sub x2, x2, x1, lsl #1              // i * 7
    add x2, x2, #3
    strb w2, [x0, x1]
    add x1, x1, #1
    cmp x1, #1536
    b.ne 2b
    ret
zero1k:
    mov x1, #0
3:  str xzr, [x0, x1]
    add x1, x1, #8
    cmp x1, #1024
    b.ne 3b
    ret
// same(x0, x1, x2): 1 if the x2 bytes are equal
same:
    mov x3, #0
4:  ldrb w4, [x0, x3]
    ldrb w5, [x1, x3]
    cmp w4, w5
    b.ne 5f
    add x3, x3, #1
    cmp x3, x2
    b.ne 4b
    mov x0, #1
    ret
5:  mov x0, #0
    ret
    .skip 20480

// ------------------------------------------------------------------ kit
.global kit
.type kit, %function
kit:
    movz x2, #0x7788
    movk x2, #0x5566, lsl #16
    movk x2, #0x3344, lsl #32
    movk x2, #0x1122, lsl #48
    movz x3, #0xff00
    movk x3, #0xddee, lsl #16
    movk x3, #0xbbcc, lsl #32
    movk x3, #0x99aa, lsl #48
    movz x4, #0xcdef
    movk x4, #0x89ab, lsl #16
    movk x4, #0x4567, lsl #32
    movk x4, #0x0123, lsl #48
    movz x5, #0x3210
    movk x5, #0x7654, lsl #16
    movk x5, #0xba98, lsl #32
    movk x5, #0xfedc, lsl #48
    movz x6, #0x6978
    movk x6, #0x4b5a, lsl #16
    movk x6, #0x2d3c, lsl #32
    movk x6, #0x0f1e, lsl #48
    movz x7, #0xe1f0
    movk x7, #0xc3d2, lsl #16
    movk x7, #0xa5b4, lsl #32
    movk x7, #0x8796, lsl #48
    adrp x9, vectors
    add x9, x9, :lo12:vectors
    ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x9]

    // Base registers: unsigned offset, unscaled, unaligned.
    str x2, [x0]
    str w3, [x0, #8]
    strh w4, [x0, #12]
    strb w5, [x0, #14]
    stur x6, [x0, #15]
    sturh w7, [x0, #23]
    // Pre- and post-index writeback.
    mov x9, x0
    str x2, [x9, #24]!
    sub x10, x9, x0
    str x10, [x1, #0]
    str x3, [x9], #8
    sub x10, x9, x0
    str x10, [x1, #8]
    // Pairs.
    stp x4, x5, [x0, #32]
    stp w6, w7, [x0, #48]
    add x9, x0, #56
    stp x2, x3, [x9, #8]!
    sub x10, x9, x0
    str x10, [x1, #16]
    stp w4, w5, [x9], #-16
    sub x10, x9, x0
    str x10, [x1, #24]
    stnp x6, x7, [x0, #80]
    // Register offset: LSL, UXTW, SXTW (negative), scaled halfword.
    mov x10, #96
    str x2, [x0, x10]
    mov w11, #26
    str w3, [x0, w11, uxtw #2]
    mov w12, #-8
    add x13, x0, #116
    strb w4, [x13, w12, sxtw]
    mov x14, #55
    strh w5, [x0, x14, lsl #1]
    sttr x6, [x0, #112]
    // SIMD&FP registers.
    str q0, [x0, #128]
    str d1, [x0, #144]
    str s2, [x0, #152]
    str h3, [x0, #156]
    str b0, [x0, #158]
    stur q1, [x0, #159]
    add x9, x0, #176
    str q2, [x9], #16
    sub x10, x9, x0
    str x10, [x1, #32]
    str q3, [x9, #16]!
    sub x10, x9, x0
    str x10, [x1, #40]
    mov x10, #14
    str q0, [x0, x10, lsl #4]
    stp q0, q1, [x0, #240]
    stp d2, d3, [x0, #272]
    add x9, x0, #288
    stp s0, s1, [x9]
    stnp q2, q3, [x0, #304]
    // ST1-ST4, multiple structures.
    add x9, x0, #336
    st1 {v0.16b}, [x9]
    add x9, x0, #352
    st2 {v0.8h, v1.8h}, [x9], #32
    sub x10, x9, x0
    str x10, [x1, #48]
    st4 {v0.4s, v1.4s, v2.4s, v3.4s}, [x9]
    add x9, x0, #448
    mov x10, #24
    st3 {v0.8b, v1.8b, v2.8b}, [x9], x10
    sub x10, x9, x0
    str x10, [x1, #56]
    st1 {v0.2d, v1.2d}, [x9]
    add x9, x0, #520
    st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x9]
    // ST1-ST4, single structure.
    add x9, x0, #584
    st1 {v0.s}[1], [x9]
    add x9, x0, #588
    st2 {v1.h, v2.h}[5], [x9], #4
    sub x10, x9, x0
    str x10, [x1, #64]
    mov x10, #3
    st3 {v0.b, v1.b, v2.b}[15], [x9], x10
    sub x10, x9, x0
    str x10, [x1, #72]
    add x9, x0, #600
    st4 {v0.d, v1.d, v2.d, v3.d}[1], [x9]
    // Ordered stores.
    add x9, x0, #632
    stlr x2, [x9]
    add x9, x0, #640
    stlr w3, [x9]
    add x9, x0, #644
    stlrh w4, [x9]
    add x9, x0, #646
    stlrb w5, [x9]
    add x9, x0, #600
    stlur x7, [x9, #48]
    stlurh w2, [x9, #56]
    // LSE atomics (old values recorded).
    add x9, x0, #664
    ldadd x3, x12, [x9]
    str x12, [x1, #80]
    add x9, x0, #672
    ldaddal w4, w13, [x9]
    str x13, [x1, #88]
    add x9, x0, #680
    ldclr x5, x14, [x9]
    str x14, [x1, #96]
    add x9, x0, #688
    ldeora w6, w15, [x9]
    str x15, [x1, #104]
    add x9, x0, #696
    ldsetl x7, x16, [x9]
    str x16, [x1, #112]
    add x9, x0, #704
    ldsmax x2, x17, [x9]
    str x17, [x1, #120]
    add x9, x0, #712
    ldsmin w3, w12, [x9]
    str x12, [x1, #128]
    add x9, x0, #720
    ldumax x4, x13, [x9]
    str x13, [x1, #136]
    add x9, x0, #728
    ldumin w5, w14, [x9]
    str x14, [x1, #144]
    add x9, x0, #736
    ldaddb w6, w15, [x9]
    str x15, [x1, #152]
    add x9, x0, #738
    ldsmaxh w7, w16, [x9]
    str x16, [x1, #160]
    add x9, x0, #741
    ldsminb w2, w17, [x9]
    str x17, [x1, #168]
    add x9, x0, #744
    swp x2, x12, [x9]
    str x12, [x1, #176]
    add x9, x0, #752
    swpal w3, w13, [x9]
    str x13, [x1, #184]
    add x9, x0, #756
    stadd w4, [x9]
    // CAS: one that succeeds, one that fails, sized ones.
    add x9, x0, #760
    ldr x12, [x9]
    cas x12, x2, [x9]
    str x12, [x1, #192]
    add x9, x0, #768
    mov x13, #0x5555
    casal x13, x3, [x9]
    str x13, [x1, #200]
    add x9, x0, #776
    ldrb w14, [x9]
    casb w14, w4, [x9]
    str x14, [x1, #208]
    add x9, x0, #778
    ldrh w15, [x9]
    casah w15, w5, [x9]
    str x15, [x1, #216]
    add x9, x0, #780
    mov w16, #7
    casl w16, w6, [x9]
    str x16, [x1, #224]
    // CASP: 64-bit pair succeeds, 32-bit pair fails.
    add x9, x0, #784
    ldp x12, x13, [x9]
    caspal x12, x13, x2, x3, [x9]
    stp x12, x13, [x1, #232]
    add x9, x0, #800
    mov x14, #1
    mov x15, #2
    casp w14, w15, w4, w5, [x9]
    stp x14, x15, [x1, #248]
    // Exclusive pairs of loads and stores, as compilers emit them.
    add x9, x0, #808
6:  ldaxr x12, [x9]
    add x13, x12, #5
    stlxr w14, x13, [x9]
    cbnz w14, 6b
    stp x12, x14, [x1, #264]
    add x9, x0, #816
7:  ldxr w12, [x9]
    eor w13, w12, #0xff
    stxr w14, w13, [x9]
    cbnz w14, 7b
    stp x12, x14, [x1, #280]
    add x9, x0, #821
8:  ldaxrb w12, [x9]
    add w13, w12, #1
    stlxrb w14, w13, [x9]
    cbnz w14, 8b
    stp x12, x14, [x1, #296]
    add x9, x0, #824
9:  ldaxr x12, [x9]
    cmp x12, x2
    b.eq 10f
    stlxr w14, x3, [x9]
    cbnz w14, 9b
10: stp x12, x14, [x1, #312]
    add x9, x0, #832
11: ldxp x12, x13, [x9]
    adds x15, x12, #1
    adc x16, x13, xzr
    stxp w14, x15, x16, [x9]
    cbnz w14, 11b
    stp x12, x13, [x1, #328]
    str x14, [x1, #344]
    add x9, x0, #848
12: ldaxp w12, w13, [x9]
    add w15, w13, #1
    add w16, w12, #2
    stlxp w14, w15, w16, [x9]
    cbnz w14, 12b
    stp x12, x13, [x1, #352]
    str x14, [x1, #368]
    // Loops that overwrite the loaded register before the store-exclusive,
    // as compilers emit them: the runtime performs the loop's retry with the
    // body interpreted. Each body uses different instruction classes.
    // Results from x1 + 512 on (x11).
    add x11, x1, #512
    add x9, x0, #1024
13: ldaxr x12, [x9]
    add x12, x12, x3                    // the loaded register itself
    stlxr w14, x12, [x9]
    cbnz w14, 13b
    stp x12, x14, [x11, #0]
    add x9, x0, #1032
14: ldxr w12, [x9]
    and w12, w12, w4
    mvn w12, w12                        // nand
    stxr w14, w12, [x9]
    cbnz w14, 14b
    stp x12, x14, [x11, #16]
    add x9, x0, #1040
15: ldaxr x12, [x9]
    cmp x12, x5
    csel x12, x12, x5, gt               // signed max
    stlxr w14, x12, [x9]
    cbnz w14, 15b
    stp x12, x14, [x11, #32]
    add x9, x0, #1048
    ldr x15, [x9]
16: ldaxr x12, [x9]
    cmp x12, x15
    b.ne 17f                            // compare-exchange, taken path not reached
    mov x12, x2
    stlxr w14, x12, [x9]
    cbnz w14, 16b
17: stp x12, x14, [x11, #48]
    add x9, x0, #1056
18: ldaxrb w12, [x9]
    sxtb w13, w12
    cmp w13, w6, sxtb
    csel w12, w12, w6, lt               // signed byte min
    stlxrb w14, w12, [x9]
    cbnz w14, 18b
    stp x12, x13, [x11, #64]
    str x14, [x11, #80]
    add x9, x0, #1064
    mov x10, #5
19: ldaxr x12, [x9]
    eor x12, x12, #0xff00ff00ff00ff00
    movk x12, #0xbeef, lsl #16
    bfi x12, x7, #40, #8
    lsl x13, x12, #3
    lsrv x16, x12, x10
    madd x12, x13, x16, x12
    eor x12, x12, x12, ror #13
    asr x17, x12, #7
    neg x17, x17
    ccmp x12, x2, #4, ne
    cinc x12, x12, eq
    csinv x12, x12, x17, mi
    tbz x12, #0, 20f
    add x12, x12, #1
20: stlxr w14, x12, [x9]
    cbnz w14, 19b
    stp x12, x13, [x11, #88]
    stp x16, x17, [x11, #104]
    str x14, [x11, #120]
    add x9, x0, #1072
21: ldaxp x12, x13, [x9]
    adds x12, x12, x5                   // carries out of the low half
    adc x13, x13, x4                    // 128-bit add
    stlxp w14, x12, x13, [x9]
    cbnz w14, 21b
    mrs x15, nzcv
    stp x12, x13, [x11, #128]
    stp x14, x15, [x11, #184]
    add x9, x0, #1104
    ldr x15, [x9]
23: ldaxr x12, [x9]
    cmp x12, x15                        // equal: GT and GE differ
    csel x12, x3, x4, gt
    csinc x12, x12, x2, ge
    stlxr w14, x12, [x9]
    cbnz w14, 23b
    stp x12, x14, [x11, #200]
    add x9, x0, #1088
22: ldxp w12, w13, [x9]
    subs w12, w6, w12                   // borrows: C clear for the sbc
    sbc w13, w13, w7
    stxp w14, w12, w13, [x9]
    cbnz w14, 22b
    mrs x15, nzcv                       // flags the interpreted body left
    stp x12, x13, [x11, #152]
    stp x14, x15, [x11, #168]
    // DC ZVA: an aligned address and an interior one.
    add x9, x0, #896
    dc zva, x9
    add x9, x0, #980
    dc zva, x9
    // A base of SP, written back.
    mov x15, sp
    add x9, x0, #880
    mov sp, x9
    str x2, [sp, #-16]!
    mov x10, sp
    mov sp, x15
    sub x10, x10, x0
    str x10, [x1, #376]
    ret
.global kit_end
kit_end:
.size kit, kit_end - kit

.section .rodata
.p2align 4
vectors:
    .byte 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    .byte 0x0f,0x1e,0x2d,0x3c,0x4b,0x5a,0x69,0x78,0x87,0x96,0xa5,0xb4,0xc3,0xd2,0xe1,0xf0
    .byte 0x13,0x57,0x9b,0xdf,0x24,0x68,0xac,0xe0,0x31,0x75,0xb9,0xfd,0x42,0x86,0xca,0x0e
    .byte 0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10,0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01
.data
.p2align 6
.global target
target: .skip 1536
.global target_end
target_end:
