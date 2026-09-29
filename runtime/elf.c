// ELF64 aarch64 loader. Maps PT_LOAD segments of a Linux image into this
// Darwin process.
//
// The alignment question this was expected to die on turned out not to be a
// problem: every aarch64 Linux binary surveyed (3548 PT_LOADs across a whole
// Fedora /usr/bin, plus FEX and Proton's ARM64 wine) uses p_align 0x10000,
// which is a clean multiple of Darwin's 16 KiB pages. See
// benchmarks/stage2-elf-alignment.txt.
//
// The wall is somewhere else, and it is hard: Darwin reserves the low 4 GiB of
// every arm64 process as __PAGEZERO, and that reservation cannot be given up.
// Measured (benchmarks/stage2-pagezero.txt):
//   - linking with any -pagezero_size below the 4 GiB default makes the kernel
//     SIGKILL the binary at exec, every size tried, 0x4000 through 0xC0000000;
//   - mmap(MAP_FIXED) and mach_vm_allocate(VM_FLAGS_FIXED) into that range both
//     fail, so it cannot be reclaimed at runtime either.
// Linux EXEC images link at 0x200000 (aarch64) or 0x400000 (x86-64), inside
// that range. They therefore cannot be loaded in-process at their link
// address, and there is no relocation information to move them. Only PIE
// images are loadable. This is stated as a hard limitation, not worked around.

#include "lxrt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define EI_NIDENT 16
#define ET_EXEC 2
#define ET_DYN  3
#define EM_AARCH64 183
#define PT_LOAD    1
#define PT_INTERP  3
#define PT_PHDR    6
#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

static int fail(char **err, const char *fmt, ...)
{
    if (err) {
        char *buf = malloc(512);
        if (buf) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, 512, fmt, ap);
            va_end(ap);
        }
        *err = buf;
    }
    return -1;
}

static int prot_of(uint32_t p_flags)
{
    int prot = 0;
    if (p_flags & PF_R) prot |= PROT_READ;
    if (p_flags & PF_W) prot |= PROT_WRITE;
    if (p_flags & PF_X) prot |= PROT_EXEC;
    return prot;
}

int lxrt_load_elf(const char *path, struct lxrt_image *out, char **err)
{
    memset(out, 0, sizeof(*out));

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fail(err, "open %s: %s", path, strerror(errno));

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return fail(err, "fstat %s: %s", path, strerror(errno));
    }

    uint8_t *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED)
        return fail(err, "mmap %s: %s", path, strerror(errno));

    if ((size_t)st.st_size < sizeof(Elf64_Ehdr))
        return fail(err, "%s: too small to be an ELF", path);

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0)
        return fail(err, "%s: not an ELF", path);
    if (eh->e_ident[4] != 2 /* ELFCLASS64 */ || eh->e_ident[5] != 1 /* LSB */)
        return fail(err, "%s: not a 64-bit little-endian ELF", path);
    if (eh->e_machine != EM_AARCH64)
        return fail(err, "%s: e_machine %u, only aarch64 is implemented "
                         "(x86-64 goes through FEX, Stage 5)", path, eh->e_machine);
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        return fail(err, "%s: e_type %u, expected EXEC or DYN", path, eh->e_type);
    if (eh->e_type == ET_EXEC)
        return fail(err,
                    "%s: non-PIE (ET_EXEC) images cannot be loaded. It links at "
                    "0x%llx, inside Darwin's 4 GiB __PAGEZERO, which cannot be "
                    "shrunk or mapped over on arm64, and an ET_EXEC image has no "
                    "relocations to move it. Rebuild it as PIE, or run it through "
                    "FEX where guest addresses are not host addresses.",
                    path, (unsigned long long)eh->e_entry);

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->e_phoff);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_INTERP)
            continue;
        if (ph[i].p_filesz == 0 || ph[i].p_filesz >= sizeof(out->interp))
            return fail(err, "%s: PT_INTERP is %llu bytes, out of range", path,
                        (unsigned long long)ph[i].p_filesz);
        memcpy(out->interp, file + ph[i].p_offset, (size_t)ph[i].p_filesz);
        out->interp[ph[i].p_filesz] = '\0';
    }

    uint64_t lo = UINT64_MAX, hi = 0;
    int loads = 0;
    bool subpage = false;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        loads++;
        if (ph[i].p_vaddr < lo)
            lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi)
            hi = ph[i].p_vaddr + ph[i].p_memsz;
        // 4 KiB-aligned images (Valve's native arm64 Steam client) put the
        // end of the code and the start of the data in one 16 KiB host page.
        // Their contents are copied, not mapped, so only the protections need
        // care: they are kept per 4 KiB by subpage.c, which flips a page
        // wanted both writable and executable between the two on faults.
        if (ph[i].p_align % LXRT_HOST_PAGE != 0) {
            if (ph[i].p_align % 4096 != 0)
                return fail(err, "%s: PT_LOAD %d has p_align 0x%llx, not a multiple of 4 KiB",
                            path, i, (unsigned long long)ph[i].p_align);
            subpage = true;
        }
    }
    if (!loads)
        return fail(err, "%s: no PT_LOAD segments", path);

    uint64_t lo_page = LXRT_ALIGN_DOWN(lo, LXRT_HOST_PAGE);
    uint64_t hi_page = LXRT_ALIGN_UP(hi, LXRT_HOST_PAGE);
    size_t span = (size_t)(hi_page - lo_page);

    // Reserve the whole span writable in one mapping, then place each segment
    // and tighten permissions afterwards. Mapping segment by segment would
    // leave the gaps between them unmapped, and a p_memsz that runs past
    // p_filesz expects zeroed pages there.
    bool is_pie = eh->e_type == ET_DYN;
    void *base = mmap(is_pie ? NULL : (void *)lo_page, span,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON | (is_pie ? 0 : MAP_FIXED), -1, 0);
    if (base == MAP_FAILED)
        return fail(err, "reserve 0x%zx bytes at 0x%llx: %s%s", span,
                    (unsigned long long)lo_page, strerror(errno),
                    is_pie ? "" :
                    " -- a non-PIE Linux image links below Darwin's __PAGEZERO; "
                    "link the runtime with -Wl,-pagezero_size,0x4000");
    if (!is_pie && (uint64_t)base != lo_page)
        return fail(err, "MAP_FIXED gave 0x%llx, wanted 0x%llx",
                    (unsigned long long)(uint64_t)base, (unsigned long long)lo_page);

    uint64_t bias = (uint64_t)base - lo_page;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        uint8_t *dst = (uint8_t *)(ph[i].p_vaddr + bias);
        if (ph[i].p_filesz)
            memcpy(dst, file + ph[i].p_offset, (size_t)ph[i].p_filesz);
        // The rest of p_memsz is .bss and is already zero from MAP_ANON.
    }

    // Tighten permissions. Two segments can share a host page only if the
    // image's p_align is smaller than the host page, which was rejected above,
    // so a straight per-segment mprotect is safe here.
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        uint64_t start = LXRT_ALIGN_DOWN(ph[i].p_vaddr + bias, LXRT_HOST_PAGE);
        uint64_t end = LXRT_ALIGN_UP(ph[i].p_vaddr + bias + ph[i].p_memsz,
                                     LXRT_HOST_PAGE);
        // Executable pages are mapped WRITABLE BUT NOT EXECUTABLE until the
        // rewriting pass has run; main.c seals them to read-execute afterwards.
        // macOS enforces W^X on arm64, so the intermediate state cannot be RWX
        // -- mprotect returns EACCES for it.
        int prot = prot_of(ph[i].p_flags);
        if (subpage) {
            uint64_t s4 = LXRT_ALIGN_DOWN(ph[i].p_vaddr + bias, 4096);
            uint64_t e4 = LXRT_ALIGN_UP(ph[i].p_vaddr + bias + ph[i].p_memsz, 4096);
            if (prot & PROT_EXEC) {
                if (out->nexec < (int)(sizeof(out->exec) / sizeof(out->exec[0]))) {
                    out->exec[out->nexec].start = ph[i].p_vaddr + bias;
                    out->exec[out->nexec].end = ph[i].p_vaddr + bias + ph[i].p_filesz;
                    out->nexec++;
                }
                prot = (prot & ~PROT_EXEC) | PROT_WRITE;   // sealed by main.c
            }
            long rc = lxrt_subpage_mprotect(s4, e4 - s4, prot);
            if (rc != 0)
                return fail(err, "subpage mprotect 0x%llx+0x%llx: %ld",
                            (unsigned long long)s4, (unsigned long long)(e4 - s4), rc);
            continue;
        }
        if (prot & PROT_EXEC) {
            prot = (prot & ~PROT_EXEC) | PROT_WRITE;
            if (out->nexec < (int)(sizeof(out->exec) / sizeof(out->exec[0]))) {
                out->exec[out->nexec].start = ph[i].p_vaddr + bias;
                out->exec[out->nexec].end = ph[i].p_vaddr + bias + ph[i].p_filesz;
                out->nexec++;
            }
        }
        if (mprotect((void *)start, (size_t)(end - start), prot) != 0)
            return fail(err, "mprotect 0x%llx+0x%llx to %d: %s",
                        (unsigned long long)start, (unsigned long long)(end - start),
                        prot, strerror(errno));
    }

    out->base = (uint8_t *)base;
    out->load_bias = bias;
    out->entry = eh->e_entry + bias;
    out->phentsize = eh->e_phentsize;
    out->phnum = eh->e_phnum;
    out->span = span;
    out->brk = hi_page + bias;
    out->is_pie = is_pie;
    out->subpage = subpage;

    // AT_PHDR must point at the program headers *as mapped*, which is only
    // true if some PT_LOAD covers e_phoff. It normally does.
    out->phdr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (eh->e_phoff >= ph[i].p_offset &&
            eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize
                <= ph[i].p_offset + ph[i].p_filesz) {
            out->phdr = ph[i].p_vaddr + bias + (eh->e_phoff - ph[i].p_offset);
            break;
        }
    }

    munmap(file, (size_t)st.st_size);
    // Where the instructions are, for the x18 pass (runtime/elfsect.c).
    out->ncode = lxrt_elf_exec_sections_path(path, bias, out->code,
                                             (int)(sizeof(out->code) / sizeof(out->code[0])));
    return 0;
}
