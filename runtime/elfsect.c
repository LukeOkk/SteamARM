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
#include <pthread.h>
#include <stdbool.h>
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

struct elf64_phdr_ {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

static bool read_all(int fd, uint64_t off, void *buf, size_t len);

// Some large ELF images reserve their whole span with PROT_NONE and then map
// PT_LOADs over it. A complete host page between two PT_LOADs is outside the
// image contract and can hold a trampoline island near otherwise unreachable
// code. Only offer a gap proven by the file's program headers; never borrow a
// page of a LOAD, including its zero-filled p_memsz tail.
bool lxrt_elf_gap_before_exec(int fd, uint64_t file_off, uint64_t map_base,
                              struct lxrt_range *gap)
{
    struct elf64_ehdr_ eh;
    if (fd < 0 || !gap || !read_all(fd, 0, &eh, sizeof eh) ||
        memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0 || eh.e_ident[4] != 2 ||
        eh.e_ident[5] != 1 || eh.e_machine != EM_AARCH64_ ||
        eh.e_phentsize != sizeof(struct elf64_phdr_) ||
        eh.e_phnum == 0 || eh.e_phnum > 128)
        return false;
    struct elf64_phdr_ ph[128];
    if (!read_all(fd, eh.e_phoff, ph, (size_t)eh.e_phnum * sizeof ph[0]))
        return false;
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != 1 /* PT_LOAD */ || !(ph[i].p_flags & 1 /* PF_X */) ||
            LXRT_ALIGN_DOWN(ph[i].p_offset, 4096) != file_off)
            continue;
        uint64_t exec_va = LXRT_ALIGN_DOWN(ph[i].p_vaddr, 4096);
        if (map_base < exec_va)
            return false;
        uint64_t bias = map_base - exec_va;
        uint64_t prev_end = 0;
        for (unsigned j = 0; j < eh.e_phnum; j++) {
            if (ph[j].p_type != 1 || ph[j].p_vaddr > UINT64_MAX - ph[j].p_memsz)
                continue;
            uint64_t end = ph[j].p_vaddr + ph[j].p_memsz;
            if (end <= exec_va && end > prev_end)
                prev_end = end;
        }
        if (!prev_end || bias > UINT64_MAX - exec_va ||
            bias > UINT64_MAX - prev_end)
            return false;
        uint64_t lo = LXRT_ALIGN_UP(bias + prev_end, LXRT_HOST_PAGE);
        uint64_t hi = LXRT_ALIGN_DOWN(bias + exec_va, LXRT_HOST_PAGE);
        if (hi <= lo)
            return false;
        // A non-adjacent LOAD may still overlap the candidate interval.
        for (unsigned j = 0; j < eh.e_phnum; j++) {
            if (ph[j].p_type != 1 || ph[j].p_vaddr > UINT64_MAX - ph[j].p_memsz)
                continue;
            if (ph[j].p_vaddr > UINT64_MAX - bias ||
                ph[j].p_memsz > UINT64_MAX - bias - ph[j].p_vaddr)
                return false;
            uint64_t a = bias + ph[j].p_vaddr;
            uint64_t b = a + ph[j].p_memsz;
            if (a < hi && b > lo)
                return false;
        }
        *gap = (struct lxrt_range){ lo, hi };
        return true;
    }
    return false;
}

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

// ---------------------------------------------------------------- functions
//
// .text is not all instructions either. Hand-written assembly keeps constant
// tables there -- OpenSSL's aarch64 SHA/AES/GHASH round constants sit between
// the functions -- and a table word that happens to decode as "uses x18" was
// rewritten into a branch. Valve's native arm64 Steam client (static OpenSSL)
// then computed wrong hashes: every TLS handshake ended in an alert and the
// client could not download anything (MEASURED: "http error 0"; with the x18
// pass off it downloaded, but real x18 uses corrupted its package checksums).
// The compiler describes every function it emits in .eh_frame; the x18 pass
// now touches only words inside an FDE's range when the file has FDEs.

struct fn_set {
    uint64_t lo, hi;                // the code window the ranges belong to
    struct lxrt_range *r;           // sorted, merged
    int n;
};
static struct fn_set *g_fn;
static int g_nfn, g_capfn;
static pthread_mutex_t g_fn_lock = PTHREAD_MUTEX_INITIALIZER;

static int cmp_range(const void *a, const void *b)
{
    const struct lxrt_range *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static void fn_register(uint64_t lo, uint64_t hi, struct lxrt_range *r, int n)
{
    qsort(r, (size_t)n, sizeof *r, cmp_range);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && r[i].start <= r[m - 1].end) {
            if (r[i].end > r[m - 1].end) r[m - 1].end = r[i].end;
        } else {
            r[m++] = r[i];
        }
    }
    pthread_mutex_lock(&g_fn_lock);
    for (int i = 0; i < g_nfn; i++)       // the same window mapped again: replace
        if (g_fn[i].lo < hi && g_fn[i].hi > lo) {
            free(g_fn[i].r);
            g_fn[i] = g_fn[--g_nfn];
            i--;
        }
    if (g_nfn == g_capfn) {
        int cap = g_capfn ? g_capfn * 2 : 64;
        struct fn_set *nf = realloc(g_fn, (size_t)cap * sizeof *nf);
        if (!nf) { pthread_mutex_unlock(&g_fn_lock); free(r); return; }
        g_fn = nf; g_capfn = cap;
    }
    g_fn[g_nfn++] = (struct fn_set){ lo, hi, r, m };
    pthread_mutex_unlock(&g_fn_lock);
}

// Is `addr` an instruction as far as the function tables know? True when no
// table covers it (no .eh_frame: every executable-section word counts).
bool lxrt_elf_in_function(uint64_t addr)
{
    bool ok = true;
    pthread_mutex_lock(&g_fn_lock);
    for (int i = 0; i < g_nfn; i++) {
        if (addr < g_fn[i].lo || addr >= g_fn[i].hi)
            continue;
        int a = 0, b = g_fn[i].n - 1;
        ok = false;
        while (a <= b) {
            int m = (a + b) / 2;
            if (addr < g_fn[i].r[m].start) b = m - 1;
            else if (addr >= g_fn[i].r[m].end) a = m + 1;
            else { ok = true; break; }
        }
        break;
    }
    pthread_mutex_unlock(&g_fn_lock);
    return ok;
}

static uint64_t uleb(const uint8_t **p, const uint8_t *end)
{
    uint64_t v = 0; unsigned sh = 0;
    while (*p < end) { uint8_t b = *(*p)++; v |= (uint64_t)(b & 0x7f) << sh; sh += 7; if (!(b & 0x80)) break; }
    return v;
}

// Read one DW_EH_PE-encoded pointer at `*p` (vaddr `va` of that field).
static bool eh_ptr(const uint8_t **p, const uint8_t *end, uint8_t enc, uint64_t va, uint64_t *out)
{
    uint64_t v;
    switch (enc & 0x0f) {
    case 0x00: if (end - *p < 8) return false; memcpy(&v, *p, 8); *p += 8; break;
    case 0x03: { uint32_t x; if (end - *p < 4) return false; memcpy(&x, *p, 4); *p += 4; v = x; break; }
    case 0x0b: { int32_t x; if (end - *p < 4) return false; memcpy(&x, *p, 4); *p += 4; v = (uint64_t)(int64_t)x; break; }
    case 0x04: case 0x0c: if (end - *p < 8) return false; memcpy(&v, *p, 8); *p += 8; break;
    default: return false;
    }
    if ((enc & 0x70) == 0x10) v += va;
    *out = v;
    return true;
}

// FDE pc ranges (vaddrs) of the .eh_frame section held in `buf` at vaddr `sva`.
static int parse_eh_frame(const uint8_t *buf, size_t size, uint64_t sva, struct lxrt_range **out)
{
    int n = 0, cap = 0;
    struct lxrt_range *r = NULL;
    uint8_t cie_enc[64]; uint64_t cie_off[64]; int ncie = 0;
    const uint8_t *p = buf, *end = buf + size;
    while (end - p >= 4) {
        const uint8_t *rec = p;
        uint32_t len32; memcpy(&len32, p, 4); p += 4;
        if (len32 == 0) break;
        if (len32 == 0xffffffff) break;                  // 64-bit DWARF: not used on aarch64 Linux
        const uint8_t *next = p + len32;
        if (next > end) break;
        uint32_t id; memcpy(&id, p, 4);
        const uint8_t *idp = p; p += 4;
        if (id == 0) {                                   // CIE
            uint8_t enc = 0x00;
            uint8_t ver = *p++;
            const char *aug = (const char *)p;
            while (p < next && *p) p++;
            p++;
            uleb(&p, next); uleb(&p, next);              // code / data alignment
            if (ver == 1) p++; else uleb(&p, next);      // return register
            if (aug[0] == 'z') {
                uleb(&p, next);
                for (const char *a = aug + 1; *a && p < next; a++) {
                    if (*a == 'R') enc = *p++;
                    else if (*a == 'L') p++;
                    else if (*a == 'P') { uint8_t pe = *p++; uint64_t dummy;
                        if (!eh_ptr(&p, next, pe & 0x7f, 0, &dummy)) break; }
                    else if (*a == 'S' || *a == 'B') continue;
                    else break;
                }
            }
            if (ncie < 64) { cie_off[ncie] = (uint64_t)(rec - buf); cie_enc[ncie] = enc; ncie++; }
        } else {                                         // FDE
            uint64_t cie = (uint64_t)(idp - buf) - id;
            uint8_t enc = 0x1b;
            for (int i = 0; i < ncie; i++) if (cie_off[i] == cie) { enc = cie_enc[i]; break; }
            uint64_t begin, range;
            if (eh_ptr(&p, next, enc, sva + (uint64_t)(p - buf), &begin) &&
                eh_ptr(&p, next, enc & 0x0f, 0, &range) && range) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 1024;
                    struct lxrt_range *nr = realloc(r, (size_t)cap * sizeof *nr);
                    if (!nr) break;
                    r = nr;
                }
                r[n].start = begin; r[n].end = begin + range; n++;
            }
        }
        p = next;
    }
    *out = r;
    return n;
}

// Register the function ranges of the file on `fd` for the exec section
// [sh_addr, +size) mapped at `mapped` (vaddr -> mapped address by `delta`).
static void register_functions(int fd, const struct elf64_ehdr_ *eh, const struct elf64_shdr_ *sh,
                               uint64_t win_lo, uint64_t win_hi, int64_t delta)
{
    const char *all_text = getenv("LXRT_X18_ALL_TEXT");
    if (all_text && *all_text) {
        char path[1024];
        if (fcntl(fd, F_GETPATH, path) == 0 && strstr(path, all_text))
            return;
    }
    if (eh->e_shstrndx >= eh->e_shnum) return;
    const struct elf64_shdr_ *ss = &sh[eh->e_shstrndx];
    if (ss->sh_size == 0 || ss->sh_size > (1u << 20)) return;
    char *names = malloc(ss->sh_size);
    if (!names || !read_all(fd, ss->sh_offset, names, ss->sh_size)) { free(names); return; }
    for (unsigned i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_name >= ss->sh_size || strcmp(names + sh[i].sh_name, ".eh_frame") != 0)
            continue;
        if (sh[i].sh_size == 0 || sh[i].sh_size > (64u << 20)) break;
        uint8_t *buf = malloc(sh[i].sh_size);
        if (buf && read_all(fd, sh[i].sh_offset, buf, sh[i].sh_size)) {
            struct lxrt_range *r = NULL;
            int n = parse_eh_frame(buf, sh[i].sh_size, sh[i].sh_addr, &r);
            if (n > 0) {
                for (int k = 0; k < n; k++) { r[k].start += (uint64_t)delta; r[k].end += (uint64_t)delta; }
                fn_register(win_lo, win_hi, r, n);
            } else {
                free(r);
            }
        }
        free(buf);
        break;
    }
    free(names);
}

// BoringSSL's FIPS module hashes its own code between the dynamic symbols
// BORINGSSL_bcm_text_start and BORINGSSL_bcm_text_end at load time (Android's
// libcrypto.so). When both lie inside the exec section [sec_lo, sec_hi) (in
// vaddrs) and inside the mapped window, register the range with tls.c so its
// thread-pointer reads stay byte-for-byte (tls.c, "Kept TLS reads").
static void register_fips(int fd, const struct elf64_ehdr_ *eh, const struct elf64_shdr_ *sh,
                          uint64_t sec_lo, uint64_t sec_hi, uint64_t win_lo, uint64_t win_hi,
                          int64_t delta)
{
    if (!lxrt_tlskeep_enabled())
        return;
    for (unsigned i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != 11 /* SHT_DYNSYM */ || sh[i].sh_entsize != 24 ||
            sh[i].sh_link >= eh->e_shnum || sh[i].sh_size == 0 || sh[i].sh_size > (64u << 20))
            continue;
        const struct elf64_shdr_ *str = &sh[sh[i].sh_link];
        if (str->sh_size == 0 || str->sh_size > (64u << 20))
            return;
        // The names first: nearly every library lacks them, and a search of
        // .dynstr is cheaper than walking .dynsym (libcef's is large).
        char *names = malloc(str->sh_size + 1);
        if (!names || !read_all(fd, str->sh_offset, names, str->sh_size) ||
            !memmem(names, str->sh_size, "BORINGSSL_bcm_text_", 19)) {
            free(names);
            return;
        }
        names[str->sh_size] = '\0';
        uint8_t *syms = malloc(sh[i].sh_size);
        uint64_t lo = 0, hi = 0;
        if (syms && read_all(fd, sh[i].sh_offset, syms, sh[i].sh_size)) {
            for (uint64_t k = 0; k + 24 <= sh[i].sh_size; k += 24) {
                uint32_t nm;
                uint64_t val;
                memcpy(&nm, syms + k, 4);
                memcpy(&val, syms + k + 8, 8);
                if (nm >= str->sh_size || !val)
                    continue;
                if (!strcmp(names + nm, "BORINGSSL_bcm_text_start")) lo = val;
                else if (!strcmp(names + nm, "BORINGSSL_bcm_text_end")) hi = val;
            }
        }
        free(syms);
        free(names);
        if (lo && hi > lo && lo >= sec_lo && hi <= sec_hi) {
            uint64_t s = lo + (uint64_t)delta, e = hi + (uint64_t)delta;
            if (s >= win_lo && e <= win_hi)
                lxrt_tlskeep_add(s, e, "BoringSSL FIPS module");
        }
        return;
    }
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
    // Function ranges, for the windows just found: vaddr v maps to
    // map_base + (v - sh_addr + sh_offset - file_off) inside those sections.
    for (int k = 0; k < n; k++)
        for (unsigned i = 0; i < eh.e_shnum; i++)
            if ((sh[i].sh_flags & SHF_EXECINSTR_) && sh[i].sh_type == SHT_PROGBITS_ &&
                out[k].start == map_base + ((sh[i].sh_offset > file_off ? sh[i].sh_offset : file_off) - file_off)) {
                register_functions(fd, &eh, sh, out[k].start, out[k].end,
                                   (int64_t)(map_base + sh[i].sh_offset - file_off) - (int64_t)sh[i].sh_addr);
                register_fips(fd, &eh, sh, sh[i].sh_addr, sh[i].sh_addr + sh[i].sh_size,
                              out[k].start, out[k].end,
                              (int64_t)(map_base + sh[i].sh_offset - file_off) - (int64_t)sh[i].sh_addr);
                break;
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
                register_functions(fd, &eh, sh, out[n].start, out[n].end, (int64_t)load_bias);
                register_fips(fd, &eh, sh, sh[i].sh_addr, sh[i].sh_addr + sh[i].sh_size,
                              out[n].start, out[n].end, (int64_t)load_bias);
                n++;
            }
        }
        free(sh);
    }
    close(fd);
    return n;
}
