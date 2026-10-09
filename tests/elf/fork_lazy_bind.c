// A fork child's first lazy (PLT) binding, under LXRT_NO_X18=1 with a
// preloaded library ahead of libc in the lookup scope: wineserver's daemon
// child exactly. glibc's ld.so keeps the scope length in x18 (do_lookup_x);
// a fork child loses the kernel's x18 preservation and its first page faults
// zero the register, so without the runtime's x18 virtualisation the lookup
// stopped at the preloaded library and the child died with 127 ("undefined
// symbol: setsid, version GLIBC_2.17"). LXRT_NO_X18 now applies to Wine's
// loaders only (runtime/wxsplit.c lxrt_no_x18), so this program keeps the
// virtualisation. Run: LXRT_NO_X18=1 LD_PRELOAD=<any loadable .so> lxrun
// fork_lazy_bind [n]. Prints PASS when no child died.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void (*volatile exitp)(int) = _exit;

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 50, died = 0;
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0) {
            setsid();                  // the child's first call: lazily bound here
            exitp(0);
        }
        int st;
        waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            died++;
    }
    printf("  %s %d of %d fork children bound setsid lazily\n", died ? "MAL" : "OK ", n - died, n);
    printf("%s\n", died ? "FAIL" : "PASS");
    return died != 0;
}
