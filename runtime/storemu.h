// storemu.c -- perform one AArch64 store for a guest thread parked in a
// fault handler. Used by subpage.c for a store executed from the 16 KiB host
// page it writes (a W/X-split page that can never be both at once).
#ifndef LXRT_STOREMU_H
#define LXRT_STOREMU_H

#include <stdbool.h>
#include <stdint.h>

enum lxrt_storemu_kind {
    LXRT_SM_STORE,      // plain store: data[] in memory order
    LXRT_SM_RELEASE,    // STLR / STLLR / STLUR: data[] with release order
    LXRT_SM_ZVA,        // DC ZVA: len zero bytes
    LXRT_SM_ATOMIC,     // LSE LD<op> / SWP: old value to rt
    LXRT_SM_CAS,        // CAS: old value to rs
    LXRT_SM_CASP,       // CASP: old pair to rs, rs+1
    LXRT_SM_EXCL,       // STXR/STLXR/STXP/STLXP: compare-and-swap, status to rs
};

struct lxrt_storemu {
    int kind;
    uint64_t addr;          // first byte written
    uint32_t len;           // bytes written
    uint32_t elem;          // single-copy-atomic unit of a plain store (bytes)
    int op;                 // LXRT_SM_ATOMIC: the operation
    unsigned rs, rt;        // result registers (see kind)
    bool pair32;            // CASP / STXP with 32-bit registers
    uint64_t val[2];        // operand / new value (pair: [0] low half)
    uint64_t cmp[2];        // expected value (pair: [0] low half)
    int wb;                 // base register to write back, -1 none (31 = SP)
    uint64_t wb_val;
    uint8_t data[64];       // LXRT_SM_STORE / LXRT_SM_RELEASE
    // LXRT_SM_EXCL with block: the loop's retry is performed (storemu.c).
    bool block;
    uint64_t ldxr_pc, stxr_pc;
    unsigned lt, lt2;       // the load-exclusive's destinations
    unsigned st_rt, st_rt2; // the store-exclusive's sources
    unsigned rn;            // their base
    int nbetween;
    uint32_t between[16];   // the words between the two
};

// Decode the instruction word at pc against the thread state in uap (a
// ucontext_t *). Returns 0 and fills *m when it is a store this module can
// perform; otherwise -1 with *why saying what it is. Nothing is written.
int lxrt_storemu_decode(uint32_t insn, uint64_t pc, void *uap,
                        struct lxrt_storemu *m, const char **why);

// Perform a decoded store: the memory access (the caller has made the bytes
// writable), the result and writeback registers, and pc += 4.
void lxrt_storemu_perform(const struct lxrt_storemu *m, void *uap);

#endif
