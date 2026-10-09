// Do EFlags PF/AF survive an exception a vectored handler continues?
// Real x64 Windows: the handler sees the flags the faulting code had, and what
// it writes to ContextRecord->EFlags is what the code resumes with.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
static volatile DWORD seen;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    const unsigned char *p = (const unsigned char *)c->Rip;
    if (p[0] != 0x0f || p[1] != 0x0b) return EXCEPTION_CONTINUE_SEARCH;
    seen = c->EFlags;
    if (p[2] == 0x94) c->EFlags ^= 0x14;       // flip PF and AF
    c->Rip += 3;
    return EXCEPTION_CONTINUE_EXECUTION;
}
// flags after "mov al,0x0f; add al,1" (AF=1, result 0x10: one bit set, PF=0)
// or "xor eax,eax" (PF=1, AF=0), then a trap (0x93 keep / 0x94 flip); returns
// the flags the code sees after the trap via pushfq.
static uint64_t run(int af_case, int flip)
{
    uint64_t f;
    if (af_case && !flip)
        __asm__ volatile("mov $0x0f, %%al\n\tadd $1, %%al\n\t.byte 0x0f,0x0b,0x93\n\tpushfq\n\tpop %0" : "=r"(f) :: "rax", "cc");
    else if (af_case)
        __asm__ volatile("mov $0x0f, %%al\n\tadd $1, %%al\n\t.byte 0x0f,0x0b,0x94\n\tpushfq\n\tpop %0" : "=r"(f) :: "rax", "cc");
    else if (!flip)
        __asm__ volatile("xor %%eax, %%eax\n\t.byte 0x0f,0x0b,0x93\n\tpushfq\n\tpop %0" : "=r"(f) :: "rax", "cc");
    else
        __asm__ volatile("xor %%eax, %%eax\n\t.byte 0x0f,0x0b,0x94\n\tpushfq\n\tpop %0" : "=r"(f) :: "rax", "cc");
    return f;
}
int main(int argc, char **argv)
{
    FILE *out = fopen(argc > 1 ? argv[1] : "veh_flags.txt", "a");
    if (!out) out = stdout;
    AddVectoredExceptionHandler(1, veh);
    int fails = 0;
    struct { int af, flip; DWORD want_seen, want_after; const char *what; } t[] = {
        {0, 0, 0x04, 0x04, "PF=1 kept"},
        {0, 1, 0x04, 0x10, "PF=1 flipped (PF->0, AF->1)"},
        {1, 0, 0x10, 0x10, "AF=1 kept"},
        {1, 1, 0x10, 0x04, "AF=1 flipped (AF->0, PF->1)"},
    };
    for (int i = 0; i < 4; i++) {
        uint64_t after = run(t[i].af, t[i].flip);
        DWORD s = seen & 0x14, a = (DWORD)after & 0x14;
        int ok = s == t[i].want_seen && a == t[i].want_after;
        fails += !ok;
        fprintf(out, "pfaf: %-30s handler saw PF/AF %02lx (want %02lx), code resumed with %02lx (want %02lx): %s\n",
                t[i].what, (unsigned long)s, (unsigned long)t[i].want_seen, (unsigned long)a, (unsigned long)t[i].want_after, ok ? "ok" : "WRONG");
    }
    fprintf(out, "pfaf: %d of 4 wrong\n", fails);
    fclose(out);
    return fails;
}
