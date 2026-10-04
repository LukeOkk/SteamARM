/* Host-only check of what the runtime keeps per guest file mapping across
 * munmap: the x18 pass's function tables (runtime/elfsect.c) and the
 * LXRT_GUEST_FAULTS file names (runtime/memlog.c). No guest code runs.
 *
 * The Steam client's controller thread dlopens and dlcloses libusb about four
 * times a second; each cycle left four 16 KiB function tables and three path
 * copies behind (MEASURED on the live client). This replays that cycle the
 * way glibc 2.39 maps a library with AT_PAGESZ 4096 -- ld.so.cache mapped and
 * unmapped at its byte length, the first PT_LOAD mapped over the whole span
 * (l_map_end - l_map_start, unrounded), the reservation trimmed, the other
 * PT_LOADs MAP_FIXED on 4 KiB pages, dlclose unmapping the unrounded span --
 * at a different address every cycle, and calls the hooks the way dispatch.c
 * does. It checks that
 *   - the heap does not grow with the number of cycles,
 *   - every word of every code window (the words the x18 pass asks
 *     lxrt_elf_in_function about) gets the answer it got in the library's
 *     first mapping, also when other libraries were mapped at that address
 *     before,
 *   - after dlclose no table answers for the span and no file name is listed,
 *     while the live mapping's file name is found,
 *   - the cost of one lookup does not grow with the number of cycles.
 * "sig" lines hash the answers in the first mapping (and in a mapping that
 * starts inside .text): built against an older elfsect.c they must not
 * change.
 *
 *   make build/elfsect_unmap_check
 *   build/elfsect_unmap_check [-n CYCLES] [--raw-len] LIB.so...
 *
 * LIB: aarch64 Linux shared objects, e.g. build/libvulkan.so.1 and a guest
 * root's usr/lib/libusb-1.0.so.0.4.0. --raw-len hands the hooks the guest's
 * length unrounded, to show what the 4 KiB rounding in do_munmap_inner is for.
 */
#include "lxrt.h"

#include <fcntl.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

bool lxrt_elf_in_function(uint64_t addr);       /* elfsect.c, for rewrite.c */

/* What elfsect.c and memlog.c take from the rest of the runtime. */
bool lxrt_tlskeep_enabled(void) { return false; }
void lxrt_tlskeep_add(uint64_t s, uint64_t e, const char *why) { (void)s; (void)e; (void)why; }
FILE *lxrt_trace_stream(void) { return stderr; }
uint64_t lxrt_last_guest_lr(void) { return 0; }

#define PAGE 4096u
#define CACHE_LEN 131579u           /* an ld.so.cache, mapped at its byte length */
#define MAX_LIBS 16
#define NOT_CODE_WINDOW 2           /* ref[] value of a word outside every window */

struct ph { uint32_t type, flags; uint64_t off, va, pa, filesz, memsz, align; };
struct sh { uint32_t name, type; uint64_t flags, addr, off, size; uint32_t link, info; uint64_t align, entsize; };

struct lib {
    const char *path;
    int fd;
    struct ph load[16];
    int nload;
    uint64_t va0, span, align;      /* first PT_LOAD page, l_map_end - l_map_start, max p_align */
    uint64_t rw_lo, rw_off;         /* the last PT_LOAD's first page, for the file-name lookup */
    struct lxrt_range win[32];      /* code windows, relative to the load address */
    int nwin;
    uint8_t *ref;                   /* each word's answer in the first mapping */
    size_t nwords, nwin_words, nfn_words;
    uint64_t sig;
    /* The executable PT_LOAD mapped alone from the middle of the largest
     * executable section: a window that starts inside .text. */
    uint64_t part_off, part_len, part_sig;
};

static struct lib g_libs[MAX_LIBS];
static int g_nlibs;
static bool g_raw_len;
static int g_failures;
static struct lib *g_collect;       /* guest_mmap_file records windows here */
static uint64_t g_collect_base;

#define CHECK(cond, ...) do { if (!(cond)) { \
    if (g_failures++ < 20) { fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} } while (0)

static size_t heap_in_use(void)
{
    malloc_statistics_t st;
    malloc_zone_statistics(NULL, &st);
    return st.size_in_use;
}

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static uint64_t fnv(uint64_t h, unsigned v) { return (h ^ v) * 1099511628211ull; }
static const char *base_name(const char *p) { return strrchr(p, '/') ? strrchr(p, '/') + 1 : p; }

static bool load_lib(struct lib *l, const char *path)
{
    unsigned char eh[64];
    l->path = path;
    l->fd = open(path, O_RDONLY);
    if (l->fd < 0 || pread(l->fd, eh, sizeof eh, 0) != (ssize_t)sizeof eh || memcmp(eh, "\x7f" "ELF", 4))
        return false;
    uint64_t phoff, shoff;
    uint16_t phnum, shnum;
    memcpy(&phoff, eh + 32, 8);
    memcpy(&shoff, eh + 40, 8);
    memcpy(&phnum, eh + 56, 2);
    memcpy(&shnum, eh + 60, 2);
    struct ph ph[64];
    if (phnum > 64 || pread(l->fd, ph, phnum * sizeof ph[0], (off_t)phoff) != (ssize_t)(phnum * sizeof ph[0]))
        return false;
    for (int i = 0; i < phnum && l->nload < 16; i++)
        if (ph[i].type == 1)
            l->load[l->nload++] = ph[i];
    if (!l->nload)
        return false;
    const struct ph *first = &l->load[0], *last = &l->load[l->nload - 1];
    l->va0 = LXRT_ALIGN_DOWN(first->va, PAGE);
    l->span = last->va + last->memsz - l->va0;
    for (int i = 0; i < l->nload; i++)
        if (l->load[i].align > l->align)
            l->align = l->load[i].align;
    l->rw_lo = LXRT_ALIGN_DOWN(last->va, PAGE) - l->va0;
    l->rw_off = LXRT_ALIGN_DOWN(last->off, PAGE);
    struct sh *sh = calloc(shnum ? shnum : 1, sizeof *sh);
    if (shnum && sh && pread(l->fd, sh, shnum * sizeof *sh, (off_t)shoff) == (ssize_t)(shnum * sizeof *sh)) {
        const struct sh *big = NULL;
        for (int i = 0; i < shnum; i++)
            if ((sh[i].flags & 4 /* SHF_EXECINSTR */) && sh[i].type == 1 && (!big || sh[i].size > big->size))
                big = &sh[i];
        for (int i = 0; big && i < l->nload; i++)
            if ((l->load[i].flags & 1) && big->off >= l->load[i].off &&
                big->off + big->size <= l->load[i].off + l->load[i].filesz) {
                l->part_off = LXRT_ALIGN_DOWN(big->off + big->size / 2, PAGE);
                l->part_len = LXRT_ALIGN_UP(l->load[i].off + l->load[i].filesz, PAGE) - l->part_off;
            }
    }
    free(sh);
    l->nwords = l->span / 4;
    l->ref = malloc(l->nwords);
    return l->ref != NULL;
}

/* dispatch.c's do_munmap_inner: the hooks see the length rounded up to 4 KiB. */
static void guest_munmap(uint64_t addr, uint64_t len)
{
    uint64_t l = g_raw_len ? len : LXRT_ALIGN_UP(len, PAGE);
    lxrt_elf_forget(addr, l);
    lxrt_memlog_file_forget(addr, l);
}

/* dispatch.c's mmap path: every file mapping is named (LNR_mmap), and an
 * executable one gets its code windows and their function tables
 * (rewrite_and_seal). */
static void guest_mmap_file(const struct lib *l, uint64_t addr, uint64_t len, uint64_t off, bool exec)
{
    lxrt_memlog_file(addr, len, off, l->fd);
    if (!exec)
        return;
    struct lxrt_range code[16];
    int n = lxrt_elf_exec_sections(l->fd, off, len, addr, code, 16);
    for (int i = 0; g_collect && i < n && g_collect->nwin < 32; i++)
        g_collect->win[g_collect->nwin++] = (struct lxrt_range){ code[i].start - g_collect_base,
                                                                 code[i].end - g_collect_base };
}

/* glibc 2.39 _dl_map_object_from_fd + _dl_map_segments, AT_PAGESZ 4096. The
 * caller has left [b - align, b + span + align) free; `lead` is how far below
 * b the over-sized reservation happened to start. */
static void guest_dlopen(const struct lib *l, uint64_t b, uint64_t cache, uint64_t lead)
{
    lxrt_memlog_file(cache, CACHE_LEN, 0, l->fd);           /* ld.so.cache */
    const struct ph *c = &l->load[0];
    guest_mmap_file(l, b, l->span, LXRT_ALIGN_DOWN(c->off, PAGE), c->flags & 1);
    if (l->align > PAGE) {
        /* _dl_map_segment: an anonymous reservation of span + align - page,
         * the file MAP_FIXED at its aligned start, the rest unmapped */
        uint64_t start = b - lead, maplen = l->span + l->align - PAGE;
        if (lead)
            guest_munmap(start, lead);
        uint64_t end = LXRT_ALIGN_UP(b + l->span, PAGE);
        if (start + maplen > end)
            guest_munmap(end, start + maplen - end);
    }
    for (int i = 1; i < l->nload; i++) {
        c = &l->load[i];
        uint64_t ms = LXRT_ALIGN_DOWN(c->va, PAGE), me = LXRT_ALIGN_UP(c->va + c->filesz, PAGE);
        if (me > ms)
            guest_mmap_file(l, b + ms - l->va0, me - ms, LXRT_ALIGN_DOWN(c->off, PAGE), c->flags & 1);
    }
    guest_munmap(cache, CACHE_LEN);                          /* _dl_unload_cache */
}

static void guest_dlclose(const struct lib *l, uint64_t b)
{
    guest_munmap(b, l->span);                                /* l_map_end - l_map_start */
}

/* Window words [first, nwords) step `step` of the span at b, against ref. */
static size_t verify(const struct lib *l, uint64_t b, size_t first, size_t step)
{
    size_t bad = 0;
    for (size_t w = first; w < l->nwords; w += step)
        if (l->ref[w] != NOT_CODE_WINDOW)
            bad += (uint8_t)lxrt_elf_in_function(b + w * 4) != l->ref[w];
    return bad;
}

/* The fault report's question: which file is at this address of the last
 * PT_LOAD, at which offset? */
static bool named_live(const struct lib *l, uint64_t b)
{
    char p[1024];
    uint64_t off;
    return lxrt_memlog_file_lookup(b + l->rw_lo + 0x800, p, sizeof p, &off) &&
           off == l->rw_off + 0x800 && strcmp(base_name(p), base_name(l->path)) == 0;
}

static bool named_at_all(const struct lib *l, uint64_t b)
{
    char p[1024];
    uint64_t off;
    return lxrt_memlog_file_lookup(b + l->rw_lo + 0x800, p, sizeof p, &off);
}

/* ns per lookup, over the window words of the span at b. */
static double lookup_ns(const struct lib *l, uint64_t b)
{
    volatile unsigned sink = 0;
    size_t n = 0;
    uint64_t t0 = now_ns();
    for (int rep = 0; n < 100000 && rep < 97; rep++)
        for (size_t w = (size_t)rep; w < l->nwords && n < 100000; w += 97)
            if (l->ref[w] != NOT_CODE_WINDOW) {
                sink += lxrt_elf_in_function(b + w * 4);
                n++;
            }
    (void)sink;
    return n ? (double)(now_ns() - t0) / (double)n : 0;
}

int main(int argc, char **argv)
{
    long cycles = 4000;
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-n") && a + 1 < argc) cycles = atol(argv[++a]);
        else if (!strcmp(argv[a], "--raw-len")) g_raw_len = true;
        else { a = argc; break; }
    }
    if (a >= argc || cycles < 100) {
        fprintf(stderr, "usage: %s [-n CYCLES>=100] [--raw-len] LIB.so...\n", argv[0]);
        return 2;
    }
    setenv("LXRT_GUEST_FAULTS", "1", 1);                     /* memlog.c reads it once */
    uint64_t stride = 0;
    for (; a < argc && g_nlibs < MAX_LIBS; a++) {
        struct lib *l = &g_libs[g_nlibs];
        if (!load_lib(l, argv[a])) {
            fprintf(stderr, "%s: not an ELF with PT_LOADs\n", argv[a]);
            return 2;
        }
        uint64_t s = LXRT_ALIGN_UP(l->span, 0x10000) + 2 * (l->align > 0x10000 ? l->align : 0x10000);
        if (s > stride) stride = s;
        g_nlibs++;
    }

    /* The reference: each library's answers in its first mapping, every
     * library at the same address in turn. */
    const uint64_t top = 0x7f0000000000ull, cache_top = 0x7e0000000000ull;
    const uint64_t ref_b = top - stride;
    const uint64_t part_b = top - 2 * stride + 0x3000;       /* inside a 16 KiB host page */
    for (int i = 0; i < g_nlibs; i++) {
        struct lib *l = &g_libs[i];
        g_collect = l;
        g_collect_base = ref_b;
        guest_dlopen(l, ref_b, cache_top, 0);
        g_collect = NULL;
        CHECK(l->nwin > 0, "%s: no code windows (no section headers?)", l->path);
        memset(l->ref, NOT_CODE_WINDOW, l->nwords);
        for (int k = 0; k < l->nwin; k++)
            for (uint64_t x = LXRT_ALIGN_UP(l->win[k].start, 4); x + 4 <= l->win[k].end; x += 4) {
                l->ref[x / 4] = lxrt_elf_in_function(ref_b + x);
                l->nfn_words += l->ref[x / 4];
            }
        l->sig = 1469598103934665603ull;
        for (size_t w = 0; w < l->nwords; w++) {
            l->nwin_words += l->ref[w] != NOT_CODE_WINDOW;
            l->sig = fnv(l->sig, l->ref[w]);
        }
        CHECK(named_live(l, ref_b), "%s: file name of the live mapping not found", l->path);
        guest_dlclose(l, ref_b);
        l->part_sig = 1469598103934665603ull;
        if (l->part_len) {
            lxrt_memlog_file(part_b, l->part_len, l->part_off, l->fd);
            struct lxrt_range code[16];
            int n = lxrt_elf_exec_sections(l->fd, l->part_off, l->part_len, part_b, code, 16);
            for (int k = 0; k < n; k++)
                for (uint64_t x = LXRT_ALIGN_UP(code[k].start, 4); x + 4 <= code[k].end; x += 4)
                    l->part_sig = fnv(l->part_sig, lxrt_elf_in_function(x));
            guest_munmap(part_b, l->part_len);
        }
        printf("sig %-22s span 0x%-7llx %2d windows, %zu of %zu words in functions  %016llx"
               "  from 0x%llx: %016llx\n", base_name(l->path), (unsigned long long)l->span,
               l->nwin, l->nfn_words, l->nwin_words, (unsigned long long)l->sig,
               (unsigned long long)l->part_off, (unsigned long long)l->part_sig);
    }

    /* After the last unmap nothing is known about any of those addresses. */
    for (int i = 0; i < g_nlibs; i++) {
        struct lib *l = &g_libs[i];
        size_t known = 0;
        for (size_t w = 0; w < l->nwords; w++)
            known += !lxrt_elf_in_function(ref_b + w * 4);
        for (uint64_t x = 0; x < l->part_len; x += 4)
            known += !lxrt_elf_in_function(part_b + x);
        CHECK(known == 0, "%s: %zu words still answered by a table after dlclose", base_name(l->path), known);
        CHECK(!named_at_all(l, ref_b), "%s: file name still listed after dlclose", base_name(l->path));
    }

    /* The cycle: each library in turn, a new address every time (the span
     * moves by stride and by up to 15 pages), checked as it goes. */
    long warm = cycles / 10;
    size_t heap0 = 0, bad = 0, stale = 0, missing = 0;
    double ns0 = 0, ns1 = 0;
    uint64_t t_cycles = now_ns();
    for (long c = 0; c < cycles; c++) {
        const struct lib *l = &g_libs[c % g_nlibs];
        uint64_t slot = (uint64_t)(c % 4096);                /* the address space is not endless */
        uint64_t lead = (uint64_t)(c % 16) * PAGE;
        uint64_t b = top - (slot + 3) * stride + lead;
        guest_dlopen(l, b, cache_top - (slot + 1) * 0x40000, lead);
        bool full = c < g_nlibs || c == cycles - 1 || c % (cycles / 8) == 0;
        bad += verify(l, b, full ? 0 : (size_t)(c % 61), full ? 1 : 61);
        missing += !named_live(l, b);
        if (c == warm)
            ns0 = lookup_ns(l, b);
        if (c == cycles - 1)
            ns1 = lookup_ns(l, b);
        guest_dlclose(l, b);
        stale += named_at_all(l, b);
        if (c == warm)
            heap0 = heap_in_use();
    }
    t_cycles = now_ns() - t_cycles;
    long growth = (long)(heap_in_use() - heap0);
    printf("moving:  %ld cycles (%.0f us each); window words answered differently: %zu\n",
           cycles, (double)t_cycles / 1e3 / (double)cycles, bad);
    printf("         file names: live mapping not found %zu, still listed after dlclose %zu\n",
           missing, stale);
    printf("         heap growth over the last %ld cycles: %ld bytes (%.1f per cycle)\n",
           cycles - warm, growth, (double)growth / (double)(cycles - warm));
    printf("         one lookup: %.0f ns at cycle %ld, %.0f ns at cycle %ld\n", ns0, warm, ns1, cycles - 1);
    CHECK(bad == 0, "%zu window words answered differently", bad);
    CHECK(missing == 0, "%zu live mappings without a file name", missing);
    CHECK(stale == 0, "%zu file names outlived their mapping", stale);
    /* The nano allocator counts a whole 16 KiB block as in use once it opens
     * one for a size class (MEASURED: +16384 twice in 18000 libusb cycles,
     * then flat; 0 under MallocNanoZone=0). One leaked file name a cycle (a
     * 32-byte slot and a path copy) is over 100 KB in the default 4000. */
    CHECK(growth < 65536, "heap grew by %ld bytes", growth);
    CHECK(ns1 < 3 * ns0 + 200, "a lookup slowed down from %.0f to %.0f ns", ns0, ns1);

    /* The same address for every library in turn. */
    bad = 0;
    for (long c = 0; c < 4 * g_nlibs; c++) {
        const struct lib *l = &g_libs[c % g_nlibs];
        uint64_t b = top - 4100 * stride;
        guest_dlopen(l, b, cache_top - 4100 * 0x40000, 0);
        bad += verify(l, b, 0, 1);
        guest_dlclose(l, b);
    }
    printf("reuse:   window words answered differently: %zu\n", bad);
    CHECK(bad == 0, "%zu window words answered differently when the address was reused", bad);

    /* What the two hooks add to every guest munmap, with every library
     * mapped (their tables and names live). */
    for (int i = 0; i < g_nlibs; i++)
        guest_dlopen(&g_libs[i], top - (4102 + (uint64_t)i) * stride, cache_top, 0);
    uint64_t t0 = now_ns();
    for (int i = 0; i < 100000; i++)
        guest_munmap(0x100000000ull + (uint64_t)(i % 64) * 0x10000, 0x4000);
    printf("munmap:  the two hooks take %.0f ns a call (%d libraries mapped)\n",
           (double)(now_ns() - t0) / 100000.0, g_nlibs);

    printf("%s\n", g_failures ? "FAILED" : "PASS");
    return g_failures ? 1 : 0;
}
