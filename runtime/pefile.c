// The aarch64 code of a PE image mapped by the guest's own loader.
//
// Wine ARM64 maps its DLLs itself (ARM64X: ntdll, kernel32, DXVK; ARM64EC:
// libarm64ecfex) section by section from the file, and that code reads x18
// as the TEB pointer -- which Darwin clears. The runtime's x18 pass
// (rewrite.c x18_site) runs only where it knows instructions are: inside
// the code windows a mapping is told about (lxrt_elf_exec_sections) and
// inside a registered function table (elfsect.c lxrt_elf_in_function). For
// an ELF both come from the file; a PE mapping got neither, its x18 sites
// stayed live, and Wine's ARM64X ntdll died on [x18 + 0x60] with x18 = 0
// (MEASURED 2026-10-06, benchmarks/stage57-wine-arm64-native.txt).
//
// Here a PE file behind a mapping is read the same way: its code ranges are
// the ARM64 / ARM64EC entries of the ARM64X CHPE code map (load config
// CHPEMetadataPointer -> IMAGE_ARM64EC_METADATA CodeMap; the x64 entries --
// type 2, the x64 side of an ARM64X DLL and the x64 thunks -- are NOT
// aarch64 and are never decoded as such), or every executable section of a
// plain ARM64 image; a pure x64 image has no aarch64 code at all. The
// windows of a mapping are those ranges translated through the section
// table to the file offsets the mapping covers. For now a window counts as
// one function for the x18 pass (the .pdata bounds would exclude data
// between functions; PE text keeps its constants in .rdata, so the risk is
// small, and the pass refuses what it cannot plan). Windows are remembered
// by address for the scans that have no file at hand (an mprotect to
// executable, a W^X flip: rewrite.c asks lxrt_pe_code_windows).
#include "lxrt.h"
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool lxrt_trace_on(void);
void lxrt_fn_register(uint64_t lo, uint64_t hi, struct lxrt_range *r, int n);

struct pe_sect { uint32_t va, vsize, raw, rawsz, ch; };
struct pe_file {
    dev_t dev; ino_t ino;
    bool is_pe, has_code;
    uint16_t machine;
    bool chpe;
    struct pe_sect *sect; int nsect;
    struct lxrt_range *code; int ncode;   // RVA ranges of aarch64 code, sorted
};

#define PE_CACHE 64
static struct pe_file g_files[PE_CACHE];
static int g_nfiles;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static bool pread_all(int fd, void *buf, size_t n, uint64_t off)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r <= 0) return false;
        p += r; off += (uint64_t)r; n -= (size_t)r;
    }
    return true;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// File offset of `need` bytes at `rva`, or -1.
static int64_t rva_offset(const struct pe_file *f, uint32_t rva, uint32_t need)
{
    for (int i = 0; i < f->nsect; i++) {
        const struct pe_sect *s = &f->sect[i];
        uint32_t span = s->vsize > s->rawsz ? s->vsize : s->rawsz;
        if (rva >= s->va && rva < s->va + span) {
            uint32_t in = rva - s->va;
            if (in + need > s->rawsz) return -1;
            return (int64_t)s->raw + in;
        }
    }
    return -1;
}

static int cmp_range(const void *a, const void *b)
{
    const struct lxrt_range *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static void add_code(struct pe_file *f, uint32_t rva, uint32_t len)
{
    if (!len) return;
    struct lxrt_range *n = realloc(f->code, (size_t)(f->ncode + 1) * sizeof *n);
    if (!n) return;
    f->code = n;
    f->code[f->ncode].start = rva;
    f->code[f->ncode].end = (uint64_t)rva + len;
    f->ncode++;
}

static void parse(int fd, struct pe_file *f)
{
    uint8_t dos[0x40];
    if (!pread_all(fd, dos, sizeof dos, 0) || dos[0] != 'M' || dos[1] != 'Z') return;
    uint32_t pe = rd32(dos + 0x3C);
    uint8_t hdr[24 + 240];
    if (pe > (64u << 20) || !pread_all(fd, hdr, sizeof hdr, pe)) return;
    if (memcmp(hdr, "PE\0\0", 4) != 0) return;
    f->is_pe = true;
    f->machine = rd16(hdr + 4);
    unsigned nsect = rd16(hdr + 6), optsz = rd16(hdr + 20);
    const uint8_t *opt = hdr + 24;
    if (optsz < 112 + 16 * 8 || rd16(opt) != 0x20B) return;   // PE32+ only
    uint64_t image_base = rd64(opt + 24);
    unsigned ndirs = rd32(opt + 108);
    // sections
    if (nsect == 0 || nsect > 96) return;
    size_t slen = (size_t)nsect * 40;
    uint8_t *sb = malloc(slen);
    if (!sb || !pread_all(fd, sb, slen, (uint64_t)pe + 24 + optsz)) { free(sb); return; }
    f->sect = calloc(nsect, sizeof *f->sect);
    if (!f->sect) { free(sb); return; }
    f->nsect = (int)nsect;
    for (unsigned i = 0; i < nsect; i++) {
        const uint8_t *s = sb + i * 40;
        f->sect[i].vsize = rd32(s + 8);
        f->sect[i].va = rd32(s + 12);
        f->sect[i].rawsz = rd32(s + 16);
        f->sect[i].raw = rd32(s + 20);
        f->sect[i].ch = rd32(s + 36);
    }
    free(sb);
    // CHPE code map, when there is one (ARM64X and ARM64EC images)
    if (ndirs > 10) {
        uint32_t lc_rva = rd32(opt + 112 + 10 * 8), lc_size = rd32(opt + 112 + 10 * 8 + 4);
        int64_t lc_off = lc_rva && lc_size ? rva_offset(f, lc_rva, 4) : -1;
        if (lc_off >= 0) {
            uint8_t lc[0xD0];
            uint32_t have = 0;
            if (pread_all(fd, lc, 4, (uint64_t)lc_off)) have = rd32(lc);
            if (have >= 0xD0 && rva_offset(f, lc_rva, 0xD0) >= 0 && pread_all(fd, lc, 0xD0, (uint64_t)lc_off)) {
                uint64_t chpe_va = rd64(lc + 0xC8);
                if (chpe_va > image_base) {
                    uint32_t chpe_rva = (uint32_t)(chpe_va - image_base);
                    int64_t m_off = rva_offset(f, chpe_rva, 12);
                    uint8_t m[12];
                    if (m_off >= 0 && pread_all(fd, m, 12, (uint64_t)m_off)) {
                        uint32_t map_rva = rd32(m + 4), count = rd32(m + 8);
                        int64_t map_off = count && count < 65536 ? rva_offset(f, map_rva, count * 8) : -1;
                        if (map_off >= 0) {
                            uint8_t *e = malloc((size_t)count * 8);
                            if (e && pread_all(fd, e, (size_t)count * 8, (uint64_t)map_off)) {
                                f->chpe = true;
                                for (uint32_t i = 0; i < count; i++) {
                                    uint32_t start = rd32(e + i * 8), len = rd32(e + i * 8 + 4);
                                    if ((start & 3) <= 1)          // 0 ARM64, 1 ARM64EC; 2 is x64
                                        add_code(f, start & ~3u, len);
                                }
                            }
                            free(e);
                        }
                    }
                }
            }
        }
    }
    if (!f->chpe) {
        if (f->machine == 0xAA64) {
            for (int i = 0; i < f->nsect; i++)
                if (f->sect[i].ch & 0x20000000u)   // IMAGE_SCN_MEM_EXECUTE
                    add_code(f, f->sect[i].va, f->sect[i].rawsz < f->sect[i].vsize ? f->sect[i].rawsz : f->sect[i].vsize);
        }
        // 0x8664 without CHPE: x64 only, nothing for an aarch64 pass
    }
    if (f->ncode > 1)
        qsort(f->code, (size_t)f->ncode, sizeof *f->code, cmp_range);
    f->has_code = f->ncode > 0;
}

static struct pe_file *lookup(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nfiles; i++)
        if (g_files[i].dev == st.st_dev && g_files[i].ino == st.st_ino) {
            pthread_mutex_unlock(&g_lock);
            return &g_files[i];
        }
    struct pe_file *f;
    if (g_nfiles < PE_CACHE) {
        f = &g_files[g_nfiles++];
    } else {
        f = &g_files[g_nfiles - 1];          // the last slot is recycled
        free(f->sect); free(f->code);
    }
    memset(f, 0, sizeof *f);
    f->dev = st.st_dev; f->ino = st.st_ino;
    parse(fd, f);
    if (f->is_pe && lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] PE image on fd %d: machine 0x%x%s, %d sections, %d aarch64 code range(s)\n",
                fd, f->machine, f->chpe ? " (CHPE code map)" : "", f->nsect, f->ncode);
    pthread_mutex_unlock(&g_lock);
    return f;
}

bool lxrt_pe_is(int fd)
{
    struct pe_file *f = lookup(fd);
    return f && f->is_pe;
}

// ------------------------------------------------- windows known by address

#define PE_WINDOWS 512
static struct lxrt_range g_win[PE_WINDOWS];
static int g_nwin;

static void remember(uint64_t lo, uint64_t hi)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nwin; i++)
        if (g_win[i].start == lo && g_win[i].end == hi) { pthread_mutex_unlock(&g_lock); return; }
    if (g_nwin < PE_WINDOWS) {
        g_win[g_nwin].start = lo;
        g_win[g_nwin].end = hi;
        g_nwin++;
    }
    pthread_mutex_unlock(&g_lock);
}

void lxrt_pe_forget(uint64_t addr, uint64_t len)
{
    if (!len) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nwin; ) {
        if (g_win[i].start < addr + len && g_win[i].end > addr)
            g_win[i] = g_win[--g_nwin];
        else
            i++;
    }
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------- images found in memory
//
// Wine's loader does not mmap a DLL's sections when their file alignment is
// finer than the host page (4 KiB sections, 16 KiB pages): it maps anonymous
// memory at the image base and reads the file into it, then makes the code
// executable with mprotect (MEASURED: ntdll.dll, 2026-10-06). No file is at
// hand then, but the whole image is in memory: the MZ/PE headers at a 64 KiB
// boundary below the code, the load config, the CHPE code map, all by RVA.

static bool mem_read(uint64_t addr, void *buf, size_t n)
{
    mach_vm_size_t got = 0;
    return addr >= 0x10000 && mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)addr, n,
                                                     (mach_vm_address_t)(uintptr_t)buf, &got) == KERN_SUCCESS && got == n;
}

// Negative cache: 64 KiB-aligned starts that were looked at and hold no image.
#define NEG_CACHE 256
static uint64_t g_neg[NEG_CACHE];
static unsigned g_neg_n;

// Parse the image at `base` (headers readable there) and remember its
// aarch64 code windows. True when it was a PE image with such code.
static bool parse_in_memory(uint64_t base)
{
    uint8_t dos[0x40];
    if (!mem_read(base, dos, sizeof dos) || dos[0] != 'M' || dos[1] != 'Z') return false;
    uint32_t pe = rd32(dos + 0x3C);
    uint8_t hdr[24 + 240];
    if (pe < 0x40 || pe > (1u << 20) || !mem_read(base + pe, hdr, sizeof hdr) || memcmp(hdr, "PE\0\0", 4) != 0)
        return false;
    uint16_t machine = rd16(hdr + 4);
    unsigned nsect = rd16(hdr + 6), optsz = rd16(hdr + 20);
    const uint8_t *opt = hdr + 24;
    if (optsz < 112 + 16 * 8 || rd16(opt) != 0x20B) return false;
    uint64_t image_base = rd64(opt + 24);
    uint32_t size_of_image = rd32(opt + 56);
    unsigned ndirs = rd32(opt + 108);
    if (!size_of_image || size_of_image > (512u << 20) || nsect == 0 || nsect > 96) return false;
    struct lxrt_range code[64];
    int ncode = 0;
    bool chpe = false;
    if (ndirs > 10) {
        uint32_t lc_rva = rd32(opt + 112 + 10 * 8), lc_size = rd32(opt + 112 + 10 * 8 + 4);
        uint8_t lc[0xD0];
        if (lc_rva && lc_size >= 0xD0 && lc_rva + 0xD0 <= size_of_image && mem_read(base + lc_rva, lc, 0xD0) && rd32(lc) >= 0xD0) {
            uint64_t chpe_va = rd64(lc + 0xC8);
            if (chpe_va > image_base && chpe_va - image_base + 12 <= size_of_image) {
                uint8_t m[12];
                if (mem_read(base + (chpe_va - image_base), m, 12)) {
                    uint32_t map_rva = rd32(m + 4), count = rd32(m + 8);
                    if (count && count < 4096 && map_rva + (uint64_t)count * 8 <= size_of_image) {
                        uint8_t *e = malloc((size_t)count * 8);
                        if (e && mem_read(base + map_rva, e, (size_t)count * 8)) {
                            chpe = true;
                            for (uint32_t i = 0; i < count && ncode < 64; i++) {
                                uint32_t st = rd32(e + i * 8), len = rd32(e + i * 8 + 4);
                                if ((st & 3) <= 1 && len) {
                                    code[ncode].start = base + (st & ~3u);
                                    code[ncode].end = base + (st & ~3u) + len;
                                    ncode++;
                                }
                            }
                        }
                        free(e);
                    }
                }
            }
        }
    }
    if (!chpe && machine == 0xAA64) {
        size_t slen = (size_t)nsect * 40;
        uint8_t *sb = malloc(slen);
        if (sb && mem_read(base + pe + 24 + optsz, sb, slen)) {
            for (unsigned i = 0; i < nsect && ncode < 64; i++) {
                const uint8_t *sc = sb + i * 40;
                uint32_t vsize = rd32(sc + 8), va = rd32(sc + 12), ch = rd32(sc + 36);
                if ((ch & 0x20000000u) && vsize) {
                    code[ncode].start = base + va;
                    code[ncode].end = base + va + vsize;
                    ncode++;
                }
            }
        }
        free(sb);
    }
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] PE image in memory at 0x%llx: machine 0x%x%s, size 0x%x, %d aarch64 code range(s)\n",
                (unsigned long long)base, machine, chpe ? " (CHPE code map)" : "", size_of_image, ncode);
    for (int i = 0; i < ncode; i++) {
        uint64_t lo = (code[i].start + 3) & ~3ull, hi = code[i].end & ~3ull;
        if (lo >= hi || hi > base + size_of_image) continue;
        struct lxrt_range *fn = malloc(sizeof *fn);
        if (fn) { fn->start = lo; fn->end = hi; lxrt_fn_register(lo, hi, fn, 1); }
        remember(lo, hi);
    }
    return ncode > 0;
}

// An image whose code could hold `addr`: its headers at a 64 KiB boundary
// at or below addr (Wine places images at 64 KiB granularity), within 64 MiB.
static void discover(uint64_t addr)
{
    if (lxrt_jit_contains(addr))
        return;                                 // a JIT buffer, never an image
    uint64_t at = addr & ~0xFFFFull;
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < g_neg_n && i < NEG_CACHE; i++)
        if (g_neg[i] == at) { pthread_mutex_unlock(&g_lock); return; }
    pthread_mutex_unlock(&g_lock);
    for (unsigned step = 0; step < 1024 && at >= 0x10000; step++, at -= 0x10000) {
        uint8_t w[4];
        if (!mem_read(at, w, 4)) {
            // Unmapped: no image spans it (images are contiguous); stop.
            break;
        }
        if (w[0] == 'M' && w[1] == 'Z') {
            if (parse_in_memory(at)) {
                struct lxrt_range r;
                if (lxrt_pe_code_windows(addr, addr + 4, &r, 1) > 0)
                    return;                     // found: the image holding addr
            }
        }
    }
    pthread_mutex_lock(&g_lock);
    g_neg[g_neg_n++ % NEG_CACHE] = addr & ~0xFFFFull;
    if (g_neg_n > NEG_CACHE) g_neg_n = NEG_CACHE;
    pthread_mutex_unlock(&g_lock);
}

int lxrt_pe_code_windows(uint64_t start, uint64_t end, struct lxrt_range *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nwin && n < max; i++) {
        uint64_t lo = g_win[i].start > start ? g_win[i].start : start;
        uint64_t hi = g_win[i].end < end ? g_win[i].end : end;
        if (lo < hi) { out[n].start = lo; out[n].end = hi; n++; }
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

bool lxrt_pe_intersects(uint64_t start, uint64_t end)
{
    struct lxrt_range r;
    if (lxrt_pe_code_windows(start, end, &r, 1) > 0)
        return true;
    discover(start);
    return lxrt_pe_code_windows(start, end, &r, 1) > 0;
}

// The aarch64 code windows of the mapping [file_off, file_off + len) of the
// PE on `fd`, placed at `addr` (what lxrt_elf_exec_sections gives for an
// ELF), registered as function tables and remembered by address. 0 for a
// file that is not a PE or has no aarch64 code.
int lxrt_pe_exec_sections(int fd, uint64_t file_off, uint64_t len, uint64_t addr,
                          struct lxrt_range *code, int max)
{
    struct pe_file *f = lookup(fd);
    if (!f || !f->is_pe || !f->has_code || !len)
        return 0;
    int n = 0;
    for (int i = 0; i < f->ncode && n < max; i++) {
        uint32_t rva = (uint32_t)f->code[i].start, clen = (uint32_t)(f->code[i].end - f->code[i].start);
        // the range may span sections only in theory; it is placed by its first byte's section
        int64_t raw = rva_offset(f, rva, 0);
        if (raw < 0) continue;
        // clip to the section's raw bytes
        for (int s = 0; s < f->nsect; s++) {
            const struct pe_sect *sc = &f->sect[s];
            if (rva >= sc->va && rva < sc->va + (sc->vsize > sc->rawsz ? sc->vsize : sc->rawsz)) {
                uint32_t avail = sc->rawsz > (rva - sc->va) ? sc->rawsz - (rva - sc->va) : 0;
                if (clen > avail) clen = avail;
                break;
            }
        }
        uint64_t rs = (uint64_t)raw, re = rs + clen;
        uint64_t lo = rs > file_off ? rs : file_off, hi = re < file_off + len ? re : file_off + len;
        if (lo >= hi) continue;
        uint64_t wlo = addr + (lo - file_off), whi = addr + (hi - file_off);
        wlo = (wlo + 3) & ~3ull; whi &= ~3ull;
        if (wlo >= whi) continue;
        code[n].start = wlo; code[n].end = whi; n++;
        struct lxrt_range *fn = malloc(sizeof *fn);
        if (fn) { fn->start = wlo; fn->end = whi; lxrt_fn_register(wlo, whi, fn, 1); }
        remember(wlo, whi);
    }
    if (n && lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] PE code mapped at 0x%llx+0x%llx (file offset 0x%llx): %d window(s), first 0x%llx-0x%llx\n",
                (unsigned long long)addr, (unsigned long long)len, (unsigned long long)file_off, n,
                (unsigned long long)code[0].start, (unsigned long long)code[0].end);
    return n;
}
