// DLOPEN_CYCLE: the library tests/elf/dlcycle.c loads and unloads thousands
// of times. It gives the runtime what a real library gives it to keep per
// load: a raw syscall and a thread-pointer read (both rewritten into a
// trampoline pool, runtime/rewrite.c), initialised data and bss after the
// code (the RW segment ends partway into a host page), and .eh_frame (the x18
// pass's function tables, runtime/elfsect.c).
#ifdef DLCYCLE_TEXT_PAD
// Moves the RW segment to a 4 KiB page that is not on a 16 KiB one, as in
// the Steam client's libusb (code 0x1c8b4 bytes, RW at 0x2f688): its first
// page then takes the runtime's sub-page path.
#define DLCYCLE_STR(x) #x
#define DLCYCLE_XSTR(x) DLCYCLE_STR(x)
__asm__(".pushsection .text.dlcycle_pad,\"ax\",%progbits\n"
        ".space " DLCYCLE_XSTR(DLCYCLE_TEXT_PAD) "\n"
        ".popsection");
#endif
static __thread long cycle_tls;
long dlcycle_data[700] = { [3] = 1 };       // .data: ends partway into a page
char dlcycle_bss[5000];                     // .bss after it

// getpid by a raw svc: a trampoline only this library's pool serves.
long dlcycle_getpid(void)
{
    register long x8 __asm__("x8") = 172;   // __NR_getpid
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

// The thread pointer (mrs tpidr_el0, rewritten), data and bss, all written.
long dlcycle_touch(long k)
{
    cycle_tls += k;
    dlcycle_bss[k % (long)sizeof dlcycle_bss] = (char)k;
    dlcycle_data[k % 700] += k;
    return cycle_tls + dlcycle_data[k % 700] + dlcycle_bss[k % (long)sizeof dlcycle_bss];
}
