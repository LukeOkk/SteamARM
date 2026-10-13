// Which bytes of a PE image are aarch64 instructions.
//
// Wine's ARM64 builds ship their DLLs as PE images that Wine's own loader maps
// (ntdll/unix/virtual.c map_image_into_view: the header, then each section
// from the file, then mprotect to what the section asked for). Valve's Proton
// ARM64 is such a tree: ntdll.dll, kernel32.dll and the rest are ARM64X --
// ARM64, ARM64EC and x64 code in ONE .text, each kind in its own range --
// and libarm64ecfex.dll (FEX as a DLL) is ARM64EC with x64 thunks. The x18
// pass (runtime/x18.c) has to run on the aarch64 ranges and must never
// decode an x64 range as aarch64 (benchmarks/stage57-wine-arm64-native.txt).
//
// Two things are read from the file, the same way elfsect.c reads an ELF:
//
//   * code ranges: the ARM64EC code map (load config CHPEMetadataPointer ->
//     IMAGE_ARM64EC_METADATA.CodeMap: {RVA | type, length}; type 0 ARM64,
//     1 ARM64EC, 2 x64). An image without that metadata is plain ARM64 and
//     every executable section is code;
//   * function bounds: the ARM64 RUNTIME_FUNCTION table ({BeginAddress,
//     UnwindData}: packed when bits 0..1 are set, FunctionLength in bits
//     2..12 in words; otherwise the RVA of .xdata whose first word carries
//     FunctionLength in bits 0..17). As with .eh_frame, a word outside every
//     function is data (jump tables, constant pools) and is left alone.
//     For an image whose file header says ARM64 that table is the exception
//     directory; for an ARM64EC image whose file header says AMD64 (the x64
//     view: libarm64ecfex.dll) the exception directory is the x64 one and
//     the ARM64EC table is the metadata's ExtraRFETable. Each entry has to
//     begin inside an aarch64 code range or it is dropped, so a wrong guess
//     about a table's format costs coverage, never a decoded x64 word.
//
// ARM64X dynamic relocations (the DVRT that switches an image between its
// x64 and ARM64 views) are not applied: Valve's DLLs carry none (MEASURED
// with scripts, stage 59), and the code map and the two tables are read in
// the x64 view as the file holds them.

#include "lxrt.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PE_MACHINE_AMD64   0x8664u
#define PE_MACHINE_ARM64   0xaa64u
#define PE_MACHINE_ARM64EC 0xa641u
#define PE_MACHINE_ARM64X  0xa64eu
#define PE32PLUS_MAGIC     0x20bu
#define SCN_CNT_CODE       0x00000020u
#define SCN_MEM_EXECUTE    0x20000000u
#define DIR_EXCEPTION      3
#define DIR_LOAD_CONFIG    10

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

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

static int why_fail(char *why, size_t n, const char *msg)
{
    if (why && n)
        snprintf(why, n, "%s", msg);
    return -1;
}

// The file offset of `need` bytes at `rva`: inside the headers, or inside a
// section's raw data (the zero-filled tail past SizeOfRawData is not in the
// file). -1 when nowhere.
int64_t lxrt_pe_rva_offset(const struct lxrt_pe_info *info, uint32_t rva, uint32_t need)
{
    if (need && rva <= UINT32_MAX - need && rva + need <= info->size_of_headers)
        return rva;
    for (int i = 0; i < info->nsect; i++) {
        const struct lxrt_pe_sect *s = &info->sect[i];
        if (rva < s->va || rva - s->va >= s->rawsz)
            continue;
        if (need > s->rawsz - (rva - s->va))
            return -1;
        return (int64_t)s->raw + (rva - s->va);
    }
    return -1;
}

static int cmp_range(const void *a, const void *b)
{
    const struct lxrt_range *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static bool in_code(const struct lxrt_pe_info *info, uint64_t rva, uint64_t *end)
{
    for (int i = 0; i < info->ncode; i++)
        if (rva >= info->code[i].start && rva < info->code[i].end) {
            *end = info->code[i].end;
            return true;
        }
    return false;
}

// Raw bytes of the section holding `rva`, loaded once per section.
struct sect_buf { uint8_t *p; };

static const uint8_t *sect_bytes(int fd, const struct lxrt_pe_info *info, struct sect_buf *bufs,
                                 uint32_t rva, uint32_t need)
{
    for (int i = 0; i < info->nsect; i++) {
        const struct lxrt_pe_sect *s = &info->sect[i];
        if (rva < s->va || rva - s->va >= s->rawsz || need > s->rawsz - (rva - s->va))
            continue;
        if (!bufs[i].p) {
            if (s->rawsz > (64u << 20))
                return NULL;
            bufs[i].p = malloc(s->rawsz);
            if (!bufs[i].p || !read_all(fd, s->raw, bufs[i].p, s->rawsz)) {
                free(bufs[i].p);
                bufs[i].p = NULL;
                return NULL;
            }
        }
        return bufs[i].p + (rva - s->va);
    }
    return NULL;
}

// The RUNTIME_FUNCTION table at `rva` (size bytes): function ranges inside
// the code ranges, appended to info->fn.
static bool read_functions(int fd, struct lxrt_pe_info *info, uint32_t rva, uint32_t size)
{
    if (size < 8 || size > (16u << 20))
        return false;
    int64_t off = lxrt_pe_rva_offset(info, rva, size);
    if (off < 0)
        return false;
    uint8_t *tab = malloc(size);
    if (!tab)
        return false;
    if (!read_all(fd, (uint64_t)off, tab, size)) {
        free(tab);
        return false;
    }
    size_t n = size / 8;
    struct lxrt_range *fn = malloc(n * sizeof *fn);
    struct sect_buf *bufs = calloc((size_t)info->nsect, sizeof *bufs);
    if (!fn || !bufs) {
        free(fn);
        free(bufs);
        free(tab);
        return false;
    }
    int m = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t begin = rd32(tab + 8 * i), uw = rd32(tab + 8 * i + 4);
        uint64_t len, code_end;
        if (!in_code(info, begin, &code_end))
            continue;
        if (uw & 3) {
            if ((uw & 3) == 3)
                continue;                              // reserved
            len = (uint64_t)((uw >> 2) & 0x7ff) * 4;
        } else {
            const uint8_t *x = sect_bytes(fd, info, bufs, uw, 4);
            if (!x)
                continue;
            len = (uint64_t)(rd32(x) & 0x3ffff) * 4;
        }
        if (!len)
            continue;
        if (begin + len > code_end)
            len = code_end - begin;                    // never past its code range
        fn[m].start = begin;
        fn[m].end = begin + len;
        m++;
    }
    for (int i = 0; i < info->nsect; i++)
        free(bufs[i].p);
    free(bufs);
    free(tab);
    if (m > 1)
        qsort(fn, (size_t)m, sizeof *fn, cmp_range);
    int k = 0;
    for (int i = 0; i < m; i++) {
        if (k && fn[i].start <= fn[k - 1].end) {
            if (fn[i].end > fn[k - 1].end)
                fn[k - 1].end = fn[i].end;
        } else {
            fn[k++] = fn[i];
        }
    }
    info->fn = fn;
    info->nfn = k;
    return true;
}

void lxrt_pe_info_free(struct lxrt_pe_info *info)
{
    free(info->sect);
    free(info->code);
    free(info->fn);
    memset(info, 0, sizeof *info);
}

// Parse the PE image open on `fd`. 0 when it is an image whose aarch64 code
// the x18 pass can locate (ARM64, ARM64EC, or ARM64X in either view); -1
// otherwise, with a word in `why` (not a PE, x64 only, malformed).
int lxrt_pe_parse_fd(int fd, struct lxrt_pe_info *info, char *why, size_t whylen)
{
    memset(info, 0, sizeof *info);
    uint8_t dos[0x40];
    if (fd < 0 || !read_all(fd, 0, dos, sizeof dos) || dos[0] != 'M' || dos[1] != 'Z')
        return why_fail(why, whylen, "not MZ");
    uint32_t lfanew = rd32(dos + 0x3c);
    if (lfanew < 0x40 || lfanew > (1u << 20))
        return why_fail(why, whylen, "e_lfanew");
    uint8_t coff[24];
    if (!read_all(fd, lfanew, coff, sizeof coff) || memcmp(coff, "PE\0\0", 4) != 0)
        return why_fail(why, whylen, "not PE");
    info->machine = rd16(coff + 4);
    unsigned nsec = rd16(coff + 6), optsize = rd16(coff + 20);
    if (nsec == 0 || nsec > 96)
        return why_fail(why, whylen, "sections");
    if (optsize < 112 + 16 * 8 || optsize > 0x400)
        return why_fail(why, whylen, "optional header");
    uint8_t opt[0x400];
    if (!read_all(fd, lfanew + 24, opt, optsize) || rd16(opt) != PE32PLUS_MAGIC)
        return why_fail(why, whylen, "not PE32+");
    uint64_t image_base = rd64(opt + 24);
    info->size_of_image = rd32(opt + 56);
    info->size_of_headers = rd32(opt + 60);
    uint32_t ndirs = rd32(opt + 108);
    if (ndirs > 16)
        ndirs = 16;
    uint32_t dir_rva[16] = {0}, dir_size[16] = {0};
    for (uint32_t i = 0; i < ndirs; i++) {
        dir_rva[i] = rd32(opt + 112 + 8 * i);
        dir_size[i] = rd32(opt + 116 + 8 * i);
    }
    if (info->machine != PE_MACHINE_ARM64 && info->machine != PE_MACHINE_ARM64EC &&
        info->machine != PE_MACHINE_ARM64X && info->machine != PE_MACHINE_AMD64)
        return why_fail(why, whylen, "machine");

    info->sect = calloc(nsec, sizeof *info->sect);
    if (!info->sect)
        return why_fail(why, whylen, "memory");
    uint8_t *st = malloc(nsec * 40);
    if (!st || !read_all(fd, lfanew + 24 + optsize, st, nsec * 40)) {
        free(st);
        lxrt_pe_info_free(info);
        return why_fail(why, whylen, "section table");
    }
    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *s = st + 40 * i;
        info->sect[i].vsize = rd32(s + 8);
        info->sect[i].va = rd32(s + 12);
        info->sect[i].rawsz = rd32(s + 16);
        info->sect[i].raw = rd32(s + 20);
        info->sect[i].ch = rd32(s + 36);
    }
    info->nsect = (int)nsec;
    free(st);

    // ARM64EC metadata: load config, CHPEMetadataPointer at 0xc8 (a VA).
    uint32_t chpe_rva = 0, extra_rfe = 0, extra_rfe_size = 0;
    if (dir_size[DIR_LOAD_CONFIG] >= 0xd0) {
        int64_t off = lxrt_pe_rva_offset(info, dir_rva[DIR_LOAD_CONFIG], 0xd0);
        uint8_t lc[0xd0];
        if (off >= 0 && read_all(fd, (uint64_t)off, lc, sizeof lc) && rd32(lc) >= 0xd0) {
            uint64_t va = rd64(lc + 0xc8);
            if (va && va >= image_base && va - image_base < info->size_of_image)
                chpe_rva = (uint32_t)(va - image_base);
        }
    }
    if (chpe_rva) {
        int64_t off = lxrt_pe_rva_offset(info, chpe_rva, 0x48);
        uint8_t md[0x48];
        if (off < 0 || !read_all(fd, (uint64_t)off, md, sizeof md)) {
            lxrt_pe_info_free(info);
            return why_fail(why, whylen, "CHPE metadata");
        }
        uint32_t map = rd32(md + 4), count = rd32(md + 8);
        extra_rfe = rd32(md + 0x40);
        extra_rfe_size = rd32(md + 0x44);
        if (count == 0 || count > 65536 || (off = lxrt_pe_rva_offset(info, map, count * 8)) < 0) {
            lxrt_pe_info_free(info);
            return why_fail(why, whylen, "code map");
        }
        uint8_t *ents = malloc(count * 8);
        info->code = calloc(count, sizeof *info->code);
        if (!ents || !info->code || !read_all(fd, (uint64_t)off, ents, count * 8)) {
            free(ents);
            lxrt_pe_info_free(info);
            return why_fail(why, whylen, "code map");
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t e = rd32(ents + 8 * i), len = rd32(ents + 8 * i + 4);
            unsigned type = e & 3;
            uint64_t rva = e & ~3u;
            if (type == 2 || type == 3 || !len || rva >= info->size_of_image)
                continue;                              // x64, or nothing
            if (len > info->size_of_image - rva)
                len = (uint32_t)(info->size_of_image - rva);
            info->code[info->ncode].start = rva;
            info->code[info->ncode].end = rva + len;
            info->ncode++;
        }
        free(ents);
        info->chpe = true;
    } else {
        if (info->machine == PE_MACHINE_AMD64) {
            lxrt_pe_info_free(info);
            return why_fail(why, whylen, "x64 only");
        }
        info->code = calloc(nsec, sizeof *info->code);
        if (!info->code) {
            lxrt_pe_info_free(info);
            return why_fail(why, whylen, "memory");
        }
        for (unsigned i = 0; i < nsec; i++) {
            const struct lxrt_pe_sect *s = &info->sect[i];
            if (!(s->ch & (SCN_CNT_CODE | SCN_MEM_EXECUTE)))
                continue;
            uint64_t len = s->vsize ? s->vsize : s->rawsz;
            if (len > s->rawsz)
                len = s->rawsz;                        // the zero tail is not code
            if (!len || s->va >= info->size_of_image)
                continue;
            info->code[info->ncode].start = s->va;
            info->code[info->ncode].end = s->va + len;
            info->ncode++;
        }
    }
    if (info->ncode > 1)
        qsort(info->code, (size_t)info->ncode, sizeof *info->code, cmp_range);

    // Function bounds. The x64 view of an ARM64EC image keeps the x64 table
    // in the exception directory and the ARM64EC one in ExtraRFETable.
    uint32_t fn_rva, fn_size;
    if (info->machine == PE_MACHINE_AMD64) {
        fn_rva = extra_rfe;
        fn_size = extra_rfe_size;
    } else {
        fn_rva = dir_rva[DIR_EXCEPTION];
        fn_size = dir_size[DIR_EXCEPTION];
    }
    if (fn_rva && fn_size)
        info->has_fn = read_functions(fd, info, fn_rva, fn_size);
    return 0;
}
