// Which bytes of a mapped ELF file are instructions.
//
// The svc/TPIDR_EL0 rewriting pass scans whole executable segments and gets
// away with it because its patterns are too specific to occur in data
// (validated against full disassemblies, benchmarks/stage2-rewrite-
// validation.txt). The x18 pass cannot: a load/store or add that happens to
// name register 18 is an ordinary 32-bit value, and the R+X PT_LOAD of every
// guest image also carries .rodata, .eh_frame, .dynsym, .dynstr, .hash --
// measured on libc.so.6, libstdc++.so.6, ld-linux-aarch64.so.1 and FEX with
// llvm-readelf: one R E segment each, .text and .rodata inside it. So the x18
// pass is confined to sections flagged SHF_EXECINSTR (.init, .plt, .text,
// .fini, FEX's HostToGuestTrampolineTemplate), read from the file's section
// headers. A file without section headers (stripped that far) or of the wrong
// machine contributes no windows, and the pass simply does not run on it.

#include "lxrt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EM_AARCH64_    183
#define SHF_EXECINSTR_ 0x4u
#define SHT_PROGBITS_  1u

struct elf64_ehdr_ {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf64_shdr_ {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
};

static bool read_all(int fd, uint64_t off, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t r = pread(fd, (char *)buf + done, len - done, (off_t)(off + done));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        done += (size_t)r;
    }
    return true;
}

// Executable-section windows of the aarch64 ELF open on `fd`, restricted to
// the file range [file_off, file_off+len) that was mapped at `map_base`.
// Returns the number of windows written (0 for a non-ELF, a non-aarch64 ELF,
// or a file without section headers), never more than `max`.
int lxrt_elf_exec_sections(int fd, uint64_t file_off, uint64_t len,
                           uint64_t map_base, struct lxrt_range *out, int max)
{
    struct elf64_ehdr_ eh;
    if (fd < 0 || max <= 0 || !read_all(fd, 0, &eh, sizeof eh))
        return 0;
    if (memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0 || eh.e_ident[4] != 2 /* ELFCLASS64 */ ||
        eh.e_ident[5] != 1 /* little-endian */ || eh.e_machine != EM_AARCH64_)
        return 0;
    if (eh.e_shoff == 0 || eh.e_shnum == 0 || eh.e_shentsize != sizeof(struct elf64_shdr_))
        return 0;
    if (eh.e_shnum > 4096)
        return 0;                   // not a real file

    struct elf64_shdr_ *sh = calloc(eh.e_shnum, sizeof *sh);
    if (!sh)
        return 0;
    if (!read_all(fd, eh.e_shoff, sh, (size_t)eh.e_shnum * sizeof *sh)) {
        free(sh);
        return 0;
    }
    int n = 0;
    uint64_t file_end = file_off + len;
    for (unsigned i = 0; i < eh.e_shnum && n < max; i++) {
        if (!(sh[i].sh_flags & SHF_EXECINSTR_) || sh[i].sh_type != SHT_PROGBITS_ ||
            sh[i].sh_size == 0)
            continue;
        uint64_t lo = sh[i].sh_offset, hi = sh[i].sh_offset + sh[i].sh_size;
        if (lo < file_off) lo = file_off;
        if (hi > file_end) hi = file_end;
        if (lo >= hi)
            continue;
        out[n].start = map_base + (lo - file_off);
        out[n].end = map_base + (hi - file_off);
        n++;
    }
    free(sh);
    return n;
}

// The same, for an image the runtime loaded itself: every executable segment
// is a file range at `base + p_vaddr`, so the section windows are found by
// offset within the file and translated by the load bias.
int lxrt_elf_exec_sections_path(const char *path, uint64_t load_bias,
                                struct lxrt_range *out, int max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct elf64_ehdr_ eh;
    int n = 0;
    if (read_all(fd, 0, &eh, sizeof eh) && memcmp(eh.e_ident, "\x7f" "ELF", 4) == 0 &&
        eh.e_ident[4] == 2 && eh.e_machine == EM_AARCH64_ && eh.e_shoff && eh.e_shnum &&
        eh.e_shnum <= 4096 && eh.e_shentsize == sizeof(struct elf64_shdr_)) {
        struct elf64_shdr_ *sh = calloc(eh.e_shnum, sizeof *sh);
        if (sh && read_all(fd, eh.e_shoff, sh, (size_t)eh.e_shnum * sizeof *sh)) {
            for (unsigned i = 0; i < eh.e_shnum && n < max; i++) {
                if (!(sh[i].sh_flags & SHF_EXECINSTR_) || sh[i].sh_type != SHT_PROGBITS_ ||
                    sh[i].sh_size == 0 || sh[i].sh_addr == 0)
                    continue;
                out[n].start = load_bias + sh[i].sh_addr;
                out[n].end = load_bias + sh[i].sh_addr + sh[i].sh_size;
                n++;
            }
        }
        free(sh);
    }
    close(fd);
    return n;
}
