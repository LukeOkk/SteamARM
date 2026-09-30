// A syscall that writes into a 4 KiB guest page the guest made read-only
// fails with EFAULT, also when the 16 KiB host page around it stays writable
// (LXRT_GUEST_PAGE=4096; subpage.c). Chromium's base::ProtectedMemory checks
// its section with getrlimit/prlimit64 into it and expects EFAULT.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    int fails = 0;
    char *p = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    p[0] = 1;
    int mp = mprotect(p, 4096, PROT_READ);
    errno = 0;
    long r1 = syscall(SYS_prlimit64, 0, RLIMIT_NPROC, NULL, p);
    int e1 = errno;
    errno = 0;
    long r2 = syscall(SYS_prlimit64, 0, RLIMIT_NPROC, NULL, p + 8192);
    int good = mp == 0 && r1 == -1 && e1 == EFAULT && r2 == 0;
    printf("  %s  prlimit64 into the read-only 4 KiB page: %ld (%s); into the writable one: %ld\n",
           good ? "OK " : "MAL", r1, e1 == EFAULT ? "EFAULT" : "no EFAULT", r2);
    fails += !good;
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
