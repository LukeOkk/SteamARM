// PE images the guest's own loader mapped, for the x18 pass.
//
// Wine's loader (ntdll/unix/virtual.c) maps a DLL as: an anonymous
// reservation of SizeOfImage, the header from the file at offset 0, each
// section from the file at its raw offset (read-write), then mprotect to
// what the section asked for. That last mprotect is where rewrite.c sees the
// code (dispatch.c rewrite_and_seal, or subpage.c for 4 KiB-aligned images),
// with no file behind it to say which words are instructions -- so the x18
// pass, which runs only inside known functions, did not run and Wine ARM64
// died on its first TEB read (benchmarks/stage57-wine-arm64-native.txt).
//
// The header mapping is the hook: a private file mapping at offset 0 that
// starts with MZ is parsed (runtime/pefile.c, once per file) and becomes a
// view: the image's aarch64 code ranges at the addresses Wine gave them,
// and its function bounds registered with elfsect.c's tables, which
// lxrt_elf_in_function consults for ELF and PE alike. rewrite.c asks for
// the windows when a caller hands it none (lxrt_pe_code_windows) and counts
// x18 sites in them read-only for the W^X loop (lxrt_rewrite_count).
//
// A view ends with its image: munmap of its header or any of its code
// (lxrt_pe_forget), or a mapping from another file or none landing on its
// code (lxrt_pe_note_mmap). Wine's own section mappings come from the same
// file and keep it. Native guests only: x86 code under FEX never executes an
// aarch64 PE, and the check would cost every file mapping a pread.

#include "lxrt.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

bool lxrt_trace_on(void);
bool lxrt_wx_enabled(void);

struct pe_file {                        // parsed once per file (dev, ino, mtime, size)
    uint64_t used;                      // LRU stamp; 0 = free slot
    dev_t dev;
    ino_t ino;
    struct timespec mtime;
    off_t size;
    bool ok;                            // an image whose aarch64 code pefile.c located
    struct lxrt_pe_info info;
    char name[64];
};
#define MAX_FILES 16
static struct pe_file g_files[MAX_FILES];
static uint64_t g_stamp;

struct pe_view {                        // one mapped image
    uint64_t base, end;
    dev_t dev;
    ino_t ino;
    struct lxrt_range *code;            // absolute aarch64 code windows
    int ncode;
    int nfn;
    char name[64];
};
static struct pe_view *g_views;
static int g_nviews, g_capviews;
static _Atomic int g_any;               // a view exists: lookups take the lock only then
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(pesect_lock, g_lock)

// munmap takes this lock (lxrt_pe_forget) and may run in a guest signal
// handler nested in the host's: holders block the asynchronous signals, as
// elfsect.c does.
static void lock_nosig(sigset_t *old)
{
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGSEGV);
    sigdelset(&all, SIGILL);
    sigdelset(&all, SIGTRAP);
    pthread_sigmask(SIG_BLOCK, &all, old);
    pthread_mutex_lock(&g_lock);
}

static void unlock_nosig(const sigset_t *old)
{
    pthread_mutex_unlock(&g_lock);
    pthread_sigmask(SIG_SETMASK, old, NULL);
}

static bool enabled(void)
{
    static int on = -1;                 // LXRT_NO_PE_X18=1: diagnostic, no PE windows
    if (on < 0)
        on = getenv("LXRT_NO_PE_X18") ? 0 : 1;
    return on && lxrt_wx_enabled();
}

static void basename_of(int fd, char *out, size_t n)
{
    char path[PATH_MAX];
    const char *base = "?";
    if (fcntl(fd, F_GETPATH, path) == 0) {
        base = strrchr(path, '/');
        base = base ? base + 1 : path;
    }
    snprintf(out, n, "%s", base);
}

// The parsed image for the file on fd (caller holds the lock).
static struct pe_file *file_for(int fd, const struct stat *st)
{
    struct pe_file *slot = NULL;
    for (int i = 0; i < MAX_FILES; i++) {
        struct pe_file *f = &g_files[i];
        if (f->used && f->dev == st->st_dev && f->ino == st->st_ino && f->size == st->st_size &&
            f->mtime.tv_sec == st->st_mtimespec.tv_sec &&
            f->mtime.tv_nsec == st->st_mtimespec.tv_nsec) {
            f->used = ++g_stamp;
            return f;
        }
        if (!slot || f->used < slot->used)
            slot = f;
    }
    if (slot->used)
        lxrt_pe_info_free(&slot->info);
    memset(slot, 0, sizeof *slot);
    slot->used = ++g_stamp;
    slot->dev = st->st_dev;
    slot->ino = st->st_ino;
    slot->size = st->st_size;
    slot->mtime = st->st_mtimespec;
    basename_of(fd, slot->name, sizeof slot->name);
    char why[48] = "";
    slot->ok = lxrt_pe_parse_fd(fd, &slot->info, why, sizeof why) == 0;
    if (!slot->ok && lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] PE %s: no aarch64 code for the x18 pass (%s)\n",
                slot->name, why);
    return slot;
}

static void view_free(int i)
{
    free(g_views[i].code);
    g_views[i] = g_views[--g_nviews];
    atomic_store(&g_any, g_nviews > 0);
}

// A mapping on a view's code from another file (st), or from none: that
// code is gone. Caller holds the lock.
static void drop_code_overlap_locked(uint64_t addr, uint64_t len, const struct stat *st)
{
    for (int i = 0; i < g_nviews; i++) {
        struct pe_view *v = &g_views[i];
        if (st && st->st_dev == v->dev && st->st_ino == v->ino)
            continue;
        bool hit = false;
        for (int k = 0; k < v->ncode && !hit; k++)
            hit = addr < v->code[k].end && addr + len > v->code[k].start;
        if (!hit)
            continue;
        if (lxrt_trace_on())
            fprintf(lxrt_trace_stream(), "[lxrt] PE %s at 0x%llx: code mapped over by 0x%llx+0x%llx\n",
                    v->name, (unsigned long long)v->base, (unsigned long long)addr,
                    (unsigned long long)len);
        view_free(i);
        i--;
    }
}

static void register_view_locked(struct pe_file *f, uint64_t base, const struct stat *st)
{
    const struct lxrt_pe_info *in = &f->info;
    uint64_t span = LXRT_ALIGN_UP((uint64_t)in->size_of_image, 4096);
    if (!span || base > UINT64_MAX - span)
        return;
    uint64_t end = base + span;
    for (int i = 0; i < g_nviews; i++)   // the same place mapped again: replace
        if (g_views[i].base < end && g_views[i].end > base) {
            view_free(i);
            i--;
        }
    struct lxrt_range *code = calloc((size_t)(in->ncode ? in->ncode : 1), sizeof *code);
    if (!code)
        return;
    int ncode = 0;
    for (int k = 0; k < in->ncode; k++) {
        if (in->code[k].end > span)
            continue;
        code[ncode].start = base + in->code[k].start;
        code[ncode].end = base + in->code[k].end;
        ncode++;
    }
    if (g_nviews == g_capviews) {
        int cap = g_capviews ? g_capviews * 2 : 32;
        struct pe_view *nv = realloc(g_views, (size_t)cap * sizeof *nv);
        if (!nv) {
            free(code);
            return;
        }
        g_views = nv;
        g_capviews = cap;
    }
    struct pe_view *v = &g_views[g_nviews++];
    memset(v, 0, sizeof *v);
    v->base = base;
    v->end = end;
    v->dev = st->st_dev;
    v->ino = st->st_ino;
    v->code = code;
    v->ncode = ncode;
    snprintf(v->name, sizeof v->name, "%s", f->name);
    atomic_store(&g_any, 1);

    // Function bounds, one table per code window. An image without a
    // RUNTIME_FUNCTION table registers none: every word of its code windows
    // then counts, as for an ELF without .eh_frame.
    int nfn = 0;
    if (in->has_fn) {
        for (int k = 0; k < ncode; k++) {
            uint64_t wlo = code[k].start, whi = code[k].end;
            int m = 0;
            for (int i = 0; i < in->nfn; i++)
                if (base + in->fn[i].start < whi && base + in->fn[i].end > wlo)
                    m++;
            struct lxrt_range *r = malloc((size_t)(m ? m : 1) * sizeof *r);
            if (!r)
                continue;
            int j = 0;
            for (int i = 0; i < in->nfn && j < m; i++) {
                uint64_t s = base + in->fn[i].start, e = base + in->fn[i].end;
                if (s >= whi || e <= wlo)
                    continue;
                r[j].start = s < wlo ? wlo : s;
                r[j].end = e > whi ? whi : e;
                j++;
            }
            lxrt_fn_register(wlo, whi, r, j);   // takes r
            nfn += j;
        }
    }
    v->nfn = nfn;
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] PE %s at 0x%llx: %d aarch64 code window%s (%s, machine %04x), "
                "%d functions%s\n", v->name, (unsigned long long)base, ncode, ncode == 1 ? "" : "s",
                in->chpe ? "ARM64EC code map" : "executable sections", in->machine, nfn,
                in->has_fn ? "" : " (no unwind table: every word counts)");
}

// dispatch.c: a mapping the guest made. fd < 0 for an anonymous one.
void lxrt_pe_note_mmap(int fd, uint64_t off, uint64_t len, uint64_t addr)
{
    if (!enabled() || !len || addr > UINT64_MAX - len)
        return;
    bool header = false;
    if (fd >= 0 && off == 0 && len >= 0x200) {
        uint8_t mz[2];
        header = pread(fd, mz, 2, 0) == 2 && mz[0] == 'M' && mz[1] == 'Z';
    }
    if (!header && !atomic_load(&g_any))
        return;
    struct stat st;
    bool have_st = fd >= 0 && fstat(fd, &st) == 0;
    sigset_t old;
    lock_nosig(&old);
    if (!header || !have_st) {
        drop_code_overlap_locked(addr, len, have_st ? &st : NULL);
    } else {
        struct pe_file *f = file_for(fd, &st);
        if (f->ok)
            register_view_locked(f, addr, &st);
        else
            drop_code_overlap_locked(addr, len, NULL);
    }
    unlock_nosig(&old);
}

// The guest unmapped [addr, addr+len): a view whose header or code it
// covers is gone.
void lxrt_pe_forget(uint64_t addr, uint64_t len)
{
    if (!len || !atomic_load(&g_any))
        return;
    uint64_t end = addr + len;
    sigset_t old;
    lock_nosig(&old);
    for (int i = 0; i < g_nviews; i++) {
        struct pe_view *v = &g_views[i];
        bool hit = v->base >= addr && v->base < end;
        for (int k = 0; k < v->ncode && !hit; k++)
            hit = addr < v->code[k].end && end > v->code[k].start;
        if (hit) {
            view_free(i);
            i--;
        }
    }
    unlock_nosig(&old);
}

// The aarch64 code windows of mapped PE images inside [start, end), clipped.
int lxrt_pe_code_windows(uint64_t start, uint64_t end, struct lxrt_range *out, int max)
{
    if (end <= start || max <= 0 || !atomic_load(&g_any))
        return 0;
    int n = 0;
    sigset_t old;
    lock_nosig(&old);
    for (int i = 0; i < g_nviews && n < max; i++) {
        const struct pe_view *v = &g_views[i];
        if (v->end <= start || v->base >= end)
            continue;
        for (int k = 0; k < v->ncode && n < max; k++) {
            uint64_t s = v->code[k].start, e = v->code[k].end;
            if (e <= start || s >= end)
                continue;
            out[n].start = s < start ? start : s;
            out[n].end = e > end ? end : e;
            n++;
        }
    }
    unlock_nosig(&old);
    return n;
}

bool lxrt_pe_intersects(uint64_t start, uint64_t end)
{
    struct lxrt_range r;
    return lxrt_pe_code_windows(start, end, &r, 1) > 0;
}
