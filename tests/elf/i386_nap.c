// i386 helper (no verdict): sleeps 40 s so a debugger can inspect FEX's
// dispatcher and JIT state in a live 32-bit guest. Links against libc.
extern int usleep(unsigned);
extern void exit(int) __attribute__((noreturn));
void _start(void) { for (int i = 0; i < 40; i++) usleep(1000000); exit(0); }
