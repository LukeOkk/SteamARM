// Three x64 threads whose ud2 traps a vectored handler emulates (count in
// rax, push/pop through rsp) for 40 s: the exception path of Minecraft
// Dungeons II's invalid-opcode VM in miniature. Prints how many traps were
// emulated (the trap rate) and how many rounds came out wrong. The emulated
// pushes and pops are an unbalanced call/ret stream for FEX's call-return
// stack, which runs into its top guard page (runtime/subpage.c).
//   veh_traps.exe [result file] [threads to suspend/read/resume each 100 ms, 0-3]
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
static volatile LONG stop, rounds, bad, faults;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    const unsigned char *p = (const unsigned char *)c->Rip;
    if (p[0] != 0x0f || p[1] != 0x0b) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&faults);
    switch (p[2]) {                   // a marker byte after ud2 says what to emulate
    case 0x90: c->Rax += 1; c->Rip += 3; break;                                      // inc rax
    case 0x91: c->Rsp -= 8; *(uint64_t *)c->Rsp = c->Rbx; c->Rip += 3; break;      // push rbx
    case 0x92: c->Rcx = *(uint64_t *)c->Rsp; c->Rsp += 8; c->Rip += 3; break;      // pop rcx
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
static uint64_t once(uint64_t v)
{
    uint64_t rax, rcx, rsp0, rsp1;
    __asm__ volatile(
        "mov %%rsp, %[s0]\n\t"
        "xor %%eax, %%eax\n\t"
        "mov %[v], %%rbx\n\t"
        ".byte 0x0f,0x0b,0x90\n\t"
        ".byte 0x0f,0x0b,0x90\n\t"
        ".byte 0x0f,0x0b,0x91\n\t"
        ".byte 0x0f,0x0b,0x90\n\t"
        ".byte 0x0f,0x0b,0x92\n\t"
        "mov %%rsp, %[s1]\n\t"
        : "=a"(rax), "=c"(rcx), [s0] "=&r"(rsp0), [s1] "=&r"(rsp1)
        : [v] "r"(v)
        : "rbx", "memory");
    if (rax != 3 || rcx != v || rsp0 != rsp1) return 0;
    return 1;
}
static DWORD WINAPI worker(void *arg)
{
    uint64_t v = (uint64_t)(uintptr_t)arg * 0x10001;
    while (!stop) {
        if (!once(v++)) InterlockedIncrement(&bad);
        InterlockedIncrement(&rounds);
    }
    return 0;
}
int main(int argc, char **argv)
{
    FILE *out = fopen(argc > 1 ? argv[1] : "veh_traps.txt", "a");
    int nsusp = argc > 2 ? atoi(argv[2]) : 0;
    if (nsusp < 0 || nsusp > 3) nsusp = 0;
    AddVectoredExceptionHandler(1, veh);
    HANDLE t[3];
    for (int i = 0; i < 3; i++) t[i] = CreateThread(NULL, 0, worker, (void *)(uintptr_t)(i + 1), 0, NULL);
    DWORD start = GetTickCount(); long susp = 0;
    while (GetTickCount() - start < 40000) {
        Sleep(100);
        for (int i = 0; i < nsusp; i++) {
            if (SuspendThread(t[i]) == (DWORD)-1) continue;
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
            GetThreadContext(t[i], &c);
            ResumeThread(t[i]);
            susp++;
        }
    }
    stop = 1;
    WaitForMultipleObjects(3, t, TRUE, 10000);
    fprintf(out ? out : stdout, "veh_traps: %ld suspensions, %ld rounds, %ld faults emulated, %ld bad\n", susp, (long)rounds, (long)faults, (long)bad);
    return 0;
}
