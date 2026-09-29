// NEAR_CODE_TRAMPOLINE_ALLOCATION without Steam: a 144 MiB executable
// PT_LOAD mapped the way ld.so maps libcef.so -- the whole span reserved
// PROT_NONE first, then each LOAD MAP_FIXED from the file at a 4 KiB (not
// 16 KiB) aligned address, so it takes the sub-page path. 160 MiB of
// reservation below leaves no free page within branch range of the first
// 32 MiB slice except the hole between LOAD0 and LOAD1, which the runtime
// may borrow (elfsect.c lxrt_elf_gap_before_exec, rewrite.c alloc_pool_near).
// Without it the svc at the start of the segment is poisoned (MEASURED on
// libcef before the gap island: "13 svc sites, 0 rewritten, 13 poisoned").
// The file has no section headers, so only svc/TLS sites are rewritten.
#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MIB (1024ul * 1024)
#define TEXT (144 * MIB)            // LOAD1 file size: beyond one pool's reach
#define EXEC_VA 0x41000ul           // LOAD0 ends at 0x1000: a 0x40000 hole
#define SPAN (EXEC_VA + TEXT + 0x1000)
#define SLACK (160 * MIB)

static int failures;
static void check(const char *name, int yes)
{
    printf("  %s  %s\n", yes ? "ok  " : "FAIL", name);
    failures += !yes;
}

static const uint32_t getpid_stub[] = { 0xd2801588, 0xd4000001, 0xd65f03c0 }; // mov x8, #172; svc #0; ret
static const uint32_t tp_stub[] = { 0xd53bd040, 0xd65f03c0 };                // mrs x0, tpidr_el0; ret

static int write_elf(int fd)
{
    unsigned char head[0x1000] = {0};
    Elf64_Ehdr *eh = (Elf64_Ehdr *)head;
    memcpy(eh->e_ident, ELFMAG, SELFMAG);
    eh->e_ident[EI_CLASS] = ELFCLASS64;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type = ET_DYN;
    eh->e_machine = EM_AARCH64;
    eh->e_version = EV_CURRENT;
    eh->e_phoff = sizeof *eh;
    eh->e_ehsize = sizeof *eh;
    eh->e_phentsize = sizeof(Elf64_Phdr);
    eh->e_phnum = 3;
    Elf64_Phdr *ph = (Elf64_Phdr *)(head + sizeof *eh);
    ph[0] = (Elf64_Phdr){ PT_LOAD, PF_R, 0, 0, 0, 0x1000, 0x1000, 0x1000 };
    ph[1] = (Elf64_Phdr){ PT_LOAD, PF_R | PF_X, 0x1000, EXEC_VA, EXEC_VA, TEXT, TEXT, 0x1000 };
    ph[2] = (Elf64_Phdr){ PT_LOAD, PF_R | PF_W, 0x1000 + TEXT, EXEC_VA + TEXT, EXEC_VA + TEXT,
                          0x1000, 0x1000, 0x1000 };
    if (ftruncate(fd, (off_t)(0x2000 + TEXT)) != 0 ||
        pwrite(fd, head, sizeof head, 0) != (ssize_t)sizeof head ||
        pwrite(fd, getpid_stub, sizeof getpid_stub, 0x1000) != (ssize_t)sizeof getpid_stub ||
        pwrite(fd, getpid_stub, sizeof getpid_stub, (off_t)(0x1000 + TEXT - 32)) != (ssize_t)sizeof getpid_stub ||
        pwrite(fd, tp_stub, sizeof tp_stub, (off_t)(0x1000 + TEXT - 16)) != (ssize_t)sizeof tp_stub)
        return -1;
    return 0;
}

int main(void)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/lxrt-pool-near-%ld.elf", (long)getpid());
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    check("sparse 144 MiB ELF written", fd >= 0 && write_elf(fd) == 0);
    if (fd < 0) goto verdict;
    unlink(path);

    // ld.so's order: reserve, then map each LOAD over the reservation. Only
    // the space above the image is given back.
    uint8_t *res = mmap(NULL, SLACK + SPAN + SLACK, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("reserve 160 MiB + span + 160 MiB", res != MAP_FAILED);
    if (res == MAP_FAILED) goto verdict;
    uintptr_t bias = ((uintptr_t)res + SLACK + 0xffff) & ~(uintptr_t)0xffff;
    uintptr_t top = (bias + SPAN + 0x3fff) & ~(uintptr_t)0x3fff;
    munmap((void *)top, (uintptr_t)res + SLACK + SPAN + SLACK - top);
    void *l0 = mmap((void *)bias, 0x1000, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    void *l1 = mmap((void *)(bias + EXEC_VA), TEXT, PROT_READ | PROT_EXEC,
                    MAP_PRIVATE | MAP_FIXED, fd, 0x1000);
    void *l2 = mmap((void *)(bias + EXEC_VA + TEXT), 0x1000, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_FIXED, fd, (off_t)(0x1000 + TEXT));
    check("LOAD0/1/2 mapped MAP_FIXED at bias + vaddr",
          l0 == (void *)bias && l1 == (void *)(bias + EXEC_VA) && l2 == (void *)(bias + EXEC_VA + TEXT));
    if (l1 != (void *)(bias + EXEC_VA)) goto verdict;
    printf("  bias %#lx, code %#lx+%#lx\n", (unsigned long)bias,
           (unsigned long)(bias + EXEC_VA), (unsigned long)TEXT);

    long (*near)(void) = (long (*)(void))(bias + EXEC_VA);
    long (*far)(void) = (long (*)(void))(bias + EXEC_VA + TEXT - 32);
    void *(*far_tp)(void) = (void *(*)(void))(bias + EXEC_VA + TEXT - 16);
    long pid = getpid();
    check("svc at the start of the segment (pool in the ELF gap)", near() == pid);
    check("svc 144 MiB further (its own slice's pool)", far() == pid);
    check("TLS read at the end of the segment", far_tp() == __builtin_thread_pointer());
    // No munmap: the runtime would rightly warn that it destroys live pools.
    close(fd);
verdict:
    puts(failures ? "== pool near: FAIL" : "== pool near: ok");
    return failures != 0;
}
